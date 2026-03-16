ROOT ?= .

CXX ?= ccache clang++
CXXFLAGS := -std=c++20 -g
OUT_DIR := $(ROOT)/build
INCLUDE_PATHS = -I $(ROOT)/src -I $(ROOT)/build
LEX_FILE = grammar/lexer.lex
YACC_FILE = $(shell find $(ROOT)/grammar -name "*.yacc")
GENERATED_PARSER_SOURCES = $(OUT_DIR)/parser.cc $(OUT_DIR)/parser.hh $(OUT_DIR)/location.hh
GENERATED_LEXER_SOURCE = $(OUT_DIR)/scanner.cc
GENERATED_PARSER_HEADERS = $(OUT_DIR)/parser.hh $(OUT_DIR)/location.hh
PARSER_SUPPORT_SOURCES = $(ROOT)/src/main.cc \
	$(ROOT)/src/lona/scan/driver.cc \
	$(ROOT)/src/lona/ast/astnode.cc \
	$(ROOT)/src/lona/ast/astnode_toJson.cc \
	$(ROOT)/src/lona/ast/astnode_toCFG.cc \
	$(ROOT)/src/lona/ast/token.cc \
	$(ROOT)/src/lona/util/cfg.cc \
	$(ROOT)/src/lona/util/string.cc
SOURCE_FILES = $(shell find $(ROOT)/src -name "*.cc") $(GENERATED_LEXER_SOURCE) $(OUT_DIR)/parser.cc
FRONTEND_SOURCE_FILES = $(PARSER_SUPPORT_SOURCES) $(GENERATED_LEXER_SOURCE) $(OUT_DIR)/parser.cc
LIBRARY_SOURCE_FILES = $(filter-out $(ROOT)/src/main.cc,$(SOURCE_FILES))
INCREMENTAL_SMOKE_SOURCES = $(ROOT)/tests/incremental_smoke.cc

LIBS = $(shell llvm-config-18 --libs core native asmparser linker)

LD_FLAGS = $(shell llvm-config-18 --ldflags)
CXXFLAGS += $(shell llvm-config-18 --cppflags)

OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(SOURCE_FILES))
FRONTEND_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(FRONTEND_SOURCE_FILES))
LIBRARY_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(LIBRARY_SOURCE_FILES))
INCREMENTAL_SMOKE_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(INCREMENTAL_SMOKE_SOURCES))
target = $(OUT_DIR)/lona
frontend_target = $(OUT_DIR)/lona-frontend
incremental_smoke_target = $(OUT_DIR)/incremental_smoke

# require llvm-18
ifeq ($(shell llvm-config-18 --version),)
$(error "llvm-18 not found")
endif
# require bison
ifeq ($(shell bison --version),)
$(error "bison not found")
endif
# require flex
ifeq ($(shell flex --version),)
$(error "flex not found")
endif

.PHONY: clean format default frontend acceptance test bench_smoke incremental_smoke

default:
	mkdir -p build
	bear --output build/compile_commands.json -- $(MAKE) -k all -j8

all: $(target)

frontend: $(frontend_target)

acceptance: $(target)
	bash $(ROOT)/scripts/acceptance.sh

test: acceptance bench_smoke incremental_smoke

bench_smoke: $(target)
	bash $(ROOT)/scripts/benchmark_smoke.sh

incremental_smoke: $(incremental_smoke_target)
	$(incremental_smoke_target)

$(target): $(OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(frontend_target): $(FRONTEND_OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(incremental_smoke_target): $(LIBRARY_OBJECTS) $(INCREMENTAL_SMOKE_OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(OUT_DIR)/%.d: %.cc | $(GENERATED_PARSER_HEADERS)
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -MM $< -MT $(@:.d=.o) > $@

$(OUT_DIR)/%.o: %.cc | $(GENERATED_PARSER_HEADERS)
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -c $< -o $@

ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
-include $(OBJECTS:.o=.d)
-include $(FRONTEND_OBJECTS:.o=.d)
-include $(INCREMENTAL_SMOKE_OBJECTS:.o=.d)
endif

$(GENERATED_LEXER_SOURCE): $(LEX_FILE) $(OUT_DIR)/parser.hh
	mkdir -p $(OUT_DIR)
	echo "Generating scanner.cc"
	flex -o $(GENERATED_LEXER_SOURCE) $(LEX_FILE)

$(GENERATED_PARSER_SOURCES) &: $(YACC_FILE) $(ROOT)/scripts/multi_yacc.py
	mkdir -p $(OUT_DIR)
	python3 scripts/multi_yacc.py
	echo "Generating parser.cc"
	bison -d -o $(OUT_DIR)/parser.cc $(OUT_DIR)/gen.yacc -Wcounterexamples -rall --report-file=report.txt

gram_check: $(GENERATED_LEXER_SOURCE) $(OUT_DIR)/parser.cc

clean:
	rm -rf $(OUT_DIR)

format:
	clang-format-18 -i $(shell find $(ROOT)/src -name "*.cc") $(shell find $(ROOT)/src -name "*.hh")

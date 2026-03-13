ROOT ?= .

CXX ?= ccache clang++
CXXFLAGS := -std=c++20 -g
OUT_DIR := $(ROOT)/build
INCLUDE_PATHS = -I $(ROOT)/src -I $(ROOT)/build
LEX_FILE = grammar/lexer.lex
YACC_FILE = $(shell find $(ROOT)/grammar -name "*.yacc")
PARSER_SUPPORT_SOURCES = $(ROOT)/src/main.cc \
	$(ROOT)/src/lona/scan/driver.cc \
	$(ROOT)/src/lona/ast/astnode.cc \
	$(ROOT)/src/lona/ast/astnode_toJson.cc \
	$(ROOT)/src/lona/ast/astnode_toCFG.cc \
	$(ROOT)/src/lona/ast/token.cc \
	$(ROOT)/src/lona/util/cfg.cc \
	$(ROOT)/src/lona/util/string.cc
SOURCE_FILES = $(shell find $(ROOT)/src -name "*.cc") build/scanner.cc build/parser.cc
FRONTEND_SOURCE_FILES = $(PARSER_SUPPORT_SOURCES) build/scanner.cc build/parser.cc

LIBS = $(shell llvm-config-18 --libs core native)

LD_FLAGS = $(shell llvm-config-18 --ldflags)
CXXFLAGS += $(shell llvm-config-18 --cppflags)

OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(SOURCE_FILES))
FRONTEND_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(FRONTEND_SOURCE_FILES))
target = $(OUT_DIR)/lona
frontend_target = $(OUT_DIR)/lona-frontend

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

.PHONY: clean format default frontend

default:
	mkdir -p build
	bear --output build/compile_commands.json -- $(MAKE) -k all -j8

all: $(target)

frontend: $(frontend_target)

$(target): $(OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(frontend_target): $(FRONTEND_OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(OUT_DIR)/%.d: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -MM $< -MT $(@:.d=.o) > $@

$(OUT_DIR)/%.o: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -c $< -o $@

-include $(OBJECTS:.o=.d)
-include $(FRONTEND_OBJECTS:.o=.d)

build/scanner.cc: $(LEX_FILE)
	mkdir -p build
	echo "Generating scanner.cc"
	flex -o build/scanner.cc $(LEX_FILE)

build/parser.cc: $(YACC_FILE)
	mkdir -p build
	python3 scripts/multi_yacc.py
	echo "Generating parser.cc"
	bison -d -o build/parser.cc build/gen.yacc -Wcounterexamples -rall --report-file=report.txt

gram_check: build/scanner.cc build/parser.cc

clean:
	rm -rf $(OUT_DIR)

format:
	clang-format-18 -i $(shell find $(ROOT)/src -name "*.cc") $(shell find $(ROOT)/src -name "*.hh")

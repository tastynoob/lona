ROOT ?= .
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/lona
RUNTIMEDIR ?= $(DATADIR)/runtime/linux_x86_64
INSTALL ?= install
PYTHON ?= python3

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
SESSION_RUNNER_SOURCES = $(ROOT)/tests/session_runner.cc

LIBS = $(shell llvm-config-18 --libs core native asmparser linker)

LD_FLAGS = $(shell llvm-config-18 --ldflags)
CXXFLAGS += $(shell llvm-config-18 --cppflags)

OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(SOURCE_FILES))
FRONTEND_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(FRONTEND_SOURCE_FILES))
LIBRARY_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(LIBRARY_SOURCE_FILES))
SESSION_RUNNER_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(SESSION_RUNNER_SOURCES))
target = $(OUT_DIR)/lona-ir
frontend_target = $(OUT_DIR)/lona-ir-frontend
session_runner_target = $(OUT_DIR)/lona-session-runner

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

.PHONY: clean format default frontend acceptance test bench_smoke incremental_smoke native_smoke hosted_smoke install uninstall

default:
	mkdir -p build
	bear --output build/compile_commands.json -- $(MAKE) -k all -j8

all: $(target)

frontend: $(frontend_target)

acceptance: $(target)
	bash $(ROOT)/scripts/acceptance.sh

test: acceptance bench_smoke incremental_smoke hosted_smoke native_smoke

bench_smoke: $(target)
	bash $(ROOT)/scripts/benchmark_smoke.sh

incremental_smoke: $(session_runner_target)
	$(PYTHON) $(ROOT)/tests/incremental_smoke.py --runner $(session_runner_target)

native_smoke: $(target)
	bash $(ROOT)/scripts/native_smoke.sh

hosted_smoke: $(target)
	bash $(ROOT)/scripts/hosted_smoke.sh

install: $(target)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -d $(DESTDIR)$(RUNTIMEDIR)
	$(INSTALL) -m 755 $(target) $(DESTDIR)$(BINDIR)/lona-ir
	$(INSTALL) -m 755 $(ROOT)/scripts/lac.sh $(DESTDIR)$(BINDIR)/lac
	$(INSTALL) -m 755 $(ROOT)/scripts/lac-native.sh $(DESTDIR)$(BINDIR)/lac-native
	$(INSTALL) -m 644 $(ROOT)/runtime/linux_x86_64/lona_start.S $(DESTDIR)$(RUNTIMEDIR)/lona_start.S
	$(INSTALL) -m 644 $(ROOT)/runtime/linux_x86_64/lona.ld $(DESTDIR)$(RUNTIMEDIR)/lona.ld

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lona-ir
	rm -f $(DESTDIR)$(BINDIR)/lac
	rm -f $(DESTDIR)$(BINDIR)/lac-native
	rm -f $(DESTDIR)$(RUNTIMEDIR)/lona_start.S
	rm -f $(DESTDIR)$(RUNTIMEDIR)/lona.ld

$(target): $(OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(frontend_target): $(FRONTEND_OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(session_runner_target): $(LIBRARY_OBJECTS) $(SESSION_RUNNER_OBJECTS)
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
-include $(SESSION_RUNNER_OBJECTS:.o=.d)
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

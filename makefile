ROOT ?= .
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install
PYTHON ?= python3
CLANG ?= clang-18

CXX ?= ccache clang++
CXXFLAGS := -std=c++20 -g
OUT_DIR := build
ASAN_OUT_DIR ?= build/asan
LONA_LANGUAGE_VERSION := 0.1 beta
INCLUDE_PATHS = -I $(ROOT)/src -I $(ROOT)/$(OUT_DIR)
LEX_FILE = grammar/lexer.lex
YACC_FILE = $(shell find $(ROOT)/grammar -name "*.yacc")
GENERATED_PARSER_SOURCES = $(OUT_DIR)/parser.cc $(OUT_DIR)/parser.hh $(OUT_DIR)/location.hh
GENERATED_LEXER_SOURCE = $(OUT_DIR)/scanner.cc
GENERATED_VERSION_SOURCE = $(OUT_DIR)/version.cc
GENERATED_VERSION_STAMP = $(OUT_DIR)/version.stamp
GENERATED_PARSER_STAMP = $(OUT_DIR)/parser.stamp
GENERATED_PARSER_HEADERS = $(OUT_DIR)/parser.hh $(OUT_DIR)/location.hh
GENERATED_SUPPORT_SOURCES = $(GENERATED_LEXER_SOURCE) $(OUT_DIR)/parser.cc $(GENERATED_VERSION_SOURCE)
PARSER_SUPPORT_SOURCES = $(ROOT)/src/main.cc \
	$(ROOT)/src/lona/scan/driver.cc \
	$(ROOT)/src/lona/ast/astnode.cc \
	$(ROOT)/src/lona/ast/astnode_toJson.cc \
	$(ROOT)/src/lona/ast/astnode_toCFG.cc \
	$(ROOT)/src/lona/ast/token.cc \
	$(ROOT)/src/lona/util/cfg.cc \
	$(ROOT)/src/lona/util/string.cc
SOURCE_FILES = $(shell find $(ROOT)/src -name "*.cc") $(GENERATED_SUPPORT_SOURCES)
FRONTEND_SOURCE_FILES = $(PARSER_SUPPORT_SOURCES) $(GENERATED_SUPPORT_SOURCES)
MAIN_SOURCE = $(ROOT)/src/main.cc
QUERY_MAIN_SOURCE = $(ROOT)/src/tooling/main.cc
LIBRARY_SOURCE_FILES = $(filter-out $(MAIN_SOURCE) $(QUERY_MAIN_SOURCE),$(SOURCE_FILES))
CORE_LIBRARY_SOURCE_FILES = $(LIBRARY_SOURCE_FILES)
SESSION_RUNNER_SOURCES = $(ROOT)/tests/session_runner.cc
VERSION_SOURCE_DEPS = $(ROOT)/makefile \
	$(ROOT)/src/lona/version.hh \
	$(wildcard $(ROOT)/.git/HEAD $(ROOT)/.git/refs/heads/* $(ROOT)/.git/packed-refs)

LIBS = $(shell llvm-config-18 --libs core native asmparser linker)

LD_FLAGS = $(shell llvm-config-18 --ldflags)
CXXFLAGS += $(shell llvm-config-18 --cppflags)
ASAN_CXXFLAGS := -std=c++20 -g -fsanitize=address -fno-omit-frame-pointer $(shell llvm-config-18 --cppflags)
ASAN_LD_FLAGS := $(shell llvm-config-18 --ldflags) -fsanitize=address

MAIN_OBJECT = $(patsubst %.cc, $(OUT_DIR)/%.o, $(MAIN_SOURCE))
QUERY_MAIN_OBJECT = $(patsubst %.cc, $(OUT_DIR)/%.o, $(QUERY_MAIN_SOURCE))
OBJECTS = $(CORE_LIBRARY_OBJECTS) $(MAIN_OBJECT)
FRONTEND_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(FRONTEND_SOURCE_FILES))
LIBRARY_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(LIBRARY_SOURCE_FILES))
CORE_LIBRARY_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(CORE_LIBRARY_SOURCE_FILES))
QUERY_OBJECTS = $(CORE_LIBRARY_OBJECTS) $(QUERY_MAIN_OBJECT)
SESSION_RUNNER_OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(SESSION_RUNNER_SOURCES))
target = $(OUT_DIR)/lona-ir
frontend_target = $(OUT_DIR)/lona-ir-frontend
session_runner_target = $(OUT_DIR)/lona-session-runner
query_target = $(OUT_DIR)/lona-query

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

.PHONY: clean format default gram_check frontend query query_memcheck acceptance smoke test perf incremental_smoke template_random ai_test install uninstall

default:
	mkdir -p build
	bear --output build/compile_commands.json -- $(MAKE) -k all -j8

all: $(target) query

frontend: $(frontend_target)

query: $(query_target)

query_memcheck:
	$(MAKE) query OUT_DIR=$(ASAN_OUT_DIR) CXXFLAGS='$(ASAN_CXXFLAGS)' LD_FLAGS='$(ASAN_LD_FLAGS)'
	LONA_QUERY_MEMCHECK_BIN=$(ASAN_OUT_DIR)/lona-query ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 $(PYTHON) -m pytest -q $(ROOT)/tests/test_query_memory.py

acceptance: $(target)
	$(PYTHON) -m pytest -q $(ROOT)/tests/acceptance

smoke: $(target) $(query_target)
	$(PYTHON) -m pytest -q -s $(ROOT)/tests/smoke

test: acceptance smoke incremental_smoke template_random

perf: $(target)
	$(PYTHON) $(ROOT)/tests/perf/profile_large_case.py --compiler $(target)

incremental_smoke: $(session_runner_target)
	$(PYTHON) $(ROOT)/tests/incremental_smoke.py --runner $(session_runner_target)

template_random: $(target)
	$(PYTHON) $(ROOT)/tests/template_random.py --compiler $(target) --clang $(CLANG)

ai_test:
	$(PYTHON) $(ROOT)/tests/test_agent.py

install: $(target) $(query_target)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(target) $(DESTDIR)$(BINDIR)/lona-ir
	$(INSTALL) -m 755 $(query_target) $(DESTDIR)$(BINDIR)/lona-query
	$(INSTALL) -m 755 $(ROOT)/scripts/lac.sh $(DESTDIR)$(BINDIR)/lac
	$(INSTALL) -m 755 $(ROOT)/scripts/lac-native.sh $(DESTDIR)$(BINDIR)/lac-native

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lona-ir
	rm -f $(DESTDIR)$(BINDIR)/lona-query
	rm -f $(DESTDIR)$(BINDIR)/lac
	rm -f $(DESTDIR)$(BINDIR)/lac-native

$(target): $(OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(query_target): $(QUERY_OBJECTS)
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

ifeq ($(filter clean query_memcheck,$(MAKECMDGOALS)),)
-include $(OBJECTS:.o=.d)
-include $(QUERY_OBJECTS:.o=.d)
-include $(FRONTEND_OBJECTS:.o=.d)
-include $(SESSION_RUNNER_OBJECTS:.o=.d)
endif

$(GENERATED_LEXER_SOURCE): $(LEX_FILE) $(OUT_DIR)/parser.hh
	mkdir -p $(OUT_DIR)
	echo "Generating scanner.cc"
	flex -o $(GENERATED_LEXER_SOURCE) $(LEX_FILE)

$(GENERATED_VERSION_SOURCE) $(GENERATED_VERSION_STAMP) &: $(VERSION_SOURCE_DEPS)
	mkdir -p $(OUT_DIR)
	REVISION=$$(git -C $(ROOT) rev-parse --short=12 HEAD 2>/dev/null || echo unknown); \
	{ \
		printf '%s\n' '#include "lona/version.hh"'; \
		printf '%s\n' '#include <string_view>'; \
		printf '\n'; \
		printf '%s\n' 'namespace lona {'; \
		printf '%s\n' 'namespace {'; \
		printf 'constexpr std::string_view kLanguageVersion = "%s";\n' "$(LONA_LANGUAGE_VERSION)"; \
		printf 'constexpr std::string_view kRevisionVersion = "%s";\n' "$$REVISION"; \
		printf 'constexpr std::string_view kCompilerVersion = "%s + %s";\n' "$(LONA_LANGUAGE_VERSION)" "$$REVISION"; \
		printf '%s\n' '}'; \
		printf '\n'; \
		printf '%s\n' 'std::string_view languageVersion() { return kLanguageVersion; }'; \
		printf '%s\n' 'std::string_view revisionVersion() { return kRevisionVersion; }'; \
		printf '%s\n' 'std::string_view versionString() { return kCompilerVersion; }'; \
		printf '%s\n' '}  // namespace lona'; \
	} > $(GENERATED_VERSION_SOURCE).tmp
	cmp -s $(GENERATED_VERSION_SOURCE).tmp $(GENERATED_VERSION_SOURCE) || mv $(GENERATED_VERSION_SOURCE).tmp $(GENERATED_VERSION_SOURCE)
	rm -f $(GENERATED_VERSION_SOURCE).tmp
	touch $(GENERATED_VERSION_STAMP)

$(GENERATED_PARSER_STAMP): $(YACC_FILE) $(ROOT)/scripts/multi_yacc.py
	mkdir -p $(OUT_DIR)
	LONA_BUILD_DIR=$(OUT_DIR) python3 scripts/multi_yacc.py
	echo "Generating parser.cc"
	bison -d -o $(OUT_DIR)/parser.cc $(OUT_DIR)/gen.yacc -Wcounterexamples -Werror -rall --report-file=report.txt
	touch $(GENERATED_PARSER_STAMP)

$(GENERATED_PARSER_SOURCES): $(GENERATED_PARSER_STAMP)
	@:

gram_check:
	mkdir -p $(OUT_DIR)
	flex -o $(GENERATED_LEXER_SOURCE) $(LEX_FILE)
	LONA_BUILD_DIR=$(OUT_DIR) python3 scripts/multi_yacc.py
	echo "Generating parser.cc"
	bison -d -o $(OUT_DIR)/parser.cc $(OUT_DIR)/gen.yacc -Wcounterexamples -Werror -rall --report-file=report.txt

clean:
	rm -rf $(OUT_DIR)

format:
	clang-format-18 -i $(shell find $(ROOT)/src -name "*.cc") $(shell find $(ROOT)/src -name "*.hh")

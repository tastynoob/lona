ROOT ?= .

CXX ?= ccache clang++
CXXFLAGS := -std=c++20 -g
OUT_DIR := $(ROOT)/build
INCLUDE_PATHS = -I $(ROOT)/src -I $(ROOT)/build
LEX_FILE = grammar/lexer.lex
YACC_FILE = $(shell find $(ROOT)/grammar -name "*.yacc")
SOURCE_FILES = $(shell find $(ROOT)/src -name "*.cc") build/scanner.cc build/parser.cc

LIBS = $(shell llvm-config-18 --libs core native)

LD_FLAGS = $(shell llvm-config-18 --ldflags)
CXXFLAGS += $(shell llvm-config-18 --cppflags)

OBJECTS = $(patsubst %.cc, $(OUT_DIR)/%.o, $(SOURCE_FILES))
target = $(OUT_DIR)/lona

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

.PHONY: clean format default

default:
	mkdir -p build
	bear --output build/compile_commands.json -- $(MAKE) -k all -j8

all: $(target)

$(target): $(OBJECTS)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LIBS) $(LD_FLAGS) -o $@

$(OUT_DIR)/%.d: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -MM $< -MT $(@:.d=.o) > $@

$(OUT_DIR)/%.o: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -c $< -o $@

-include $(OBJECTS:.o=.d)

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


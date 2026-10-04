# Vayu build
#
#   make            -> build compiler (vyc) + runtime (libvyrt.a)
#   make test       -> build everything and run the test suite
#   make clean

CXX      ?= clang++
CC       ?= clang
AR       ?= ar
LLVM_CONFIG ?= llvm-config

BUILD    := build
.DEFAULT_GOAL := all
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-rtti \
            -I compiler -I runtime/include -MMD -MP
LDFLAGS  :=

# --- runtime ---------------------------------------------------------------
RT_SRC   := $(wildcard runtime/src/*.c)
RT_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(RT_SRC))
RT_LIB   := $(BUILD)/libvyrt.a

RT_INC   := $(shell $(LLVM_CONFIG) --includedir)
# -ffunction-sections/-fdata-sections let the linker's --gc-sections drop the
# runtime parts a program never calls (spec sections 4 and 29).
RT_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC \
             -ffunction-sections -fdata-sections -MMD -MP \
             -I runtime/include $(shell $(LLVM_CONFIG) --cflags)
RT_LDLIBS := -lcurl -lssl -lcrypto -lz -lm -lpthread -ldl

# --- compiler --------------------------------------------------------------
CC_CPP   := $(wildcard compiler/src/*.cpp) $(wildcard compiler/lexer/*.cpp) \
            $(wildcard compiler/ast/*.cpp)   $(wildcard compiler/parser/*.cpp) \
            $(wildcard compiler/sema/*.cpp)  $(wildcard compiler/backend/*.cpp) \
            $(wildcard compiler/tools/*.cpp)
CC_OBJ   := $(patsubst %.cpp,$(BUILD)/%.o,$(CC_CPP))

# Header dependency tracking: without this, editing runtime/include/*.h left
# stale objects that still referenced the old symbols (undefined refs at link).
-include $(CC_OBJ:.o=.d) $(RT_OBJ:.o=.d)
VYC      := $(BUILD)/vyc

.PHONY: all test clean runtime examples chatbot install uninstall

all: $(VYC) $(RT_LIB)

# --- compile rules ---------------------------------------------------------
# The runtime is C and must go through the C compiler; the explicit pattern
# rules (rather than one pattern handling both) keep that guaranteed.
$(BUILD)/runtime/src/%.o: runtime/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(BUILD)/compiler/%.o: compiler/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/compiler/src/%.o: compiler/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

runtime: $(RT_LIB)

$(RT_LIB): $(RT_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	@echo "  AR    $@"

$(VYC): $(CC_OBJ) $(RT_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CC_OBJ) $(RT_LIB) $(RT_LDLIBS) -o $@
	@echo "  LINK  $@"

# --- examples ---------------------------------------------------------------
# Everything below runs offline with no configuration. The chatbot needs an API
# key and a network, so it is filtered out rather than breaking `make examples`
# for someone who just cloned the repo -- `make chatbot` runs it against the
# bundled mock server instead.
EXAMPLES := $(filter-out examples/chatbot/chatbot.vy, \
              $(wildcard examples/*/*.vy examples/*.vy))

examples: $(VYC)
	@set -e; for f in $(EXAMPLES); do \
	  echo "  ---- running $$f"; \
	  $(VYC) run "$$f" < /dev/null; \
	done
	@echo "  ---- skipped examples/chatbot/chatbot.vy (it needs AI_API_KEY)"
	@echo "       run it offline against the mock server: make chatbot"

# A full round trip through examples/chatbot: start the mock OpenAI-compatible
# server, send the chatbot one message, then shut the server down. Override the
# port with `make chatbot MOCK_PORT=9000` if 8642 is taken.
MOCK_PORT ?= 8642

chatbot: $(VYC)
	@command -v python3 >/dev/null 2>&1 || { echo "python3 is required"; exit 1; }
	@python3 examples/chatbot/mock_server.py $(MOCK_PORT) & mock=$$!; \
	 trap 'kill $$mock 2>/dev/null' EXIT; \
	 sleep 1; \
	 printf 'hello from make\nexit\n' | \
	   AI_API_KEY=test AI_API_URL=http://127.0.0.1:$(MOCK_PORT)/v1/chat AI_MODEL=mock-model \
	   $(VYC) run examples/chatbot/chatbot.vy

# --- runtime self-test ----------------------------------------------------
RT_TEST := $(BUILD)/runtime_test

$(RT_TEST): tests/runtime_test.c $(RT_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(RT_CFLAGS) $< $(RT_LIB) $(RT_LDLIBS) -o $@

# --- language test suite --------------------------------------------------
test: $(VYC) $(RT_TEST)
	@$(RT_TEST)
	@bash tests/interp.sh

# --- install --------------------------------------------------------------
# vyc resolves libvyrt.a and the runtime headers relative to its own path (see
# runtime_archive_path in compiler/src/driver.cpp), so the resulting prefix is
# self-contained: bin/vyc + lib/libvyrt.a + include/vyrt.h. Nothing else has to
# be configured, and the prefix can be moved.
#
#   make install                      -> /usr/local (needs root)
#   make install PREFIX=$HOME/.local  -> user prefix
#   make install DESTDIR=/tmp/pkg     -> staged package tree
PREFIX  ?= /usr/local
DESTDIR ?=

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 755 $(VYC) $(DESTDIR)$(PREFIX)/bin/vyc
	install -m 644 $(RT_LIB) $(DESTDIR)$(PREFIX)/lib/libvyrt.a
	install -m 644 runtime/include/vyrt.h runtime/include/vyrt_helpers.h $(DESTDIR)$(PREFIX)/include/
	@echo "installed vyc into $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/vyc \
	      $(DESTDIR)$(PREFIX)/lib/libvyrt.a \
	      $(DESTDIR)$(PREFIX)/include/vyrt.h \
	      $(DESTDIR)$(PREFIX)/include/vyrt_helpers.h

clean:
	rm -rf $(BUILD)
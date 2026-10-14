# Ovyth build
#
#   make            -> build compiler (ovc) + runtime (libovrt.a)
#   make lsp        -> build the VS Code language server (editors/vscode/bin/ovyth-lsp)
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
RT_LIB   := $(BUILD)/libovrt.a

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

# --- language server (LSP) --------------------------------------------------
# The editor server is the compiler's front end behind a JSON-RPC loop, so it
# links only the lexer/parser/ast/sema objects -- no runtime archive, no
# backend, no libcurl. Built on demand: `make lsp`.
LSP_SRC  := compiler/lsp/lsp.cpp
FE_SRC   := $(wildcard compiler/lexer/*.cpp) $(wildcard compiler/parser/*.cpp) \
            $(wildcard compiler/ast/*.cpp)   $(wildcard compiler/sema/*.cpp) $(LSP_SRC)
LSP_OBJ  := $(patsubst %.cpp,$(BUILD)/%.o,$(FE_SRC))
LSP_BIN  := editors/vscode/bin/ovyth-lsp

# Header dependency tracking: without this, editing runtime/include/*.h left
# stale objects that still referenced the old symbols (undefined refs at link).
-include $(CC_OBJ:.o=.d) $(RT_OBJ:.o=.d) $(LSP_OBJ:.o=.d)
OVC      := $(BUILD)/ovc

.PHONY: all test clean runtime examples chatbot chatbot-rag agent agent-tui install uninstall lsp

all: $(OVC) $(RT_LIB)

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

$(OVC): $(CC_OBJ) $(RT_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CC_OBJ) $(RT_LIB) $(RT_LDLIBS) -o $@
	@echo "  LINK  $@"

$(LSP_BIN): $(LSP_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -pthread $(LSP_OBJ) -o $@
	@echo "  LINK  $@"

lsp: $(LSP_BIN)

# --- examples ---------------------------------------------------------------
# Everything below runs offline with no configuration. The chatbot needs an API
# key and a network, so it is filtered out rather than breaking `make examples`
# for someone who just cloned the repo -- `make chatbot` runs it against the
# bundled mock server instead.
#
# Two examples are applications on the published packages (rag_pipeline on
# `rag`, chatbot on `ai`), so the packages are installed first. That is the
# point: an example shows the module being used, not re-implemented.
EXAMPLES := $(filter-out examples/chatbot/chatbot.ov, \
              $(wildcard examples/*/*.ov examples/*.ov))

# "Already installed" is the goal met, not a failure, so it is not reported as
# one -- only a real failure (no network, say) gets a note.
modules:
	@set -e; for p in ai rag; do \
	  out="$$($(OVC) pkg install $$p 2>&1)" || { \
	    case "$$out" in \
	      *"already installed"*) ;; \
	      *) echo "  ---- note: could not install '$$p': $$out" ;; \
	    esac; \
	  }; \
	done

examples: $(OVC) modules
	@set -e; for f in $(EXAMPLES); do \
	  echo "  ---- running $$f"; \
	  $(OVC) run "$$f" < /dev/null; \
	done
	@echo "  ---- skipped examples/chatbot/chatbot.ov (it needs AI_API_KEY)"
	@echo "       run it offline against the mock server: make chatbot"

# A full round trip through examples/chatbot: start the mock OpenAI-compatible
# server, send the chatbot one message, then shut the server down. Override the
# port with `make chatbot MOCK_PORT=9000` if 8642 is taken.
MOCK_PORT ?= 8642

chatbot: $(OVC)
	@command -v python3 >/dev/null 2>&1 || { echo "python3 is required"; exit 1; }
	@python3 examples/chatbot/mock_server.py $(MOCK_PORT) & mock=$$!; \
	 trap 'kill $$mock 2>/dev/null' EXIT; \
	 sleep 1; \
	 printf 'hello from make\nexit\n' | \
	   AI_API_KEY=test AI_API_URL=http://127.0.0.1:$(MOCK_PORT)/v1/chat/completions AI_MODEL=mock-model \
	   $(OVC) run examples/chatbot/chatbot.ov

# The autonomous agent, in demo mode: scripted model, real commands, real
# observation. With AI_API_KEY set it runs against a live endpoint instead.
agent-tui: $(OVC)
	@$(OVC) agent < /dev/null

agent: $(OVC)
	@AI_API_KEY= AGENT_STEPS=6 $(OVC) run examples/agent/agent.ov < /dev/null

# The integrated chatbot: RAG retrieval + persistent memory, fully offline.
# Persistence goes to examples/chatbot/chatbot_rag_memory.json, so a second run
# resumes the conversation. Point it at a real endpoint with AI_API_KEY /
# AI_API_URL / AI_MODEL; with AI_API_KEY unset it answers from the local
# responder so the retrieval, memory and token-budget paths still run.
chatbot-rag: $(OVC)
	@printf 'what is RAG?\nhow does arena allocation work?\nexit\n' | \
	   AI_API_KEY=$(AI_API_KEY) AI_API_URL=$(AI_API_URL) AI_MODEL=$(AI_MODEL) \
	   CHATBOT_MEMORY_PATH=examples/chatbot/chatbot_rag_memory.json \
	   $(OVC) run examples/chatbot/chatbot_rag.ov

# --- runtime self-test ----------------------------------------------------
RT_TEST := $(BUILD)/runtime_test

$(RT_TEST): tests/runtime_test.c $(RT_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(RT_CFLAGS) $< $(RT_LIB) $(RT_LDLIBS) -o $@

# --- language test suite --------------------------------------------------
test: $(OVC) $(RT_TEST)
	@$(RT_TEST)
	@bash tests/interp.sh
	@bash tests/pkg.sh
	@bash tests/ui.sh
	@bash tests/net.sh
	@bash tests/memory.sh

# --- install --------------------------------------------------------------
# ovc resolves libovrt.a and the runtime headers relative to its own path (see
# runtime_archive_path in compiler/src/driver.cpp), so the resulting prefix is
# self-contained: bin/ovc + lib/libovrt.a + include/ovrt.h. Nothing else has to
# be configured, and the prefix can be moved.
#
#   make install                      -> /usr/local (needs root)
#   make install PREFIX=$HOME/.local  -> user prefix
#   make install DESTDIR=/tmp/pkg     -> staged package tree
PREFIX  ?= /usr/local
DESTDIR ?=

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/share/ovyth/agent
	install -m 755 $(OVC) $(DESTDIR)$(PREFIX)/bin/ovc
	install -m 644 $(RT_LIB) $(DESTDIR)$(PREFIX)/lib/libovrt.a
	install -m 644 runtime/include/ovrt.h runtime/include/ovrt_helpers.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 examples/agent/tui.ov $(DESTDIR)$(PREFIX)/share/ovyth/agent/tui.ov
	@echo "installed ovc into $(DESTDIR)$(PREFIX)"

# Delegated to uninstall.sh so `make uninstall` and `./uninstall.sh` do the
# same thing and say the same thing: the four files go, directories go only if
# they are empty, and --purge is the opt-in for the package store.
uninstall:
	@DESTDIR="$(DESTDIR)" bash ./uninstall.sh "$(PREFIX)"

clean:
	rm -rf $(BUILD)
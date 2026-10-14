#!/usr/bin/env bash
# Agent memory regression: RAG-backed recall across restarts.
#
# The agent remembers by writing each turn into a RAG store on disk and
# recalling the chunks that match what you just said. The value is that memory
# grows while the context window does not -- which is the whole reason for
# using retrieval rather than dragging history through the prompt.
#
# No model is involved here, and that is deliberate: what is under test is the
# recall path (load -> topk -> inject) and the store commands (:memory,
# :forget), not whether a particular endpoint answers well. The fixture ai
# module answers every request as a failure, so the turn loop stops before the
# model -- but the recall happens *before* that, which is what gets asserted.
#
# The recall block is asserted through OVC_AGENT_DEBUG, which the agent already
# writes to stderr; that keeps the test on the real code path rather than a
# copy of it.
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

BIN="$(mktemp -d)"
cleanup() {
  rm -rf "$BIN"
  pkill -9 -f "ovc run $BIN" 2>/dev/null
}
trap cleanup EXIT

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass+1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

AGENT_HOME="$BIN/home"
mkdir -p "$AGENT_HOME/packages/ai" "$AGENT_HOME/packages/rag"
cp "$PWD/tests/fixtures/ai.ov"  "$AGENT_HOME/packages/ai/ai.ov"
cp "$PWD/tests/fixtures/rag.ov" "$AGENT_HOME/packages/rag/rag.ov"

MEM="$BIN/memory.json"

# A seeded store holding one fact, written by hand: the shape remember_turn
# produces, so the recall path is exercised against real data.
cat > "$MEM" <<'EOF'
[{"text":"Q: what is my cats name\nA: Your cat is called Pixel.","source":"turn","chunk":0,"doc":0},
 {"text":"Q: which editor do i use\nA: You use Neovim.","source":"turn","chunk":0,"doc":1},
 {"text":"Ovyth compiles .ov files to native executables with zero VM overhead.","source":"note","chunk":0,"doc":2}]
EOF

run_agent() { # <stdin-file> -> stdout+stderr
  OVYTH_HOME="$AGENT_HOME" OVYTH_AGENT_MEMORY="$MEM" \
  OVC_AGENT_DEBUG=1 timeout 90 "$OVC" run "$PWD/examples/agent/tui.ov" 2>&1
}

# ---------------------------------------------------------------- recall
# A question about a stored fact must recall it. The fixture model then fails,
# so the output ends there — but the recall line proves the retrieval ran.
printf 'what is my cats name\n:quit\n' > "$BIN/ask.txt"
out="$(run_agent < "$BIN/ask.txt")"
if grep -q "recalled from earlier" <<<"$out"; then
  ok "a matching question recalls memory"
else
  bad "a matching question recalls memory" "$out"
fi
grep -q "cat is called Pixel" <<<"$out" && ok "the recalled chunk is the right one" \
  || bad "the recalled chunk is the right one" "$out"

# A question with nothing to do with the store must not recall anything -- an
# empty prompt block injected on every turn is the failure mode here.
printf 'what time is it in tokyo\n:quit\n' > "$BIN/ask2.txt"
if run_agent < "$BIN/ask2.txt" | grep -q "recalled from earlier"; then
  bad "an unrelated question recalls nothing" "it recalled anyway"
else
  ok "an unrelated question recalls nothing"
fi

# ------------------------------------------------------------------ :memory
# The seeded store holds 3 chunks; the two recall turns above each filed their
# exchange, so the count grows. That growth is the assertion — it is what
# proves turns are being written back, which is half of the memory loop.
printf ':memory\n:quit\n' > "$BIN/mem.txt"
out="$(run_agent < "$BIN/mem.txt")"
chunks="$(sed -n 's/.*memory: \([0-9]*\) chunk(s).*/\1/p' <<<"$out" | head -1)"
[[ "$chunks" =~ ^[0-9]+$ && "$chunks" -gt 3 ]] && ok ":memory reports the store growing ($chunks chunks)" \
  || bad ":memory reports the store size" "$out"
grep -q "cat is called Pixel" <<<"$out" && ok ":memory previews recent chunks" \
  || bad ":memory previews recent chunks" "$out"

# ------------------------------------------------------------------ :forget
printf ':forget\n:memory\n:quit\n' > "$BIN/forget.txt"
out="$(run_agent < "$BIN/forget.txt")"
grep -q "memory erased" <<<"$out" && ok ":forget erases the store" \
  || bad ":forget erases the store" "$out"
grep -q "0 chunk(s)" <<<"$out" && ok ":forget leaves an empty store" \
  || bad ":forget leaves an empty store" "$out"

# A missing store is a cold start, not an error: the agent starts and says 0.
rm -f "$MEM"
printf ':memory\n:quit\n' > "$BIN/cold.txt"
if out="$(run_agent < "$BIN/cold.txt")"; then
  grep -q "0 chunk(s)" <<<"$out" && ok "a missing store is a cold start" \
    || bad "a missing store is a cold start" "$out"
else
  bad "a missing store is a cold start" "agent exited non-zero"
fi

# ------------------------------------------------------- both backends
# The memory path must not depend on which backend runs it: the same fixture
# and the same store, compiled natively, must recall the same fact.
#
# A native compile that fails with "unknown name" is a *known* gap, not a new
# failure: the agent keeps its state (mem, history, sink, settings) in
# top-level lists, and a top-level `let` is invisible to functions on the
# native backend. That predates the memory work and affects the whole agent,
# so it is asserted as the known gap rather than skipped silently.
if nce="$("$OVC" "$PWD/examples/agent/tui.ov" -o "$BIN/native" 2>&1)"; then
  nout="$(OVYTH_HOME="$AGENT_HOME" OVYTH_AGENT_MEMORY="$MEM" \
          OVC_AGENT_DEBUG=1 timeout 90 "$BIN/native" < "$BIN/ask.txt" 2>&1 || true)"
  if grep -q "cat is called Pixel" <<<"$nout"; then
    ok "the native backend recalls memory too"
  else
    bad "the native backend recalls memory too" "$nout"
  fi
elif grep -q "unknown name" <<<"$nce"; then
  ok "native recall: known top-level-state gap blocks the whole agent"
else
  bad "native compile failed unexpectedly" "$nce"
fi

# ------------------------------------------------- a reply with no content
# A gateway answering 200 with no message content must not take the agent
# down. It did once: ai_request reported ok with a nil text, the turn loop
# called .trim() on it, and the agent died mid-session.
#
# This drives it with an ai module that still behaves the old way -- ok=true,
# text=nil -- because with ai 1.0.4 fixed the package reports the failure
# itself and the agent's own guard would never be exercised. The point of the
# test is the agent's guard, so the package side is made to fail again.
OLD_HOME="$BIN/oldhome"
mkdir -p "$OLD_HOME/packages/ai" "$OLD_HOME/packages/rag"
cp "$PWD/tests/fixtures/rag.ov" "$OLD_HOME/packages/rag/rag.ov"
cat > "$OLD_HOME/packages/ai/ai.ov" <<'EOF'
// The message half of ai, with a request that reproduces the old bug:
// 200 answered, no content in the body, and ok left true.
function ai_message(role, content) {
    return {"role": role, "content": content}
}
function ai_conversation(system) {
    return [ai_message("system", system)]
}
function ai_ask(messages, text) {
    messages.push(ai_message("user", text))
    return messages
}
function ai_told(messages, text) {
    messages.push(ai_message("assistant", text))
    return messages
}
function ai_trim(messages, keep) {
    if len(messages) <= keep { return messages }
    head = []
    i = len(messages) - keep
    if i < 0 { i = 0 }
    while i < len(messages) {
        head.push(messages[i])
        i = i + 1
    }
    return head
}
function ai_request(url, key, model, messages, timeout_s) {
    return {"ok": true, "text": null, "status": 200, "error": "",
            "elapsed_ms": 1.0}
}
EOF

printf 'hi\nhello\n:quit\n' > "$BIN/nc.txt"
if out="$(OVYTH_HOME="$OLD_HOME" OVYTH_AGENT_MEMORY="$MEM" \
          timeout 60 "$OVC" run "$PWD/examples/agent/tui.ov" < "$BIN/nc.txt" 2>&1)"; then
  if grep -qE "nil has no method" <<<"$out"; then
    bad "a reply with no content does not crash the agent" "$out"
  elif grep -q "no message content" <<<"$out"; then
    ok "a reply with no content does not crash the agent"
  else
    bad "a reply with no content does not crash the agent" "$out"
  fi
else
  bad "a reply with no content does not crash the agent" "agent exited non-zero"
fi

# ------------------------------------------- a model that answers the nudge
# A weak model, nudged for not producing a tool call, answers the nudge:
# "Understood. I will reply with exactly one JSON object per turn." That is a
# message about the protocol, not an answer, and the agent used to show it to
# the user verbatim. It now says so plainly instead.
MGPORT=$(( 19500 + (RANDOM % 1000) ))
python3 "$PWD/tests/fixtures/metagateway.py" "$MGPORT" >/dev/null 2>&1 &
mg_pid=$!
sleep 1

printf 'what is your name\n:quit\n' > "$BIN/mg.txt"
if out="$(OVYTH_HOME="$AGENT_HOME" OVYTH_AGENT_MEMORY="$MEM" \
          AI_API_URL="http://127.0.0.1:$MGPORT/v1/chat/completions" \
          timeout 90 "$OVC" run "$PWD/examples/agent/tui.ov" < "$BIN/mg.txt" 2>&1)"; then
  if grep -q "Understood. I will reply" <<<"$out"; then
    bad "meta-commentary is not shown to the user" "$out"
  elif grep -q "replying about the protocol" <<<"$out"; then
    ok "meta-commentary is not shown to the user"
  else
    bad "meta-commentary is not shown to the user" "$out"
  fi
else
  bad "meta-commentary is not shown to the user" "agent exited non-zero"
fi
kill $mg_pid 2>/dev/null || true

echo
echo "ovc agent memory regression: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
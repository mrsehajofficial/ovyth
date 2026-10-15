#!/usr/bin/env bash
# The AI boundary suite: what a caller actually gets when the provider is
# wrong.
#
# The contract under test is the published ovyth-ai package's: ai_request()
# never throws, never returns a bare string, and answers with a result map a
# caller can branch on. Every case here drives a provider that misbehaves on
# purpose -- a 500 with an error body, a 200 that is not JSON, a 200 with an
# HTML login page, a 200 with zero choices, a 200 with null content, a
# connection that is refused, and a response that never arrives. The agent
# and the chatbot both sit on this boundary, so a regression here is a
# regression in every AI feature at once.
#
# Credentials get their own case: the key must reach the provider -- the
# suite proves it was sent -- and must appear in no output the client
# produces.
#
# Runs on both backends, because a native backend that shapes an error
# differently from the interpreter is exactly the failure that matters.
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
  [[ -n "${PROV_PID:-}" ]] && kill "$PROV_PID" 2>/dev/null
}
trap cleanup EXIT

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

# The pinned fixture, not the installed package: a clean runner has no store,
# and this suite has to pass on one for the same reason the memory suite
# does. Copy it into the store the suite's HOME points at.
mkdir -p "$BIN/packages/ai"
cp "$PWD/tests/fixtures/ai.ov" "$BIN/packages/ai/ai.ov"

# ---------------------------------------------------------------- provider
python3 "$PWD/tests/ai/provider.py" "$BIN/port" "$BIN/seen" \
  >"$BIN/provider.log" 2>&1 &
PROV_PID=$!

port=""
for _ in $(seq 1 50); do
  [[ -f "$BIN/port" ]] && { port="$(<"$BIN/port")"; break; }
  sleep 0.2
done
if [[ -z "$port" ]]; then
  bad "provider published a port" "$(cat "$BIN/provider.log")"
  exit 1
fi
ok "provider published a port ($port)"

BASE="http://127.0.0.1:$port/v1/chat/completions"
KEY="sk-test-DEADBEEF-notasecret-$(date +%s)"

# One probe against the provider. <model> <timeout> <command...> -> the
# command's output.
probe() { # <model> <timeout> <command...>
  local model="$1" tmo="$2"
  shift 2
  AI_MODE=probe AI_MODEL="$model" AI_TIMEOUT="$tmo" \
    AI_URL="$BASE" AI_KEY="$KEY" \
    OVYTH_HOME="$BIN" HOME="$BIN" "$@" 2>&1
}

# Every network case is asserted on both backends from the same expectation.
run_both() { # <label> <expected> <model> <timeout>
  local label="$1" want="$2" model="$3" tmo="$4"
  local got
  got="$(probe "$model" "$tmo" "$OVC" run "$PWD/tests/ai/boundary.ov" | head -1)"
  if [[ "$got" == "$want" ]]; then
    ok "$label (interp)"
  else
    bad "$label (interp)" "got [$got] want [$want]"
  fi
  AI_MODE=probe AI_URL="$BASE" OVYTH_HOME="$BIN" HOME="$BIN" \
    "$OVC" "$PWD/tests/ai/boundary.ov" -o "$BIN/boundary_bin" >/dev/null 2>&1
  got="$(AI_MODE=probe AI_MODEL="$model" AI_TIMEOUT="$tmo" \
         AI_URL="$BASE" AI_KEY="$KEY" OVYTH_HOME="$BIN" HOME="$BIN" \
         "$BIN/boundary_bin" 2>&1 | head -1)"
  if [[ "$got" == "$want" ]]; then
    ok "$label (native)"
  else
    bad "$label (native)" "got [$got] want [$want]"
  fi
}

# ---------------------------------------------------------------- the cases
# A well-formed reply: ok, and the content is the provider's text verbatim.
run_both "a good reply is ok=true with the content" \
  "RESULT true 200 [] [pong from the bad provider]" ok 5

# A 500 must keep the server's message, not just its status: "HTTP 500"
# gives a user nothing to act on.
run_both "a 500 keeps the server's own message" \
  "RESULT false 500 [HTTP 500: upstream is on fire] []" err500 5

# A 200 whose body is not JSON: the status is named alongside the parse
# error, because a 200 with a login page is a proxy problem, not a
# provider bug.
run_both "a 200 that is not JSON says which status it came with" \
  "RESULT false 200 [HTTP 200: invalid JSON: this is not json, not even close {] []" \
  badjson 5

# An HTML interstitial is the everyday version of "not JSON", and it must
# not be reported as if the caller's own data were at fault.
run_both "an HTML login page is a defined failure" \
  "RESULT false 200 [HTTP 200: invalid JSON: <html><body>Please sign in to continue</body></html>] []" \
  html200 5

# Zero choices: the shape, not the index, is what's wrong.
run_both "zero choices is a defined failure, not an index error" \
  "RESULT false 200 [the endpoint replied 200 with zero choices] []" \
  emptychoices 5

# Null content: the case the package documents, unchanged by the fix.
run_both "null content is 'no message content'" \
  "RESULT false 200 [the endpoint replied with no message content] []" \
  nullcontent 5

# Empty content is a value, not an absence: the caller gets ok=true and an
# empty string, which is what a terse model legitimately answers with.
run_both "empty content is ok with empty text" \
  "RESULT true 200 [] []" emptycontent 5

# ---------------------------------------------------------------- transport
# Nothing is listening: a transport failure is a defined result, not a crash.
got="$(AI_MODE=probe AI_MODEL=ok AI_TIMEOUT=2 AI_URL="http://127.0.0.1:1/v1/chat/completions" \
       AI_KEY="$KEY" OVYTH_HOME="$BIN" HOME="$BIN" \
       "$OVC" run "$PWD/tests/ai/boundary.ov" 2>&1 | head -1)"
if [[ "$got" == RESULT\ false\ 0\ * ]]; then
  ok "a refused connection is a defined failure"
else
  bad "a refused connection is a defined failure" "got [$got]"
fi

# A provider that never answers: the client must stop at its own timeout,
# not at the suite's. Asserted on wall clock, because "bounded" is the
# claim being made.
t0=$(date +%s%N)
got="$(probe hang 1 "$OVC" run "$PWD/tests/ai/boundary.ov" | head -1)"
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
# The message's millisecond count is the measured elapsed time, so it lands
# anywhere in 1000-1099; everything else about it is exact.
if [[ "$got" == "RESULT false 0 [Operation timed out after 1"[0-9][0-9][0-9]" milliseconds with 0 bytes received] []" ]]; then
  ok "a hung provider fails at the client timeout on both backends"
else
  bad "a hung provider fails at the client timeout on both backends" "got [$got]"
fi
if (( ms < 4000 )); then
  ok "the timeout is bounded (${ms}ms for a 1s limit)"
else
  bad "the timeout is bounded" "took ${ms}ms for a 1s limit"
fi

# ---------------------------------------------------------------- credentials
# The key reaches the provider -- the provider's own log says so -- and it
# appears in nothing the client printed.
if grep -q "$KEY" "$BIN/seen" 2>/dev/null; then
  ok "the key is sent to the provider"
else
  bad "the key is sent to the provider" "not in the provider's log"
fi
leaked="$( { probe badjson 2 "$OVC" run "$PWD/tests/ai/boundary.ov";
              probe html200 2 "$OVC" run "$PWD/tests/ai/boundary.ov";
              probe err500 2 "$OVC" run "$PWD/tests/ai/boundary.ov"; } \
           | grep -c "$KEY" )"
if [[ "$leaked" == "0" ]]; then
  ok "the key never appears in client output"
else
  bad "the key never appears in client output" "found $leaked line(s) with it"
fi

# ---------------------------------------------------------------- pure logic
# No network: the two helpers the boundary and the agent both lean on.
for backend in "run" "bin"; do
  if [[ "$backend" == "run" ]]; then
    out="$(AI_MODE=url OVYTH_HOME="$BIN" HOME="$BIN" \
           "$OVC" run "$PWD/tests/ai/boundary.ov" 2>&1; \
           AI_MODE=trim OVYTH_HOME="$BIN" HOME="$BIN" \
           "$OVC" run "$PWD/tests/ai/boundary.ov" 2>&1)"
  else
    AI_MODE=probe AI_URL="$BASE" OVYTH_HOME="$BIN" HOME="$BIN" \
      "$OVC" "$PWD/tests/ai/boundary.ov" -o "$BIN/boundary2" >/dev/null 2>&1
    out="$(AI_MODE=url OVYTH_HOME="$BIN" HOME="$BIN" "$BIN/boundary2" 2>&1; \
           AI_MODE=trim OVYTH_HOME="$BIN" HOME="$BIN" "$BIN/boundary2" 2>&1)"
  fi
  name=$([[ "$backend" == "run" ]] && echo interp || echo native)
  # slice(a, b) is inclusive of b, so a trailing slash is a path of "/" and
  # the default endpoint path has to survive it.
  want_url='URL http://127.0.0.1:11434/v1/chat/completions
URL http://127.0.0.1:11434/v1/chat/completions
URL http://127.0.0.1:8080/v1/chat/completions
URL http://127.0.0.1:8080/v1/chat/completions
URL localhost:11434'
  got_url="$(printf '%s\n' "$out" | grep '^URL' | head -5)"
  if [[ "$got_url" == "$want_url" ]]; then
    ok "ai_normalize_url maps hosts to the endpoint ($name)"
  else
    bad "ai_normalize_url maps hosts to the endpoint ($name)" "got [$got_url]"
  fi
  # system, then the newest two of the five turns (four, five) = a, u.
  got_trim="$(printf '%s\n' "$out" | grep '^TRIM' | tr '\n' ' ')"
  if [[ "$got_trim" == "TRIM len=3 roles=s,a,u, TRIM short=6 "* ]]; then
    ok "ai_trim keeps the system message and cuts the oldest turns ($name)"
  else
    bad "ai_trim keeps the system message and cuts the oldest turns ($name)" "got [$got_trim]"
  fi
done

echo
echo "ai boundary: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

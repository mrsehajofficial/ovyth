#!/usr/bin/env bash
# The automation example's proof.
#
# The example is a claim: fetch, validate, filter, transform, write, retry,
# and report structured errors. A claim needs a failing run as much as a
# passing one, so this suite drives the same pipeline four ways -- a happy
# path, a flaky endpoint the retry has to survive, an endpoint that is down,
# and a malformed-200 endpoint -- and checks the observable outcome of each:
# the exit code, the summary line, the output file's exact bytes, and that a
# failed run leaves nothing behind.
#
# Both backends run every case; the outputs must be byte-identical.
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
[[ -x "$OVC" ]] || { echo "ovc not built; run 'make' first" >&2; exit 2; }

BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"; [[ -n "${API_PID:-}" ]] && kill "$API_PID" 2>/dev/null' EXIT

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

start_api() {  # <port> -> running, and API_PID set
  python3 "$PWD/examples/automation/records_api.py" "$1" >"$BIN/api.log" 2>&1 &
  API_PID=$!
  for _ in $(seq 1 50); do
    curl -s -m 1 "http://127.0.0.1:$1/records" >/dev/null 2>&1 && return 0
    sleep 0.2
  done
  return 1
}

PORT=8799
OUT="$BIN/report.txt"

run_case() {  # <label> <path> -> sets OUT_LINES and RC
  local label="$1" path="$2"
  rm -f "$OUT"
  OUT_LINES="$(RECORDS_URL="http://127.0.0.1:$PORT$path" RECORDS_OUT="$OUT" \
               timeout 60 "$OVC" run "$PWD/examples/automation/pipeline.ov" 2>&1)"
  RC=$?
}

# ---------------------------------------------------------------- the cases
start_api "$PORT" || { bad "the records API started" "$(cat "$BIN/api.log")"; exit 1; }
ok "the records API started"

# 1. The happy path: five records in, three needing attention, two rejected
#    for real schema reasons, an output file with exact bytes.
run_case "happy" "/records"
if [[ $RC -eq 0 ]]; then ok "the happy path exits 0"; else
  bad "the happy path exits 0" "rc=$RC $(tail -1 <<<"$OUT_LINES")"; fi
if grep -qx "summary ok records=3 rejected=2 bytes=125 lines=3" <<<"$OUT_LINES"; then
  ok "the happy path reports the right summary"
else bad "the happy path reports the right summary" "$(grep '^summary' <<<"$OUT_LINES")"; fi
want_report='1 api-gateway degraded cpu=71.2 region=eu-west
3 billing down cpu=0 region=us-east
4 search degraded cpu=88.9 region=us-east'
if [[ "$(cat "$OUT" 2>/dev/null)" == "$want_report" ]]; then
  ok "the output file has exactly the transformed records"
else bad "the output file has exactly the transformed records" "$(cat "$OUT" 2>/dev/null)"; fi
if grep -qx "  rejected 6 missing cpu" <<<"$OUT_LINES" &&
   grep -qx "  rejected 7 name is not a String" <<<"$OUT_LINES"; then
  ok "malformed records are rejected with a reason"
else bad "malformed records are rejected with a reason" "$(grep rejected <<<"$OUT_LINES")"; fi

# 2. The flaky endpoint: the first two requests 500, then it answers. The
#    retry must recover, and the recovery must cost the retries.
rm -f "$OUT"
OUT_LINES="$(RECORDS_URL="http://127.0.0.1:$PORT/flaky" RECORDS_OUT="$OUT" \
             timeout 60 "$OVC" run "$PWD/examples/automation/pipeline.ov" 2>&1)"
RC=$?
attempts=$(grep -c "^fetch attempt" <<<"$OUT_LINES")
if [[ $RC -eq 0 ]] && (( attempts >= 2 )); then
  ok "a transient 500 is retried and recovered from ($attempts retries)"
else bad "a transient 500 is retried and recovered from" "rc=$RC attempts=$attempts"; fi
if grep -qx "summary ok records=3 rejected=2 bytes=125 lines=3" <<<"$OUT_LINES"; then
  ok "the recovered run produces the identical result"
else bad "the recovered run produces the identical result" "$(grep '^summary' <<<"$OUT_LINES")"; fi

# 3. The endpoint is down: a defined failure, exit 70, and no output file --
#    a failed run must not leave a half-written report behind.
rm -f "$OUT"
OUT_LINES="$(RECORDS_URL="http://127.0.0.1:$PORT/down" RECORDS_OUT="$OUT" \
             timeout 60 "$OVC" run "$PWD/examples/automation/pipeline.ov" 2>&1)"
RC=$?
if [[ $RC -eq 70 ]]; then ok "an unavailable endpoint exits 70"; else
  bad "an unavailable endpoint exits 70" "rc=$RC"; fi
if grep -qx "fetch false unreachable the records service did not answer within 4 attempts" <<<"$OUT_LINES"; then
  ok "an unavailable endpoint reports which step failed and why"
else bad "an unavailable endpoint reports which step failed and why" "$(tail -1 <<<"$OUT_LINES")"; fi
if [[ ! -e "$OUT" ]]; then ok "a failed run leaves no output file"; else
  bad "a failed run leaves no output file" "found $OUT after the failure"; fi

# 4. Nothing is listening: the transport failure is a defined error too.
rm -f "$OUT"
OUT_LINES="$(RECORDS_URL="http://127.0.0.1:1/records" RECORDS_OUT="$OUT" \
             timeout 60 "$OVC" run "$PWD/examples/automation/pipeline.ov" 2>&1)"
RC=$?
if [[ $RC -eq 70 ]] && grep -qx "fetch false unreachable the records service did not answer within 4 attempts" <<<"$OUT_LINES"; then
  ok "a refused connection is the same defined failure"
else bad "a refused connection is the same defined failure" "rc=$RC $(tail -1 <<<"$OUT_LINES")"; fi

# 5. A 200 whose body is not JSON at all: a proxy's login page.
run_case "html" "/html"
if [[ $RC -eq 70 ]] && grep -qx "fetch false unparseable the endpoint answered 200 with a body that is not JSON" <<<"$OUT_LINES"; then
  ok "a 200 that is not JSON is a named failure"
else bad "a 200 that is not JSON is a named failure" "rc=$RC $(tail -1 <<<"$OUT_LINES")"; fi

# 6. A 200 with JSON but the wrong shape: valid JSON, no records key.
run_case "malformed" "/nope"
if [[ $RC -eq 70 ]] && grep -qx "fetch false unreachable the records service did not answer within 4 attempts" <<<"$OUT_LINES"; then
  ok "a 404 is retried and then reported as unreachable"
else bad "a 404 is retried and then reported as unreachable" "rc=$RC $(tail -1 <<<"$OUT_LINES")"; fi

# ------------------------------------------------------- both backends
# Every case again, compiled: the claim includes that the example is not an
# interpreter-only story.
compile_fail=0
for path in /records /flaky /down; do
  timeout 120 "$OVC" "$PWD/examples/automation/pipeline.ov" -o "$BIN/pipeline_bin" \
    >"$BIN/cc.log" 2>&1 || { bad "the pipeline compiles natively" "$(head -2 "$BIN/cc.log")"; compile_fail=1; }
done
if (( compile_fail == 0 )); then
  ok "the pipeline compiles natively"
  rm -f "$OUT"
  NAT_LINES="$(RECORDS_URL="http://127.0.0.1:$PORT/records" RECORDS_OUT="$OUT" \
               timeout 60 "$BIN/pipeline_bin" 2>&1)"
  NAT_RC=$?
  INT_LINES="$(RECORDS_URL="http://127.0.0.1:$PORT/records" RECORDS_OUT="$OUT" \
               timeout 60 "$OVC" run "$PWD/examples/automation/pipeline.ov" 2>&1)"
  # Both runs use the same URL, so they must agree line for line.
  if [[ $NAT_RC -eq 0 ]] && [[ "$NAT_LINES" == "$INT_LINES" ]]; then
    ok "the native binary produces byte-identical output"
  else
    bad "the native binary produces byte-identical output" \
        "rc $NAT_RC vs $RC$(diff <(printf '%s\n' "$NAT_LINES") <(printf '%s\n' "$OUT_LINES") | head -4)"
  fi
  if [[ "$(cat "$OUT" 2>/dev/null)" == "$want_report" ]]; then
    ok "the native binary writes the identical output file"
  else bad "the native binary writes the identical output file" "$(cat "$OUT" 2>/dev/null)"; fi
fi

# ---------------------------------------------------------------- examples suite
# The example is part of `make examples`, so it must run under it too.
if grep -q "automation/pipeline.ov" Makefile; then
  ok "the example is wired into the Makefile"
else bad "the example is wired into the Makefile" "not found in Makefile"; fi

echo
echo "automation example: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

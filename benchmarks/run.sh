#!/usr/bin/env bash
# Ovyth :: benchmarks/run.sh
#
# The benchmark driver (spec sections 34-37). It compiles each case natively,
# times the whole process with the shell's own clock, and reports wall-clock
# milliseconds. Timing the process rather than an in-language timer means
# startup and runtime init are included -- excluding them would be exactly the
# "benchmark cheating" section 37 warns against.
#
#   benchmarks/run.sh [--reps N] [--release]
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
REPS="${REPS:-5}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reps) REPS="$2"; shift 2 ;;
    --release) RELEASE=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
RELEASE="${RELEASE:-1}"

EXTRA=""
[[ $RELEASE -eq 1 ]] && EXTRA="--release"

# ---------------------------------------------------------------------------
time_case() {
  local name="$1" file="$2"
  local exe="$OUT/$name"
  if ! "$OVC" "$file" $EXTRA -o "$exe" >/dev/null 2>&1; then
    printf '%-14s BUILD FAILED\n' "$name"
    return
  fi
  local best=99999999 total=0
  for _ in $(seq "$REPS"); do
    local t0 t1 ms
    t0=$(date +%s%N)
    "$exe" >/dev/null 2>&1
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    total=$(( total + ms ))
    (( ms < best )) && best=$ms
  done
  local avg=$(( total / REPS ))
  local bytes
  bytes=$(stat -c%s "$exe")
  printf '%-14s best=%5sms avg=%5sms  binary=%8s bytes\n' "$name" "$best" "$avg" "$bytes"
}

echo "=== Ovyth benchmark suite (native, ${REPS} reps, best-of) ==="
echo
printf '%-14s %s\n' "case" "result"
printf '%s\n' "---------------------------------------------"

time_case startup   benchmarks/cases/startup.ov
time_case intloop   benchmarks/cases/intloop.ov
time_case fib       benchmarks/cases/fib.ov
time_case strconcat benchmarks/cases/strconcat.ov
time_case listappend benchmarks/cases/listappend.ov
time_case mapops    benchmarks/cases/mapops.ov
time_case json      benchmarks/cases/json.ov
time_case chatbot   benchmarks/cases/chatbot_stub.ov

echo
echo "(ms = wall clock for the whole process, including startup)"
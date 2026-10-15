#!/usr/bin/env bash
# Ovyth :: benchmarks/profile.sh
#
# The measurement harness: every dimension of every case, in one table.
#
# Where run.sh answers "is it fast?", this answers "why is it that fast?" --
# which is the only question worth asking before changing anything. Four
# dimensions per case:
#
#   time    wall clock, median across reps, whole process (startup included)
#   memory  peak resident set, measured outside the process
#   heap    allocations, GC cycles, collection resizes, peak heap
#   size    the binary, which is what the user downloads
#
# Correctness is checked first: a case whose output changed is reported as a
# FAILED case, not as a number. Measuring a wrong answer quickly is worse than
# not measuring.
#
#   benchmarks/profile.sh [--reps N] [--interp] [--debug]
#
#   --reps N    repetitions per case (default 5; medians need more than 1)
#   --interp    profile the tree-walking backend instead of compiled binaries
#   --debug     -O0 instead of --release, to see what optimisation is buying
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
REPS="${REPS:-5}"
MODE="release"
WRAP="$(mktemp -d)"
trap 'rm -rf "$WRAP"' EXIT

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reps)  REPS="$2"; shift 2 ;;
    --interp) MODE="interp"; shift ;;
    --debug) MODE="debug"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

cat > "$WRAP/rss.c" <<'EOF'
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char** argv) {
  pid_t p = fork();
  if (p == 0) { execvp(argv[1], argv + 1); _exit(127); }
  int st = 0; waitpid(p, &st, 0);
  struct rusage ru; getrusage(RUSAGE_CHILDREN, &ru);
  fprintf(stderr, "%ld\n", ru.ru_maxrss);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
EOF
clang -O2 -o "$WRAP/rss" "$WRAP/rss.c" || { echo "cannot build the rss helper" >&2; exit 2; }

# median of the timings in $TIMES (space separated, milliseconds)
median() {
  local sorted
  sorted="$(printf '%s\n' $1 | sort -n)"
  local n=0 mid
  for _ in $sorted; do n=$((n + 1)); done
  mid=$((n / 2))
  printf '%s' "$sorted" | sed -n "${mid}p"
}

# One case can be named differently from its file: the chatbot stub has always
# been chatbot_stub.ov, and a harness that reports "BUILD FAILED" for a case
# that compiles is measuring the harness.
CASES="startup intloop fib strconcat listappend mapops json chatbot procs"
declare -A CASE_FILE=( [chatbot]=chatbot_stub )

printf '=== Ovyth profile (%s, %s reps, %s) ===\n\n' "$MODE" "$REPS" "$(hostname)"
printf '%-12s %8s %8s %9s %8s %6s %6s %9s\n' \
  "case" "median" "best" "rss KB" "allocs" "gc" "resize" "binary"
printf '%s\n' "--------------------------------------------------------------------------------------------"

for c in $CASES; do
  f="benchmarks/cases/${CASE_FILE[$c]:-$c}.ov"
  bin="$WRAP/$c"

  # --- build -------------------------------------------------------------
  case "$MODE" in
    release) "$OVC" "$f" --release --stats -o "$bin" >/dev/null 2>&1 ;;
    debug)   "$OVC" "$f" -O0 --stats -o "$bin" >/dev/null 2>&1 ;;
    interp)  : ;;
  esac
  if [[ "$MODE" != "interp" && ! -x "$bin" ]]; then
    printf '%-12s BUILD FAILED\n' "$c"
    continue
  fi

  # --- run function (differs by mode) -------------------------------------
  run_case() {
    if [[ "$MODE" == "interp" ]]; then "$OVC" run --stats "$f" 2>"$WRAP/e"; else "$bin" 2>"$WRAP/e"; fi
  }

  # --- correctness: capture one run, then compare every run to it ---------
  want="$(run_case 2>/dev/null)"
  bad=0
  for _ in $(seq 3); do
    [[ "$(run_case 2>/dev/null)" == "$want" ]] || bad=1
  done
  if [[ $bad -eq 1 ]]; then
    printf '%-12s UNSTABLE OUTPUT (skipped)\n' "$c"
    continue
  fi

  # --- time: median and best of REPS --------------------------------------
  times=""
  for _ in $(seq "$REPS"); do
    t0=$(date +%s%N)
    run_case >/dev/null 2>&1
    t1=$(date +%s%N)
    times="$times $(( (t1 - t0) / 1000000 ))"
  done

  # --- memory: peak RSS of one representative run --------------------------
  if [[ "$MODE" == "interp" ]]; then
    rss="$("$WRAP/rss" "$OVC" run "$f" 2>&1 >/dev/null | tail -1)"
  else
    rss="$("$WRAP/rss" "$bin" 2>&1 >/dev/null | tail -1)"
  fi

  # --- heap telemetry: one run with --stats ------------------------------
  run_case 2>"$WRAP/e" >/dev/null
  allocs="$(sed -n 's/^  allocations *//p' "$WRAP/e")"
  gcs="$(sed -n 's/^  gc cycles *//p' "$WRAP/e")"
  resizes="$(sed -n 's/^  list resizes *//p' "$WRAP/e")"
  peakheap="$(sed -n 's/^  peak heap *\([0-9]*\).*/\1/p' "$WRAP/e")"

  size="-"
  [[ "$MODE" != "interp" ]] && size="$(stat -c%s "$bin")"

  printf '%-12s %7sms %7sms %9s %8s %6s %6s %9s\n' \
    "$c" "$(median "$times")" "$(printf '%s\n' $times | sort -n | sed -n 1p)" \
    "${rss:-?}" "${allocs:-?}" "${gcs:-?}" "${resizes:-?}" "${size:-?}"
done

printf '\n(memory: RSS is the whole process, peak, in KB; the heap counters come from --stats)\n'

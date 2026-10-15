#!/usr/bin/env bash
# Ovyth :: benchmarks/gate.sh
#
# The performance regression gate. profile.sh measures; this file decides
# whether the measurements are still acceptable. Per the performance spec
# (sections 34-37 and the CI requirement), a regression has to be able to
# fail the build rather than be noticed by a reader months later.
#
# Two tiers, because not every metric travels between machines:
#
#   full (default)  median wall clock and peak RSS compared against
#                   benchmarks/EXPECTED.tsv with a 1.5x tolerance. Valid on
#                   the pinned machine the expectations were recorded on
#                   (same CPU, OS, toolchain) — that constraint is the
#                   benchmark protocol, and a gate that lies on a shared
#                   runner is worse than no gate.
#   --tier heap     the deterministic counters only: allocations and
#                   collection resizes may never exceed the recorded value.
#                   These come from the runtime's own --stats telemetry, so
#                   they are machine-independent. This is the tier CI runs.
#                   Fewer allocations than recorded is an improvement, not
#                   a failure.
#
# Usage:
#   benchmarks/gate.sh                 # full gate, 5 reps
#   benchmarks/gate.sh --tier heap     # CI tier (no wall clock)
#   benchmarks/gate.sh --reps 9        # more reps, less median noise
#   benchmarks/gate.sh --update        # re-record EXPECTED.tsv from this run
#
# Exit status: 0 if every checked metric is within tolerance, 1 otherwise.
# A case that fails to build, or whose output is not stable, always fails:
# measuring a wrong or flaky program is not a measurement.
set -u
cd "$(dirname "$0")/.."

REPS=5
TIER="full"
UPDATE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --reps)    REPS="$2"; shift 2 ;;
    --tier)    TIER="$2"; shift 2 ;;
    --update)  UPDATE=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ "$TIER" != "full" && "$TIER" != "heap" ]]; then
  echo "--tier must be full or heap" >&2; exit 2
fi

EXP="benchmarks/EXPECTED.tsv"
[[ -f "$EXP" ]] || { echo "missing $EXP" >&2; exit 2; }

# --- measure ---------------------------------------------------------------
prof="$(mktemp)"
trap 'rm -f "$prof"' EXIT
bash benchmarks/profile.sh --reps "$REPS" > "$prof" 2>/dev/null || {
  echo "profile.sh could not run" >&2; exit 2; }

# Pull the "$case ... $medianms $bestms $rss $allocs $gc $resizes $binary"
# rows out of the table into "case median rss allocs resizes" lines.
rows() {
  awk '/^[a-z_]+[[:space:]]+[0-9]+ms[[:space:]]+[0-9]+ms/ {
         gsub(/ms/, "", $2);
         print $1, $2, $4, $5, $7 }' "$prof"
}

# A broken case is reported by profile.sh as a word, not a number.
harness_failures() {
  awk '/BUILD FAILED|UNSTABLE OUTPUT/ {print $1}' "$prof"
}

# --- compare ---------------------------------------------------------------
fails=0
printf '%-14s %8s %10s %10s %7s\n' metric got recorded limit verdict
printf '%s\n' "-------------------------------------------------------------"

# check_row <label> <got> <recorded> <limit> — pass when got <= limit.
check_row() {
  local label="$1" got="$2" rec="$3" lim="$4"
  if awk -v g="$got" -v l="$lim" 'BEGIN{exit !(g <= l)}'; then
    printf '%-14s %8s %10s %10s %7s\n' "$label" "$got" "$rec" "$lim" PASS
  else
    printf '%-14s %8s %10s %10s %7s\n' "$label" "$got" "$rec" "$lim" FAIL
    fails=$((fails + 1))
  fi
}

while read -r c want_ms want_rss want_allocs want_resizes; do
  got="$(rows | awk -v c="$c" '$1 == c {
          print $2, $3, $4, $5; exit }')"
  if [[ -z "$got" ]]; then
    printf '%-14s %8s %10s %10s %7s\n' "$c/-" "-" "-" "-" MISSING
    fails=$((fails + 1))
    continue
  fi
  read -r ms rss allocs resizes <<< "$got"

  if [[ "$TIER" == "full" ]]; then
    # Wall clock and RSS: 1.5x the recorded value. Desktop noise is
    # documented at +/-20-40%, so this only trips on a real regression.
    check_row "$c/median" "$ms" "$want_ms" \
      "$(awk -v w="$want_ms" 'BEGIN{printf "%d", w * 1.5 + 0.999}')"
    check_row "$c/rss" "$rss" "$want_rss" \
      "$(awk -v w="$want_rss" 'BEGIN{printf "%d", w * 1.5 + 0.999}')"
  else
    # The CI tier reports what it skipped rather than silently omitting it.
    printf '%-14s %8s %10s %10s %7s\n' "$c/median" "skip" "$want_ms" "-" "CI-SKIP"
    printf '%-14s %8s %10s %10s %7s\n' "$c/rss" "skip" "$want_rss" "-" "CI-SKIP"
  fi

  # Deterministic counters, checked on every tier: an allocation or resize
  # regression is a code change, not noise. Improvements pass silently.
  check_row "$c/allocs" "$allocs" "$want_allocs" "$want_allocs"
  check_row "$c/resizes" "$resizes" "$want_resizes" "$want_resizes"
done < <(grep -v '^#' "$EXP")

for c in $(harness_failures); do
  printf '%-14s %8s %10s %10s %7s\n' "$c/-" "-" "-" "-" FAIL
  fails=$((fails + 1))
done

echo

# --- --update ---------------------------------------------------------------
# Re-recording is how a deliberate change is accepted, so it must work even
# when the current numbers are outside tolerance -- otherwise the gate cannot
# be moved forward by the very change it is measuring. A throttled machine
# makes this concrete: every wall clock regressed, and none of it is code.
if (( UPDATE )); then
  {
    echo "# case	median_ms	rss_kb	allocs	resizes"
    echo "# Re-recorded $(date -u +%d\ %b\ %Y) by gate.sh --update on $(hostname),"
    echo "# \`bash benchmarks/profile.sh --reps $REPS\`. The wall clock and RSS"
    echo "# columns are only meaningful on this machine and toolchain; the"
    echo "# allocs/resizes counters are deterministic everywhere, which is why"
    echo "# the CI tier gates only those."
    rows | while read -r c ms rss allocs resizes; do
      printf '%s\t%s\t%s\t%s\t%s\n' "$c" "$ms" "$rss" "$allocs" "$resizes"
    done
  } > "$EXP.tmp" && mv "$EXP.tmp" "$EXP"
  echo "re-recorded $EXP from this run ($fails metric(s) were outside tolerance)"
  exit 0
fi

if (( fails )); then
  echo "gate: $fails failure(s) — investigate before keeping the change"
  echo "(a deliberate improvement re-records the expectations: gate.sh --update)"
  exit 1
fi
echo "gate: all cases within tolerance ($TIER tier, $REPS reps)"

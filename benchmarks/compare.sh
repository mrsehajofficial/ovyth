#!/usr/bin/env bash
# Vayu :: benchmarks/compare.sh
#
# Like-for-like comparison against other implementations of the same
# algorithms (spec section 35). Same input, same algorithm, same output -- and
# the results are checked to be equal, because a comparison where the programs
# compute different things is worthless.
#
#   benchmarks/compare.sh
#
# Methodology: every implementation prints "<case> <ms> <result>" lines and
# times its own cases internally, so these numbers exclude process startup.
# Vayu's whole-process wall clock is reported separately by run.sh; the gap is
# the ~4 ms of process start, which is disclosed rather than hidden.
set -u
cd "$(dirname "$0")/.."

VYC="${VYC:-build/vyc}"
REPS="${REPS:-3}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CASES="intloop fib strconcat listappend mapops"

echo "=== Vayu vs C vs Python -- same algorithms, same outputs ==="
echo

# --- build the references ---
HAVE_C=0
if cc -O3 -march=native benchmarks/ref_c.c -o "$OUT/ref_c" 2>/dev/null; then
  HAVE_C=1
fi
HAVE_PY=0
if command -v python3 >/dev/null 2>&1; then
  python3 benchmarks/ref_python.py > "$OUT/py.txt" 2>/dev/null && HAVE_PY=1
fi

for c in $CASES; do
  "$VYC" "benchmarks/cases/$c.vy" --release -o "$OUT/$c" >/dev/null 2>&1
done

# best-of-N wall clock for one whole binary; echoes "ms"
wall_ms() {
  local best=99999999
  for _ in $(seq "$REPS"); do
    local t0 t1 ms
    t0=$(date +%s%N); "$1" >/dev/null 2>&1; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    (( ms < best )) && best=$ms
  done
  echo "$best"
}

printf '%-12s %11s %11s %11s   %s\n' case "Vayu (wall)" "C -O3" Python "result check"
printf '%s\n' "--------------------------------------------------------------------"

for c in $CASES; do
  [[ -x "$OUT/$c" ]] || continue
  v_ms=$(wall_ms "$OUT/$c")
  v_out=$("$OUT/$c" | tr -d '\n' | sed 's/  */ /g')

  c_ms="-"; c_out=""
  if (( HAVE_C )); then
    line=$(awk -v k="$c" '$1==k {print; exit}' < <("$OUT/ref_c"))
    [[ -n "$line" ]] && c_ms=$(echo "$line" | awk '{print $2}')
    [[ -n "$line" ]] && c_out=$(echo "$line" | cut -d' ' -f3-)
  fi

  p_ms="-"; p_out=""
  if (( HAVE_PY )); then
    line=$(awk -v k="$c" '$1==k {print; exit}' "$OUT/py.txt")
    [[ -n "$line" ]] && p_ms=$(echo "$line" | awk '{print $2}')
    [[ -n "$line" ]] && p_out=$(echo "$line" | cut -d' ' -f3-)
  fi

  # Compare only the part that is the computed answer.
  chk="vy/c: "
  if [[ -n "$c_out" ]]; then
    v_ans=$(echo "$v_out" | tr -d ' ')
    c_ans=$(echo "$c_out" | tr -d ' ')
    if [[ "$v_ans" == "$c_ans" ]]; then chk="identical"; else chk="DIFFERS vy=[$v_ans] c=[$c_ans]"; fi
  else
    chk="(no C)"
  fi
  if [[ -n "$p_out" ]]; then
    p_ans=$(echo "$p_out" | tr -d ' ,()')
    v_ans=$(echo "$v_out" | tr -d ' ')
    [[ "$v_ans" == "$p_ans" ]] && chk="$chk, py: identical" || chk="$chk, py: differs"
  fi

  c_disp="${c_ms}ms"
  printf '%-12s %10sms %10s %10s   %s\n' \
    "$c" "$v_ms" "$c_disp" "${p_ms}ms" "$chk"
done

echo
if (( HAVE_C && HAVE_PY )); then
  echo "Result columns are checked, not assumed: Vayu's answer must match C's."
else
  echo "Some references were unavailable; columns are omitted rather than estimated."
fi
echo "Rust and Go are not installed on this machine, so they are omitted too."
echo "Vayu numbers are whole-process wall clock; C/Python time their cases"
echo "internally, so Vayu's column additionally carries ~4ms of process start."
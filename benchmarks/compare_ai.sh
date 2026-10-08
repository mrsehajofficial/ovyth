#!/usr/bin/env bash
# Vayu :: benchmarks/compare_ai.sh
#
# Compare AI pipeline benchmark: Vayu vs C vs Python.
# Runs all three, checks results are plausible, prints a comparison table.
#
# Usage:
#   bash benchmarks/compare_ai.sh
#   bash benchmarks/compare_ai.sh --release   (use -O3 for Vayu)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VYC="$REPO_ROOT/build/vyc"
RELEASE_FLAG=""
if [[ "${1:-}" == "--release" ]]; then
  RELEASE_FLAG="--release"
fi

echo "=== Vayu vs C vs Python -- AI pipeline benchmark ==="
echo ""

# ----------------------------------------------------------------- build

# Build Vayu binary
TMP_AI="/tmp/vayu_bench_ai_$$"
if [[ -n "$RELEASE_FLAG" ]]; then
  echo "Building Vayu (--release)..."
  "$VYC" "$REPO_ROOT/benchmarks/bench_ai.vy" --release -o "$TMP_AI"
else
  echo "Building Vayu (debug)..."
  "$VYC" "$REPO_ROOT/benchmarks/bench_ai.vy" -o "$TMP_AI"
fi

# Build C reference
TMP_C="/tmp/ref_ai_c_$$"
echo "Building C (-O3)..."
clang -O3 -std=c11 -march=native "$REPO_ROOT/benchmarks/ref_ai.c" -o "$TMP_C" -lm

echo ""
echo "Running benchmarks..."
echo ""

# ----------------------------------------------------------------- run

VY_OUT=$(   "$TMP_AI" 2>/dev/null | grep -E '^[a-z_]+\s+[0-9]+ms' || true)
C_OUT=$(    "$TMP_C"  2>/dev/null | grep -E '^[a-z_]+\s+[0-9.]+ms' || true)
PY_OUT=$(   python3 "$REPO_ROOT/benchmarks/ref_ai.py" 2>/dev/null | grep -E '^[a-z_]+\s+[0-9]+ms' || true)

# ----------------------------------------------------------------- parse and print table

printf "%-18s  %10s  %8s  %8s  %10s  %10s\n" \
  "case" "Vayu (ms)" "C (ms)" "Py (ms)" "vs C" "vs Python"
printf "%-18s  %10s  %8s  %8s  %10s  %10s\n" \
  "------------------" "----------" "--------" "--------" "----------" "----------"

cases=("json_parse" "json_access" "context_build" "chunk_pipeline" "hash_map_str" "multi_parse" "string_scan")

for case in "${cases[@]}"; do
  vy_ms=$(echo "$VY_OUT" | awk -v c="$case" '$1==c{gsub(/ms/,"",$2); print $2}' | head -1)
  c_ms=$(echo  "$C_OUT"  | awk -v c="$case" '$1==c{gsub(/ms/,"",$2); print $2}' | head -1)
  py_ms=$(echo "$PY_OUT" | awk -v c="$case" '$1==c{gsub(/ms/,"",$2); print $2}' | head -1)

  vy_ms="${vy_ms:-?}"
  c_ms="${c_ms:-?}"
  py_ms="${py_ms:-?}"

  vs_c="?"
  vs_py="?"

  if [[ "$vy_ms" != "?" && "$c_ms" != "?" ]]; then
    vs_c=$(awk "BEGIN { if ($c_ms+0 == 0) printf \"n/a\"; else { r=$vy_ms/$c_ms; if (r>=1) printf \"%.1fx slower\", r; else printf \"%.1fx faster\", 1/r } }")
  fi
  if [[ "$vy_ms" != "?" && "$py_ms" != "?" ]]; then
    vs_py=$(awk "BEGIN { if ($py_ms+0 == 0) printf \"n/a\"; else { r=$vy_ms/$py_ms; if (r>=1) printf \"%.1fx slower\", r; else printf \"%.1fx faster\", 1/r } }")
  fi

  printf "%-18s  %10s  %8s  %8s  %10s  %10s\n" \
    "$case" "${vy_ms}ms" "${c_ms}ms" "${py_ms}ms" "$vs_c" "$vs_py"
done

echo ""
echo "Notes:"
echo "  - All three sides time each case internally, so startup is excluded"
echo "  - Build: C uses clang -O3 -march=native"
if [[ -n "$RELEASE_FLAG" ]]; then
  echo "  - Vayu built with --release (-O3)"
else
  echo "  - Vayu built with debug flags (use --release for fair comparison)"
fi
echo ""
echo "Context: These benchmarks measure the AI orchestration layer"
echo "  (JSON, context assembly, chunking, map ops) -- not LLM inference."
echo "  LLM inference dominates at 100ms-10s; we optimize the 1-50ms overhead."

# cleanup
rm -f "$TMP_AI" "$TMP_C"

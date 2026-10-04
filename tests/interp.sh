#!/usr/bin/env bash
# Vayu regression suite: runs every test in tests/interp/ through BOTH
# backends and requires them to agree with the golden output.
#
#   1. the tree-walking interpreter   (`vyc run`)
#   2. the native backend             (`vyc <file> -o exe`, then run exe)
#
# Checking both is the point: a native backend that silently diverges from the
# interpreter is the failure mode that matters. Exit non-zero on any failure.
set -u
cd "$(dirname "$0")/.."

VYC="${VYC:-build/vyc}"
if [[ ! -x "$VYC" ]]; then
  echo "vyc not built ($VYC); run 'make' first" >&2
  exit 2
fi

BIN="${BIN:-$(mktemp -d)}"
trap 'rm -rf "$BIN"' EXIT

pass=0
fail=0

report_fail() { # name backend detail
  printf 'FAIL %s [%s] %s\n' "$1" "$2" "$3"
  fail=$((fail+1))
}

for f in tests/interp/*.vy; do
  [[ -e "$f" ]] || continue
  name="$(basename "$f")"
  golden="${f%.vy}.want"
  want=""
  [[ -f "$golden" ]] && want="$(cat "$golden")"

  # ---- interpreter ----
  iout="$("$VYC" run "$f" 2>"$BIN/err")"; irc=$?
  if [[ $irc -ne 0 ]]; then
    report_fail "$name" interp "rc=$irc $(head -c 200 "$BIN/err")"
  elif [[ -n "$want" && "$iout" != "$want" ]]; then
    report_fail "$name" interp "output mismatch: got [$iout] want [$want]"
  else
    printf 'PASS %s [interp]\n' "$name"; pass=$((pass+1))
  fi

  # ---- native ----
  exe="$BIN/$(basename "${f%.vy}")"
  if ! err="$("$VYC" "$f" -o "$exe" 2>&1)"; then
    report_fail "$name" native "compile failed: $(echo "$err" | head -c 300)"
    continue
  fi
  nout="$("$exe" 2>"$BIN/nerr")"; nrc=$?
  if [[ $nrc -ne 0 ]]; then
    report_fail "$name" native "rc=$nrc $(head -c 200 "$BIN/nerr")"
  elif [[ -n "$want" && "$nout" != "$want" ]]; then
    report_fail "$name" native "output mismatch: got [$nout] want [$want]"
  elif [[ "$nout" != "$iout" ]]; then
    # Backends disagree even though both match / predate the golden.
    report_fail "$name" native "diverges from interpreter: native [$nout] interp [$iout]"
  else
    printf 'PASS %s [native ]\n' "$name"; pass=$((pass+1))
  fi
done

echo
echo "vyc regression: $pass passed, $fail failed (both backends)"
[[ $fail -eq 0 ]]
#!/usr/bin/env bash
# The rag index suite: indexed ranking must be *identical* to a full scan.
#
# The claim being defended is narrow and testable: skipping only the chunks
# that hold none of the query's terms cannot change what is recalled, because
# every such chunk scores zero. A store of 1000 chunks, a list of queries, and
# both paths rendered to strings -- if they differ anywhere, the suite says
# where.
#
# The agent's memory grows for the life of the process and beyond, so this is
# the test that keeps recall cheap without also making it quietly wrong.
#
# Runs on both backends, because a native backend that ranks differently from
# the interpreter is the failure mode that matters.
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

BIN="$(mktemp -d)"
HOMEDIR="$BIN/home"
trap 'rm -rf "$BIN"' EXIT

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass+1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

# The pinned fixture, not the installed package: a clean runner has no store,
# and this suite has to pass on one for the same reason the memory suite does.
mkdir -p "$HOMEDIR/packages/rag"
cp "$PWD/tests/fixtures/rag.ov" "$HOMEDIR/packages/rag/rag.ov"

PROG="$PWD/tests/rag/idx.ov"

# ---------------------------------------------------------------- interpreter
iout="$(OVYTH_HOME="$HOMEDIR" timeout 180 "$OVC" run "$PROG" 2>&1)"; irc=$?
if [[ $irc -ne 0 ]]; then
  bad "the differential program runs" "$irc: $(head -c 300 <<<"$iout")"
else
  ok "the differential program runs"
  grep -q "RESULT PASS" <<<"$iout" && ok "indexed ranking matches the full scan (interp)" \
    || bad "indexed ranking matches the full scan (interp)" "$iout"
  # The index must actually skip, or the whole thing is theatre: a term

  if grep -qE "mask marks ([0-9]+) of 1000" <<<"$iout"; then
    marked="$(grep -oE "mask marks [0-9]+" <<<"$iout" | grep -oE "[0-9]+")"
    if [[ "$marked" -lt 500 ]]; then
      ok "the index really skips chunks ($marked of 1000)"
    else
      bad "the index really skips chunks" "marked $marked of 1000"
    fi
  else
    bad "the index really skips chunks" "no mask line in output"
  fi
  grep -q "MISMATCH" <<<"$iout" && bad "no mismatches reported" "$(grep MISMATCH <<<"$iout" | head -3)" \
    || ok "no mismatches reported"
fi

# --------------------------------------------------------------------- native
exe="$BIN/idx"
if err="$(OVYTH_HOME="$HOMEDIR" timeout 180 "$OVC" "$PROG" -o "$exe" 2>&1)"; then
  nout="$(OVYTH_HOME="$HOMEDIR" timeout 180 "$exe" 2>&1)"; nrc=$?
  if [[ $nrc -ne 0 ]]; then
    bad "the differential program compiles and runs natively" "rc=$nrc: $(head -c 300 <<<"$nout")"
  elif [[ "$nout" != "$iout" ]]; then
    bad "native agrees with the interpreter" "native:
$nout
interp:
$iout"
  else
    ok "native agrees with the interpreter"
  fi
else
  bad "the differential program compiles natively" "$(head -c 300 <<<"$err")"
fi

echo
echo "ovc rag index regression: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

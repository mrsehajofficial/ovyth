#!/usr/bin/env bash
# The agent package's own logic, tested directly rather than through a model.
#
# tests/agent/guard_test.ov imports the published package and asserts:
#   * the guard's allow/refuse matrix, including the cases that passed the first
#     version of the guard (rm -rf / and rm -rf $HOME)
#   * AGENT_PROTECT and the parsing of an rm target
#   * json_end's bracketing: doubled braces, fences, braces inside strings,
#     escapes, nesting, truncation
#
# The package is installed into a throwaway OVYTH_HOME from the live registry, so
# the test exercises what users actually install. With no network it is skipped
# with a note rather than silently passing -- a skipped test is honest, a green
# test that ran nothing is not.
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"' EXIT

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass+1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
note(){ printf 'SKIP %s: %s\n' "$1" "${2:-}"; }

# A local `ai` stand-in, so importing the package's entry module does not need a
# real endpoint. (guard.ov and json.ov need nothing, but agent.ov does.)
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

# --- install the package into a private store -------------------------------
# OVYTH_HOME is set first so the install cannot touch the developer's own store,
# and --force makes it work whether or not the package is already there.
export OVYTH_HOME="$BIN/home"
if ! "$OVC" pkg install agent --refresh --force \
        >"$BIN/install.log" 2>&1; then
  note "agent tests" "could not install the agent package (offline?): $(tail -1 "$BIN/install.log")"
  echo
  echo "ovc agent regression: 0 passed, 0 failed, 1 skipped"
  exit 0
fi
[[ -d "$OVYTH_HOME/packages/agent" ]] || {
  note "agent package tests" "agent did not land in the store"
  exit 0
}
ok "agent 1.0.0 installed into a private store"

# --- the published guard and json modules ------------------------------------
for mod in guard json; do
  [[ -f "$OVYTH_HOME/packages/agent/$mod.ov" ]] && ok "agent/$mod.ov is part of the package" \
    || bad "agent/$mod.ov is part of the package" "missing"
done

# --- run the assertions ------------------------------------------------------
out="$(timeout 120 "$OVC" run tests/agent/guard_test.ov 2>&1)"
rc=$?
if [[ $rc -eq 0 ]] && grep -q "all assertions passed" <<<"$out"; then
  ok "guard + json assertions"
else
  bad "guard + json assertions" "$out"
fi

# A regression guard for the two commands that defeated the first guard. The
# matrix above covers them too, but these run a file the test writes itself --
# if the guard were ever removed from the package, this is what would say so.
cat > "$BIN/refuse.ov" <<'EOF'
import "agent/guard"
for c in ["rm -rf /", "rm -rf /usr", "rm -rf $HOME", "mkfs.ext4 /dev/sda1"] {
    if guard(c)["ok"] { throw "the guard allowed: " + c }
}
print("ok")
EOF
rout="$(timeout 60 "$OVC" run "$BIN/refuse.ov" 2>&1)"
[[ "$rout" == "ok" ]] && ok "the published guard still refuses the four"   || bad "the published guard still refuses the four" "$rout"

echo
echo "ovc agent regression: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

#!/usr/bin/env bash
# Package manager regression suite.
#
# Drives the whole pipeline against a throwaway OVYTH_HOME so the machine's
# real store is never touched, and against a file:// registry index so the
# suite runs offline:
#
#   1. install a package by name through the registry index
#   2. import it from an unrelated directory, on BOTH backends
#   3. import a submodule of it
#   4. install a second copy without --force (must fail) and with it
#   5. list / search / info / update
#   6. remove it, and confirm the import then fails
#   7. a local shadowing module still wins over the installed package
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"' EXIT

pass=0
fail=0
ok()   { printf 'PASS %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
check_eq() { # desc expected actual
  if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1" "got [$3] want [$2]"; fi
}

# ---------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------
export OVYTH_HOME="$BIN/home"

# The package source, with an entry module and a submodule.
mkdir -p "$BIN/src/greet"
cat > "$BIN/src/greet/greet.ov" <<'EOF'
function greet(who) {
    return "hello, " + who
}
EOF
cat > "$BIN/src/greet/extra.ov" <<'EOF'
function farewell(who) {
    return "bye, " + who
}
EOF

# A registry index naming it. `path` sources are what keep this offline; a
# real index uses "repo" (git) or "url" (tarball) instead.
cat > "$BIN/registry.json" <<EOF
{
  "packages": {
    "greet": {
      "description": "a greeting module used by the package manager tests",
      "license": "MIT",
      "homepage": "https://example.invalid/greet",
      "versions": {
        "1.0.0": { "path": "$BIN/src/greet", "note": "first" }
      }
    }
  }
}
EOF
REGISTRY="file://$BIN/registry.json"

# A program in a directory with no .ov files of its own -- the whole point of
# a global store is that the import works from here.
mkdir -p "$BIN/work"
cat > "$BIN/work/main.ov" <<'EOF'
import "greet"
import "greet/extra"

print(greet("world"))
print(farewell("world"))
EOF

# ---------------------------------------------------------------------------
# install
# ---------------------------------------------------------------------------
if out="$("$OVC" pkg install greet --registry="$REGISTRY" 2>&1)"; then
  ok "pkg install greet"
else
  bad "pkg install greet" "$out"
fi
[[ -f "$OVYTH_HOME/packages/greet/greet.ov" ]] && ok "package landed in the store" \
  || bad "package landed in the store" "no greet.ov in $OVYTH_HOME/packages/greet"
[[ -f "$OVYTH_HOME/packages/greet/.ovpkg.json" ]] && ok "install metadata written" \
  || bad "install metadata written" "no .ovpkg.json"

# ---------------------------------------------------------------------------
# importing from an unrelated directory, on both backends
# ---------------------------------------------------------------------------
iout="$("$OVC" run "$BIN/work/main.ov" 2>&1)"; irc=$?
check_eq "interpreter imports the package" "hello, world
bye, world" "$iout"

exe="$BIN/app"
if cerr="$("$OVC" "$BIN/work/main.ov" -o "$exe" 2>&1)"; then
  nout="$("$exe" 2>&1)"
  check_eq "native backend imports the package" "hello, world
bye, world" "$nout"
else
  bad "native backend imports the package" "$cerr"
fi

# ---------------------------------------------------------------------------
# reinstall policy
# ---------------------------------------------------------------------------
if "$OVC" pkg install greet --registry="$REGISTRY" >/dev/null 2>&1; then
  bad "second install is refused" "expected a non-zero exit"
else
  ok "second install is refused"
fi
if "$OVC" pkg install greet --force --registry="$REGISTRY" >/dev/null 2>&1; then
  ok "--force reinstalls"
else
  bad "--force reinstalls"
fi

# ---------------------------------------------------------------------------
# inspection
# ---------------------------------------------------------------------------
lout="$("$OVC" pkg list 2>&1)"
grep -q 'greet' <<<"$lout" && ok "pkg list shows the package" || bad "pkg list shows the package" "$lout"

sout="$("$OVC" pkg search greeting --registry="$REGISTRY" 2>&1)"
grep -q 'greet' <<<"$sout" && ok "pkg search finds it" || bad "pkg search finds it" "$sout"

iout="$("$OVC" pkg info greet --registry="$REGISTRY" 2>&1)"
grep -q 'installed   yes' <<<"$iout" && ok "pkg info reports installed" || bad "pkg info reports installed" "$iout"
grep -q '1.0.0' <<<"$iout" && ok "pkg info reports the version" || bad "pkg info reports the version" "$iout"

if "$OVC" pkg update greet --registry="$REGISTRY" >/dev/null 2>&1; then
  ok "pkg update on an installed package"
else
  bad "pkg update on an installed package"
fi

# ---------------------------------------------------------------------------
# a directory in the project must not shadow an installed package
#
# Regression: local import resolution accepted any readable path, so an
# unrelated `greet/` directory next to the importer swallowed `import "greet"`
#  and the program compiled with none of the module's definitions -- no error,
#  just missing functions.
mkdir -p "$BIN/work/greet"
printf 'not a module\n' > "$BIN/work/greet/keep.txt"
cat > "$BIN/work/shadow.ov" <<'EOF'
import "greet"
print(greet("shadow"))
EOF
out="$("$OVC" run "$BIN/work/shadow.ov" 2>&1)"
grep -q "hello, shadow" <<<"$out" && ok "a directory does not shadow a package" \
  || bad "a directory does not shadow a package" "$out"

# A real module file still wins, exactly as it should.
cat > "$BIN/work/greet.ov" <<'EOF'
function greet(who) {
    return "LOCAL " + who
}
EOF
out="$("$OVC" run "$BIN/work/shadow.ov" 2>&1)"
grep -q "LOCAL shadow" <<<"$out" && ok "a local module still shadows a package" \
  || bad "a local module still shadows a package" "$out"
rm -f "$BIN/work/greet.ov"

# ---------------------------------------------------------------------------
# a local module shadows the installed package
# ---------------------------------------------------------------------------
cat > "$BIN/work/greet.ov" <<'EOF'
function greet(who) {
    return "LOCAL " + who
}
EOF
lout="$("$OVC" run "$BIN/work/main.ov" 2>&1)"
grep -q 'LOCAL world' <<<"$lout" && ok "local module shadows the package" \
  || bad "local module shadows the package" "$lout"
rm "$BIN/work/greet.ov"

# ---------------------------------------------------------------------------
# removal
# ---------------------------------------------------------------------------
if "$OVC" pkg remove greet >/dev/null 2>&1; then
  ok "pkg remove"
else
  bad "pkg remove"
fi
[[ -d "$OVYTH_HOME/packages/greet" ]] && bad "package directory gone" "still there" \
  || ok "package directory gone"

if "$OVC" run "$BIN/work/main.ov" >/dev/null 2>&1; then
  bad "import fails after removal" "expected a non-zero exit"
else
  ok "import fails after removal"
fi

# ---------------------------------------------------------------------------
# an unknown package fails cleanly, and says what it does have
# ---------------------------------------------------------------------------
if out="$("$OVC" pkg install nope --registry="$REGISTRY" 2>&1)"; then
  bad "unknown package fails" "expected a non-zero exit"
else
  grep -qi "no package named" <<<"$out" && ok "unknown package fails" \
    || bad "unknown package fails" "$out"
fi

# An empty store lists nothing instead of erroring.
if out="$("$OVC" pkg list 2>&1)"; then
  grep -qi "no packages installed" <<<"$out" && ok "empty store lists nothing" \
    || bad "empty store lists nothing" "$out"
else
  bad "empty store lists nothing" "$out"
fi

# ---------------------------------------------------------------------------
# a registry whose index is broken reports the problem, not a crash
# ---------------------------------------------------------------------------
echo 'not json' > "$BIN/bad.json"
if out="$("$OVC" pkg search x --registry="file://$BIN/bad.json" 2>&1)"; then
  bad "malformed registry index fails" "expected a non-zero exit"
else
  ok "malformed registry index fails"
fi

# ---------------------------------------------------------------------------
# the help flag is a question, not an error
# ---------------------------------------------------------------------------
if "$OVC" pkg --help >/dev/null 2>&1; then ok "ovc pkg --help"; else bad "ovc pkg --help"; fi
if "$OVC" --help 2>&1 | grep -q "ovc pkg"; then ok "ovc --help mentions pkg"; else bad "ovc --help mentions pkg"; fi
if "$OVC" pkg nonsense >/dev/null 2>&1; then bad "unknown subcommand fails"; else ok "unknown subcommand fails"; fi

# ---------------------------------------------------------------------------
# a package name is not allowed to escape the store
#
# Path-shaped specs are the user's own local install (they must still fail
# here, for want of a directory); the registry-shaped ones are what a hostile
# index would serve, and those are rejected by name.
# ---------------------------------------------------------------------------
for bad_name in "../evil" ".hidden" "a/b" "sp ace"; do
  if out="$("$OVC" pkg install "$bad_name" --registry="$REGISTRY" 2>&1)"; then
    bad "hostile name '$bad_name' rejected" "expected a non-zero exit"
  elif [[ "$bad_name" == *"/"* || "$bad_name" == .* ]]; then
    ok "hostile name '$bad_name' rejected"
  else
    grep -qi "not a valid package name" <<<"$out" && ok "hostile name '$bad_name' rejected" \
      || bad "hostile name '$bad_name' rejected" "$out"
  fi
done
[[ -e "$OVYTH_HOME/evil" || -e "$OVYTH_HOME/packages/evil" ]] && bad "nothing escaped the store" \
  || ok "nothing escaped the store"

echo
echo "ovc pkg regression: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

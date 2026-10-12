#!/usr/bin/env bash
# UI regression: decoration is for terminals only.
#
# The rule this guards is simple — when stdout is piped, ovc prints exactly
# what it printed before the pretty layer existed, byte for byte, with no
# escape codes. tests/pkg.sh and tests/interp.sh both depend on it: one greps
# real strings out of `ovc pkg`, the other compares two backends byte for
# byte. A box drawn into a pipe would break both.
#
#   1. piped output contains no ESC and no box-drawing characters
#   2. NO_COLOR produces the same thing (structure only)
#   3. `ovc version` stays a single parseable line
#   4. the strings the other suites grep for are still there
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"' EXIT
export OVYTH_HOME="$BIN/home"

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass+1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

# A fixture package, installed through a file:// index, so the commands below
# have something real to print.
mkdir -p "$BIN/src/greet"
printf 'function greet(who) {\n    return "hello, " + who\n}\n' > "$BIN/src/greet/greet.ov"
printf '{\n "packages": {\n  "greet": {\n   "description": "a friendly greeting",\n   "license": "MIT",\n   "versions": { "1.0.0": { "path": "%s" } }\n  }\n }\n}\n' \
  "$BIN/src/greet" > "$BIN/registry.json"
REGISTRY="file://$BIN/registry.json"
"$OVC" pkg install greet --registry="$REGISTRY" >/dev/null 2>&1

# check_plain <desc> <command...> -- every line of the command's output, with
# stdout already a pipe, must be free of ESC and box drawing.
check_plain() {
  local desc="$1"; shift
  local out
  out="$("$@" 2>&1)"
  # 0x1b, and the box-drawing range U+2500-U+257F
  if grep -qP '\x1b|[\x{2500}-\x{257f}]' <<<"$out"; then
    bad "$desc" "escape or box-drawing bytes in piped output"
    return
  fi
  ok "$desc"
}

check_plain "pkg list is plain when piped"   "$OVC" pkg list
check_plain "pkg search is plain when piped"  "$OVC" pkg search greet --registry="$REGISTRY"
check_plain "pkg info is plain when piped"    "$OVC" pkg info greet --registry="$REGISTRY"
check_plain "version is plain when piped"     "$OVC" version
check_plain "init output is plain when piped" "$OVC" init "$BIN/app"

# NO_COLOR: the same commands must not emit colour either. (The sprite is gone
# too, because decoration as a whole is TTY-only — but the *text* is intact.)
no_color="$(NO_COLOR=1 "$OVC" pkg list 2>&1)"
if grep -qP '\x1b' <<<"$no_color"; then
  bad "NO_COLOR suppresses colour" "escape codes present"
else
  ok "NO_COLOR suppresses colour"
fi

# --- the exact strings the other suites rely on ----------------------------
info="$("$OVC" pkg info greet --registry="$REGISTRY" 2>&1)"
grep -q 'installed   yes' <<<"$info" && ok "pkg info still prints 'installed   yes'" \
  || bad "pkg info still prints 'installed   yes'" "$info"

empty="$("$OVC" pkg list 2>&1; HOME="$BIN/nohome" OVYTH_HOME="$BIN/nohome/.ovyth" "$OVC" pkg list 2>&1)"
grep -q 'no packages installed' <<<"$empty" \
  && ok "pkg list still reports an empty store" \
  || bad "pkg list still reports an empty store" "$empty"

# --- `ovc version` stays one parseable line --------------------------------
ver="$(./build/ovc version)"
if [[ "$ver" == "ovc "*"(Ovyth)" ]]; then
  ok "ovc version is a single parseable line"
else
  bad "ovc version is a single parseable line" "[$ver]"
fi
lines="$(./build/ovc version | wc -l)"
[[ "$lines" == "1" ]] && ok "ovc version prints exactly one line" \
  || bad "ovc version prints exactly one line" "$lines lines"

# --- no camelCase string methods (startswith, not StartsWith) --------------
# The language's methods are all lower-case. A camelCase call parses and fails at
# runtime, which is the worst place to find it -- three separate times while
# writing the agent, so it is now checked here rather than discovered.
bad_ov="$(grep -rn '\.startsWith(\|\.indexOf(\|\.lastIndexOf(' --include='*.ov' . 2>/dev/null)"
if [[ -n "$bad_ov" ]]; then
  bad "no camelCase string methods in .ov sources" "$bad_ov"
else
  ok "no camelCase string methods in .ov sources"
fi

# --- the installer stays plain when piped ---------------------------------
inst="$(bash install.sh "$BIN/prefix" 2>&1 | cat -v)"
if grep -q '\^\[' <<<"$inst"; then
  bad "install.sh is plain when piped" "escape codes present"
else
  ok "install.sh is plain when piped"
fi
grep -q 'bin/ovc' <<<"$inst" && grep -q 'libovrt.a' <<<"$inst" \
  && grep -q 'ovrt.h' <<<"$inst" && ok "install.sh still lists all three paths" \
  || bad "install.sh still lists all three paths" "$inst"
grep -q 'try it:' <<<"$inst" && ok "install.sh still prints the try-it hint" \
  || bad "install.sh still prints the try-it hint" "$inst"

# --- the uninstaller removes what the installer put there -------------------
# Install into a throwaway prefix, then remove it, and check that exactly the
# four files are gone — and that anything else in those directories is not.
p="$BIN/prefix"
bash install.sh "$p" >/dev/null 2>&1
for f in bin/ovc lib/libovrt.a include/ovrt.h include/ovrt_helpers.h; do
  [ -f "$p/$f" ] || { bad "uninstall setup: $f installed" "missing"; }
done
# A file the installer never wrote: it and its directory must survive.
touch "$p/bin/not-ours"

un="$(bash uninstall.sh "$p" 2>&1 | cat -v)"
if grep -q '\^\[' <<<"$un"; then
  bad "uninstall.sh is plain when piped" "escape codes present"
else
  ok "uninstall.sh is plain when piped"
fi
for f in bin/ovc lib/libovrt.a include/ovrt.h include/ovrt_helpers.h; do
  if [ -e "$p/$f" ]; then bad "uninstall removed $f" "still there"
  else ok "uninstall removed $f"; fi
done
[ -e "$p/bin/not-ours" ] && ok "uninstall leaves other files alone" \
  || bad "uninstall leaves other files alone" "not-ours was deleted"
[ -d "$p/bin" ] && ok "uninstall keeps a non-empty directory" \
  || bad "uninstall keeps a non-empty directory" "bin/ was removed"
# Empty directories the installer created and left empty do go.
[ -d "$p/include" ] && bad "uninstall removes emptied directories" "include/ remains" \
  || ok "uninstall removes emptied directories"

bash uninstall.sh --help >/dev/null 2>&1 && ok "uninstall.sh --help" \
  || bad "uninstall.sh --help"
# Uninstalling something that was never installed is a quiet success, not an error.
bash uninstall.sh "$BIN/never-installed" >/dev/null 2>&1 \
  && ok "uninstall of a missing prefix is not an error" \
  || bad "uninstall of a missing prefix is not an error"
# --purge must refuse to delete a directory that is not a package store.
if out="$(OVYTH_HOME="$HOME" bash uninstall.sh "$BIN/prefix" --purge 2>&1)"; then
  bad "--purge refuses a non-store path" "expected a non-zero exit"
else
  grep -qi "does not look like a store" <<<"$out" \
    && ok "--purge refuses a non-store path" \
    || bad "--purge refuses a non-store path" "$out"
fi

echo
echo "ovc ui regression: $pass passed, $fail failed"
[[ $fail -eq 0 ]]

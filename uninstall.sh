#!/usr/bin/env bash
# Ovyth uninstaller.
#
#   ./uninstall.sh                       remove the install in ~/.local
#   ./uninstall.sh /usr/local            remove the install in a prefix
#   DESTDIR=/tmp/pkg ./uninstall.sh /usr/local     un-stage a package tree
#   ./uninstall.sh --purge               also remove ~/.ovyth (your packages)
#
# It removes exactly the files install.sh put there — never a directory it
# did not create, never anything else in the prefix:
#
#   <prefix>/bin/ovc            the compiler
#   <prefix>/lib/libovrt.a      the runtime
#   <prefix>/include/ovrt.h     the runtime headers
#   <prefix>/include/ovrt_helpers.h
#
# and then the directories, if — and only if — they are empty afterwards.
#
# Your installed packages live on in ~/.ovyth. They are yours, so removing
# them needs --purge; without it you are merely told they are still there.
#
# Output mirrors install.sh, and honours the same rules: decoration on a
# terminal only, nothing but plain lines when piped, NO_COLOR respected.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

prefix="${1:-$HOME/.local}"
destdir="${DESTDIR:-}"
purge=0

for a in "$@"; do
  case "$a" in
    --purge) purge=1 ;;
    --help|-h)
      printf 'usage:\n'
      printf '  ./uninstall.sh [<prefix>]        remove the install in a prefix\n'
      printf '  ./uninstall.sh --purge           also remove the package store\n'
      printf '  ./uninstall.sh --help            this\n'
      printf '\n'
      printf 'DESTDIR=<dir> ./uninstall.sh <prefix>   un-stage a package tree\n'
      exit 0
      ;;
  esac
done

# --- decoration, on a terminal only ----------------------------------------

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  M=$'\033[35m'  # magenta — Ovyth's colour
  C=$'\033[36m'  # cyan    — steps
  G=$'\033[32m'  # green   — done
  Y=$'\033[33m'  # yellow  — a warning
  D=$'\033[90m'  # dim     — secondary text
  B=$'\033[1m'   # bold
  N=$'\033[0m'
  SPARK='✦'; STEP='⬡'; OK='✓'; DOT='•'; HEART='♥'; WARN='▲'; ZZZ='z z z'
  PRETTY=1
else
  M=""; C=""; G=""; Y=""; D=""; B=""; N=""
  SPARK=""; STEP=""; OK=""; DOT=""; HEART=""; WARN=""
  PRETTY=0
fi

# `⬡ label .........` — printed at once and flushed, so a slow step still
# reads as progress rather than as silence.
step() {
  [ "$PRETTY" = 1 ] || return 0
  local label="${1:0:46}"
  local dots
  dots="$(printf '%*s' "$((46 - ${#label}))" '' | tr ' ' '.')"
  printf '   %s %s %s ' "$C$STEP$N" "$label" "$D$dots$N"
}

step_done() {
  [ "$PRETTY" = 1 ] || return 0
  local note="${1:-done}"
  printf '%s%s%s\n' "$G" "$note" "$N"
}

# Nothing was there to remove — not a failure, just a quieter goodbye.
step_skip() {
  [ "$PRETTY" = 1 ] || return 0
  printf '%snot installed%s\n' "$D" "$N"
}

# A path the way a human reads it: the prefix is implied by the context, so
# `bin/ovc` says more than `/home/you/.local/bin/ovc` in a step line.
rel() { local p="$1"; printf '%s' "${p#"$destdir$prefix"/}"; }

# A removal target, with what it was. The size in parentheses keeps the line
# one width, whatever the path length.
bullet() {
  local mark=" "
  [ "$PRETTY" = 1 ] && mark="$D$DOT$N"
  printf '       %s %s  %s(%s)%s\n' "$mark" "$1" "$D" "$2" "$N"
}

# Bytes, the way a human reads them.
human() {
  local b="$1"
  if   [ "$b" -ge 1048576 ]; then printf '%d.%d MB' $((b / 1048576)) $(((b % 1048576) * 10 / 1048576))
  elif [ "$b" -ge 1024 ];    then printf '%d.%d KB' $((b / 1024)) $(((b % 1024) * 10 / 1024))
  else                            printf '%d B' "$b"
  fi
}

# --- the two faces of Ovyth ------------------------------------------------

# Awake: the same sprite install.sh greets with, so the two scripts read as
# one character meeting you and saying goodbye.
greet() {
  [ "$PRETTY" = 1 ] || return 0
  printf '\n'
  printf '   %s\n' "$M$SPARK$N"
  printf '       /\      /\          %s O v y t h %s   %sgoodbye%s\n' \
    "$M$SPARK$N" "$M$SPARK$N" "$D" "$N"
  printf '      /  \____/  \         %sit was nice having you here%s\n' "$D" "$N"
  printf '     |  *      *  |         %slet me tidy up%s\n' "$D" "$N"
  printf "     |      w      |\n"
  printf "     |   '----'   |\n"
  printf '      \          /\n'
  printf "       '.______.'\n"
  printf '         |  |        %s\n' "$M$HEART$N"
  printf '        (_)(_)\n'
  printf '\n'
}

# Asleep: closed eyes, a purr, the heart moved to the bottom.
farewell() {
  [ "$PRETTY" = 1 ] || return 0
  printf '\n'
  printf '   %s\n' "$M$SPARK$N"
  printf '       /\      /\          %s O v y t h %s   %sgoodnight%s\n' \
    "$M$SPARK$N" "$M$SPARK$N" "$D" "$N"
  printf '      /  \____/  \         %sthank you for having me%s\n' "$D" "$N"
  printf '     |  -      -  |         %s%s%s\n' "$D" "$ZZZ" "$N"
  printf "     |      w      |\n"
  printf "     |   '----'   |\n"
  printf '      \          /\n'
  printf "       '.______.'\n"
  printf '         |  |\n'
  printf '        (_)(_)        %s\n' "$M$HEART$N"
}

# --- what is installed ------------------------------------------------------

bindir="$destdir$prefix/bin"
libdir="$destdir$prefix/lib"
incdir="$destdir$prefix/include"

# A file is a target only if it is one install.sh writes. This is the safety
# property of the whole script: nothing else in the prefix is touched.
targets=(
  "$bindir/ovc"
  "$libdir/libovrt.a"
  "$incdir/ovrt.h"
  "$incdir/ovrt_helpers.h"
)

present=()
bytes=0
for f in "${targets[@]}"; do
  if [ -f "$f" ]; then
    present+=("$f")
    bytes=$(( bytes + $(stat -c %s "$f") ))
  fi
done

greet

# Nothing installed is a quiet goodbye — but --purge still has work to do, so
# this path skips the removal rather than exiting the script.
if [ "${#present[@]}" -eq 0 ]; then
  printf '   %s nothing to remove — Ovyth is not installed in %s\n' \
    "$D$STEP$N" "$prefix"
  [ "$PRETTY" = 1 ] || printf 'nothing to remove: Ovyth is not installed in %s\n' "$prefix"
else
  step "found ${#present[@]} file$( [ "${#present[@]}" -eq 1 ] || printf s ) in $prefix"
  step_done
  for f in "${present[@]}"; do
    bullet "$(rel "$f")" "$(human "$(stat -c %s "$f")")"
  done

  for f in "${present[@]}"; do
    step "removing $(rel "$f")"
    rm -f "$f"
    step_done
  done

  # Directories go last, and only if they are empty — a directory the user also
  # keeps something else in survives.
  rmdir_if_empty() {
    [ -d "$1" ] || return 0
    if rmdir "$1" 2>/dev/null; then
      step "removing empty directory $(rel "$1")"
      step_done
    else
      step "leaving $(rel "$1") alone"
      step_done "not empty"
    fi
  }
  rmdir_if_empty "$incdir"
  rmdir_if_empty "$libdir"
  rmdir_if_empty "$bindir"

  if [ "$PRETTY" = 1 ]; then
    printf '\n   %s Ovyth is uninstalled — %s freed\n' "$G$OK$N" "$(human "$bytes")"
  else
    printf 'Ovyth is uninstalled from %s (%s freed)\n' "$prefix" "$(human "$bytes")"
  fi
fi

# --- the package store ------------------------------------------------------

store="${OVYTH_HOME:-$HOME/.ovyth}"
if [ -d "$store" ]; then
  count=0
  [ -d "$store/packages" ] &&
    count="$(find "$store/packages" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | wc -l)"
  if [ "$purge" = 1 ]; then
    # Refuse to delete something that is not a package store.
    case "$store" in
      ""|"/"|"$HOME")
        printf '\n   %s refusing to remove %s — that does not look like a store\n' \
          "$Y$WARN$N" "$store"
        exit 1
        ;;
    esac
    step "removing the package store ($store)"
    rm -rf "$store"
    step_done
    if [ "$count" -gt 0 ]; then
      printf '\n   %s %s package%s went with it\n' "$D$HEART$N" "$count" \
        "$( [ "$count" -eq 1 ] || printf s )"
    fi
  elif [ "$count" -gt 0 ]; then
    printf '\n   %s your packages are still in %s\n' "$C$DOT$N" "$store"
    printf '       %s%s installed package%s, untouched%s\n' "$D" "$count" \
      "$( [ "$count" -eq 1 ] || printf s )" "$N"
    printf '       %sremove them too with:%s  ./uninstall.sh --purge\n' "$D$B" "$N$D"
  else
    printf '\n   %s an empty package store is still in %s\n' "$D$DOT$N" "$store"
    printf '       %sremove it with:%s  ./uninstall.sh --purge\n' "$D$B" "$N$D"
  fi
fi

if [ "$PRETTY" = 1 ]; then
  farewell
  printf '\n   %s come back soon%s\n\n' "$M$HEART$N" "$N"
else
  printf 'come back soon\n'
fi

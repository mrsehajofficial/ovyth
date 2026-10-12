#!/usr/bin/env bash
# Ovyth installer.
#
#   ./install.sh                        install into ~/.local   (no sudo)
#   ./install.sh /usr/local             install into a prefix
#   DESTDIR=/tmp/pkg ./install.sh /usr/local    stage a package tree
#
# Three things are installed, laid out so `ovc` finds them with no
# configuration at all:
#
#   <prefix>/bin/ovc            the compiler
#   <prefix>/lib/libovrt.a      the runtime every compiled program links against
#   <prefix>/include/ovrt*.h    the runtime headers ovc hands to clang
#
# ovc resolves the last two relative to its own location (see
# runtime_archive_path in compiler/src/driver.cpp), so the whole prefix can be
# moved or copied and compiled programs keep working.
#
# Output. On a terminal this is a welcome from Ovyth herself and a step-by-step
# progress list. Piped — packaging, CI, a log file — it is the plain list of
# paths it has always been: no colour, no sprite, no escape codes. NO_COLOR
# keeps the structure and drops the colour, the same rule ovc follows.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

prefix="${1:-$HOME/.local}"
destdir="${DESTDIR:-}"

# --- decoration, on a terminal only ----------------------------------------

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  M=$'\033[35m'  # magenta — Ovyth's colour
  C=$'\033[36m'  # cyan    — steps
  G=$'\033[32m'  # green   — success
  Y=$'\033[33m'  # yellow  — a warning
  D=$'\033[90m'  # dim     — secondary text
  B=$'\033[1m'   # bold
  N=$'\033[0m'
  SPARK='✦'; STEP='⬡'; OK='✓'; DOT='•'; HEART='♥'; WARN='▲'
  PRETTY=1
else
  M=""; C=""; G=""; Y=""; D=""; B=""; N=""
  SPARK=""; STEP=""; OK=""; DOT=""; HEART=""
  PRETTY=0
fi

# `⬡ label .........` — printed at once and flushed, so a slow build still
# reads as progress rather than as silence.
step() {
  [ "$PRETTY" = 1 ] || return 0
  local label="${1:0:46}"
  local dots
  dots="$(printf '%*s' "$((46 - ${#label}))" '' | tr ' ' '.')"
  printf '   %s %s %s ' "$C$STEP$N" "$label" "$D$dots$N"
  t0=$(date +%s%N)
}

step_done() {
  [ "$PRETTY" = 1 ] || return 0
  local ms=$((($(date +%s%N) - t0) / 1000000)) note="done"
  if [ "$ms" -ge 1000 ]; then
    note="done ($((ms / 1000)).$(((ms % 1000) / 100))s)"
  elif [ "$ms" -gt 0 ]; then
    note="done (${ms}ms)"
  fi
  printf '%s%s%s\n' "$G" "$note" "$N"
}

# An installed path with what it is for. Always printed, pretty or not —
# packagers and curious humans both want to know where things landed.
bullet() {
  local mark=" "
  [ "$PRETTY" = 1 ] && mark="$D$DOT$N"
  printf '       %s %-26s %s%s%s\n' "$mark" "$1" "$D" "$2" "$N"
}

wordmark() {
  [ "$PRETTY" = 1 ] || return 0
  local version="$1"
  printf '\n'
  printf '   %s\n' "$M$SPARK$N"
  printf '       /\      /\          %s O v y t h %s   %s%s%s\n' \
    "$M$SPARK$N" "$M$SPARK$N" "$D" "$version" "$N"
  printf '      /  \____/  \         %sa tiny compiled%s\n' "$D" "$N"
  printf '     |  *      *  |         %slanguage%s\n' "$D" "$N"
  printf "     |      w      |\n"
  printf "     |   '----'   |\n"
  printf '      \          /\n'
  printf "       '.______.'\n"
  printf '         |  |        %s\n' "$M$HEART$N"
  printf '        (_)(_)\n'
  printf '\n'
}

# The version comes from ovc itself when it is built, and from the source when
# it is not — the sprite greets before the first build, not after it.
version="$(build/ovc version 2>/dev/null | tail -1 | awk '{print $2}')"
if [ -z "$version" ]; then
  version="$(sed -n 's/^constexpr const char\* kVersion *= *"\([^"]*\)".*/\1/p' \
    compiler/src/main.cpp | head -1)"
fi
wordmark "$version"

bindir="$destdir$prefix/bin"
libdir="$destdir$prefix/lib"
incdir="$destdir$prefix/include"

if [ "$PRETTY" = 1 ]; then
  step "building ovc and libovrt.a"
  make -s
  step_done

  step "installing into $prefix"
  install -d "$bindir" "$libdir" "$incdir"
  install -m 755 build/ovc       "$bindir/ovc"
  install -m 644 build/libovrt.a "$libdir/libovrt.a"
  install -m 644 runtime/include/ovrt.h runtime/include/ovrt_helpers.h "$incdir/"
  # The agent TUI ships as source, like every other Ovyth program: `ovc agent`
  # finds it next to the binary, so there is no second build of it.
  install -d "$destdir$prefix/share/ovyth/agent"
  install -m 644 examples/agent/tui.ov "$destdir$prefix/share/ovyth/agent/tui.ov"
  step_done
  bullet "$bindir/ovc" "the compiler"
  bullet "$libdir/libovrt.a" "linked into every compiled program"
  bullet "$incdir/ovrt.h" "the runtime API, handed to clang"
else
  echo "building ovc and libovrt.a..."
  make -s

  install -d "$bindir" "$libdir" "$incdir"
  install -m 755 build/ovc       "$bindir/ovc"
  install -m 644 build/libovrt.a "$libdir/libovrt.a"
  install -m 644 runtime/include/ovrt.h runtime/include/ovrt_helpers.h "$incdir/"
  install -d "$destdir$prefix/share/ovyth/agent"
  install -m 644 examples/agent/tui.ov "$destdir$prefix/share/ovyth/agent/tui.ov"
fi

if [ "$PRETTY" = 1 ]; then
  case ":$PATH:" in
    *":$prefix/bin:"*)
      printf '\n   %s done — %s is on your PATH\n' "$G$OK$N" "$prefix/bin" ;;
    *)
      printf '\n   %s %s/bin is not on your PATH yet\n' "$Y$WARN$N" "$prefix"
      printf '       %secho %s >> ~/.bashrc%s\n' "$D" \
        "'export PATH=\"$prefix/bin:\$PATH\"'" "$N" ;;
  esac
  printf '\n   %s try it:  %sovc init myapp && cd myapp && make run%s\n' \
    "$M$HEART$N" "$B" "$N"
else
  echo
  echo "installed:"
  echo "  $bindir/ovc"
  echo "  $libdir/libovrt.a"
  echo "  $incdir/ovrt.h"
  echo "  $prefix/share/ovyth/agent/tui.ov     the agent TUI, run with: ovc agent"

  case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *)
      echo
      echo "note: $prefix/bin is not on your PATH. add it with:"
      echo "  echo 'export PATH=\"$prefix/bin:\$PATH\"' >> ~/.bashrc"
      ;;
  esac

  echo
  echo "try it:"
  echo "  ovc init myapp && cd myapp && make run"
fi

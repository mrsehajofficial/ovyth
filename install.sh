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
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

prefix="${1:-$HOME/.local}"
destdir="${DESTDIR:-}"

echo "building ovc and libovrt.a..."
make -s

bindir="$destdir$prefix/bin"
libdir="$destdir$prefix/lib"
incdir="$destdir$prefix/include"

install -d "$bindir" "$libdir" "$incdir"
install -m 755 build/ovc           "$bindir/ovc"
install -m 644 build/libovrt.a     "$libdir/libovrt.a"
install -m 644 runtime/include/ovrt.h runtime/include/ovrt_helpers.h "$incdir/"

echo
echo "installed:"
echo "  $bindir/ovc"
echo "  $libdir/libovrt.a"
echo "  $incdir/ovrt.h"

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

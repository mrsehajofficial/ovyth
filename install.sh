#!/usr/bin/env bash
# Vayu installer.
#
#   ./install.sh                        install into ~/.local   (no sudo)
#   ./install.sh /usr/local             install into a prefix
#   DESTDIR=/tmp/pkg ./install.sh /usr/local    stage a package tree
#
# Three things are installed, laid out so `vyc` finds them with no
# configuration at all:
#
#   <prefix>/bin/vyc            the compiler
#   <prefix>/lib/libvyrt.a      the runtime every compiled program links against
#   <prefix>/include/vyrt*.h    the runtime headers vyc hands to clang
#
# vyc resolves the last two relative to its own location (see
# runtime_archive_path in compiler/src/driver.cpp), so the whole prefix can be
# moved or copied and compiled programs keep working.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

prefix="${1:-$HOME/.local}"
destdir="${DESTDIR:-}"

echo "building vyc and libvyrt.a..."
make -s

bindir="$destdir$prefix/bin"
libdir="$destdir$prefix/lib"
incdir="$destdir$prefix/include"

install -d "$bindir" "$libdir" "$incdir"
install -m 755 build/vyc           "$bindir/vyc"
install -m 644 build/libvyrt.a     "$libdir/libvyrt.a"
install -m 644 runtime/include/vyrt.h runtime/include/vyrt_helpers.h "$incdir/"

echo
echo "installed:"
echo "  $bindir/vyc"
echo "  $libdir/libvyrt.a"
echo "  $incdir/vyrt.h"

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
echo "  vyc init myapp && cd myapp && make run"

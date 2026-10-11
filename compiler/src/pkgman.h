// Ovyth :: compiler/src/pkgman.h
//
// `ovc pkg ...` — the package manager.
//
// A package is a plain directory of `.ov` files with one entry module
// (`<name>/<name>.ov`, or `<name>/main.ov`). Installing one copies it into a
// global store — $OVYTH_HOME/packages/<name>, defaulting to ~/.ovyth — and
// the module loader (ModuleLoader in main.cpp) falls back to that store when
// an `import "..."` is not found next to the importing file. An installed
// package is therefore importable from any directory on the machine, the way
// Python's site-packages works, with no per-project configuration.
//
//   ovc pkg install <name>[@version]   download + install globally
//   ovc pkg list                       every installed package
//   ovc pkg remove <name>              uninstall
//   ovc pkg search <term>              search the registry index
//   ovc pkg info <name>                registry + installed detail
//   ovc pkg update [<name>]            refresh the index / upgrade
//
// The registry is a JSON index fetched over HTTP and cached in the store for
// an hour; a file:// URL reads it straight off disk (which is how the test
// suite drives the whole pipeline offline).
#pragma once

#include <string>
#include <vector>

namespace ov {

// Dispatch `ovc pkg <sub> ...`. Returns the process exit code.
int run_pkg_command(const std::vector<std::string>& args);

// --- import resolution, used by the module loader ---------------------------

// The global package store root: $OVYTH_HOME/packages. Empty when no home
// directory can be determined, in which case the store is simply absent.
const std::string& packages_root();

// Resolve an import path against the global package store. Returns "" when no
// installed package provides it, so the caller keeps its usual local
// resolution (and its usual error) untouched. `import "pkg"` and
// `import "pkg/submodule"` are both supported.
std::string resolve_package_import(const std::string& path);

}  // namespace ov

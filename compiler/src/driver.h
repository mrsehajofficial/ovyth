// Vayu :: compiler/src/driver.h
//
// Native compilation driver: LLVM IR -> object -> executable.
//
//   AST ──(codegen)──> LLVM IR module
//                     ├─(llc)──> .o
//                     └─(clang driver)──> .o for the runtime, then link
//
// The linker step goes through the system clang driver so the produced
// executable picks up libcurl/OpenSSL/zlib and the C++ runtime without vyc
// having to know the platform's library search paths.
#pragma once

#include <string>
#include <vector>

#include "ast/ast.h"
#include "sema/sema.h"

namespace vy {

struct Options {
  std::string output;          // -o
  int opt_level = 2;           // -O2
  bool keep_ir = false;        // --keep-ir
  bool stats = false;          // --stats
  bool check_only = false;     // --check-only
  bool release = false;        // --release   (-O3, LTO, strip)
  bool target_native = false;  // --target=native (-march=native)
  bool no_strip = false;       // --no-strip  (keep symbols in release)
  std::vector<std::string> extra_args;
};

class Driver {
 public:
  Driver(std::string input, Options opt) : input_(std::move(input)), opt_(std::move(opt)) {}
  ~Driver();

  // Returns the process exit code.
  int compile(const ast::Program& program, Sema& sema, const std::string& source);

  std::string last_error() const { return error_; }

 private:
  bool emit_ir(const ast::Program& program, Sema& sema, std::string& ir_path, std::string& err);
  bool run(const std::vector<std::string>& argv, std::string& err);

  std::string input_;
  Options opt_;
  std::string error_;
  std::vector<std::string> temps_;
};

// Resolve the installed Vayu runtime (libvyrt.a) relative to the vyc binary,
// so a compiled program never needs an interpreter -- only the runtime, which
// is linked into the executable itself.
std::string runtime_archive_path();

// Directory holding vyrt.h, resolved the same way as the archive.
std::string runtime_include_dir();

}  // namespace vy
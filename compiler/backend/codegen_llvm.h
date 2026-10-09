// Ovyth :: compiler/backend/codegen_llvm.h
#pragma once

#include <string>

#include "../ast/ast.h"
#include "../sema/sema.h"

namespace ov {

// Lower the AST to LLVM IR (textual) and return it. `err` receives a
// human-readable message on failure. This is the whole native backend entry
// point: the driver then hands the module to llc/clang.
std::string emit_llvm_ir(const ast::Program& program, Sema& sema,
                         const std::string& source, std::string& err);

}  // namespace ov
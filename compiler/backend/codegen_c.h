// Ovyth :: compiler/backend/codegen_c.h
//
// Emits a self-contained C source file that calls into the C runtime.
// The driver compiles the emitted .c with clang and links against
// libovrt.a, producing a native executable.
#pragma once

#include <string>

#include "../ast/ast.h"
#include "../sema/sema.h"

namespace ov {

// Emits C source implementing `program`.  err receives a message on failure.
std::string emit_c_source(const ast::Program& program, Sema& sema,
                          const std::string& source, std::string& err);

}  // namespace ov

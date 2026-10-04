#include "codegen_llvm.h"

namespace vy {

std::string emit_llvm_ir(const ast::Program& program, Sema& sema,
                         const std::string& source, std::string& err) {
  (void)program;
  (void)sema;
  (void)source;
  err = "native backend not wired up yet -- use `vyc run`";
  return "";
}

}  // namespace vy
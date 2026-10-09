#include "codegen_llvm.h"

namespace ov {

std::string emit_llvm_ir(const ast::Program& program, Sema& sema,
                         const std::string& source, std::string& err) {
  (void)program;
  (void)sema;
  (void)source;
  err = "native backend not wired up yet -- use `ovc run`";
  return "";
}

}  // namespace ov
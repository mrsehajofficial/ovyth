// Vayu :: backend/interp.h
//
// The tree-walking backend. It exists so the language can be *used* while the
// AOT backend is being built: `vyc run foo.vy` gives a correct, fast-enough
// answer for every semantic question, and the stdlib lives here once so the
// LLVM backend reuses the same behaviour instead of reimplementing it.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../ast/ast.h"

extern "C" {
#include "vyrt.h"
}

namespace vy {

// std::string -> VyValue, without the (ptr,len) dance at every call site.
inline VyValue vy_s(const std::string& s) { return vy_str_of(s.data(), s.size()); }
inline VyValue vy_s(const char* s)         { return vy_str_val(s); }

class Interp {
 public:
  Interp(const ast::Program& program, std::string filename);
  ~Interp();

  int run();  // returns the process exit code

  enum class Flow { Normal, Break, Continue, Return, Exit };
  struct Signal {
    Flow flow = Flow::Normal;
    VyValue value{};
  };
  // A throw is a C++ exception carrying the Vayu error value.
  struct Throw {
    VyValue value;
  };

  // --- environment ------------------------------------------------------
  struct Env {
    Env* parent = nullptr;
    std::unordered_map<std::string, VyValue> vars;
    explicit Env(Env* p) : parent(p) {}
    VyValue* find(const std::string& n) {
      for (Env* e = this; e; e = e->parent) {
        auto it = e->vars.find(n);
        if (it != e->vars.end()) return &it->second;
      }
      return nullptr;
    }
  };

  struct FnDef {
    const ast::Stmt* decl = nullptr;
    Env* closure = nullptr;
    std::string name;
  };

  struct ClosureObj {
    const ast::Expr* expr = nullptr;
    Env* env = nullptr;
    std::string name;
  };

  // --- statements / expressions -----------------------------------------
  // public: the builtin table evaluates its arguments through this
  VyValue eval(const ast::Expr* e, Env& env);

 private:
  void exec_block(const ast::StmtList& body, Env& env);
  void exec_stmt(const ast::Stmt* s, Env& env);

  VyValue call_value(VyValue callee, const ast::Expr* site, Env& env,
                     const std::vector<VyValue>& args,
                     const std::vector<ast::NamedArg>& named);
  VyValue call_function(const FnDef& fn, const ast::ExprList& arg_exprs, Env& env);
  VyValue call_closure(const ClosureObj& cl, const std::vector<VyValue>& args);
  VyValue call_builtin(const std::string& qualified, const ast::Expr* site, Env& env,
                       const ast::ExprList& args,
                       const std::vector<ast::NamedArg>& named, bool* handled);

  VyValue index_get(VyValue base, VyValue idx);
  VyValue member_get(VyValue base, const std::string& name, const ast::Expr* site);
  VyValue interpolate(const ast::Expr* e, Env& env);

  void assign_to(ast::Expr* target, VyValue value, Env& env);

  // --- helpers ------------------------------------------------------------
  static VyValue str_of(VyValue v) { return vy_str(vy_render(v)); }

  /* Shadow stack. Building a list or a map allocates, so every value already
   * stored in it must stay reachable while the next element is evaluated --
   * the collector may run at any allocation. One open slot per nesting level
   * is enough, and it is cheap. */
  VyValue* open_root();
  void close_root();
  std::vector<VyValue*> root_slots_;

  const ast::Program& prog_;
  std::string file_;
  Env* globals_;
  std::unordered_map<std::string, FnDef> functions_;
  Signal signal_;
  int exit_code_ = 0;
  int depth_ = 0;
};

}  // namespace vy
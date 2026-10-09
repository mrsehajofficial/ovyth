// Ovyth :: backend/interp.h
//
// The tree-walking backend. It exists so the language can be *used* while the
// AOT backend is being built: `ovc run foo.ov` gives a correct, fast-enough
// answer for every semantic question, and the stdlib lives here once so the
// LLVM backend reuses the same behaviour instead of reimplementing it.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ast/ast.h"

extern "C" {
#include "ovrt.h"
}

namespace ov {

// std::string -> OvValue, without the (ptr,len) dance at every call site.
inline OvValue ov_s(const std::string& s) { return ov_str_of(s.data(), s.size()); }
inline OvValue ov_s(const char* s)         { return ov_str_val(s); }

class Interp {
 public:
  Interp(const ast::Program& program, std::string filename);
  ~Interp();

  int run();  // returns the process exit code

  enum class Flow { Normal, Break, Continue, Return, Exit };
  struct Signal {
    Flow flow = Flow::Normal;
    OvValue value{};
  };
  // A throw is a C++ exception carrying the Ovyth error value.
  struct Throw {
    OvValue value;
  };

  // --- environment ------------------------------------------------------
  struct Env : public std::enable_shared_from_this<Env> {
    std::shared_ptr<Env> parent;
    std::unordered_map<std::string, OvValue> vars;
    Env* prev_env = nullptr;
    Env* next_env = nullptr;
    explicit Env(std::shared_ptr<Env> p = nullptr);
    ~Env();
    OvValue* find(const std::string& n) {
      for (Env* e = this; e; e = e->parent.get()) {
        auto it = e->vars.find(n);
        if (it != e->vars.end()) return &it->second;
      }
      return nullptr;
    }
  };

  struct Root {
    OvValue* slot = nullptr;
    Root(OvValue v = ov_nil());
    ~Root();
    OvValue get() const { return *slot; }
    void set(OvValue v) { *slot = v; }
    operator OvValue() const { return *slot; }
  };

  struct FnDef {
    const ast::Stmt* decl = nullptr;
    std::shared_ptr<Env> closure;
    std::string name;
  };

  struct ClosureObj {
    const ast::Expr* expr = nullptr;
    std::shared_ptr<Env> env;
    std::string name;
  };

  // --- statements / expressions -----------------------------------------
  // public: the builtin table evaluates its arguments through this
  OvValue eval(const ast::Expr* e, std::shared_ptr<Env> env);

 private:
  void exec_block(const ast::StmtList& body, std::shared_ptr<Env> env);
  void exec_stmt(const ast::Stmt* s, std::shared_ptr<Env> env);

  OvValue call_value(OvValue callee, const ast::Expr* site, std::shared_ptr<Env> env,
                     const std::vector<OvValue>& args,
                     const std::vector<ast::NamedArg>& named);
  OvValue call_function(const FnDef& fn, const ast::ExprList& arg_exprs, std::shared_ptr<Env> env);
  OvValue call_closure(const ClosureObj& cl, const std::vector<OvValue>& args);
  OvValue call_builtin(const std::string& qualified, const ast::Expr* site, std::shared_ptr<Env> env,
                       const ast::ExprList& args,
                       const std::vector<ast::NamedArg>& named, bool* handled);

  OvValue index_get(OvValue base, OvValue idx);
  OvValue member_get(OvValue base, const std::string& name, const ast::Expr* site);
  OvValue interpolate(const ast::Expr* e, std::shared_ptr<Env> env);

  void assign_to(ast::Expr* target, OvValue value, std::shared_ptr<Env> env);

  // --- helpers ------------------------------------------------------------
  static OvValue str_of(OvValue v) { return ov_str(ov_render(v)); }

  /* Shadow stack. Building a list or a map allocates, so every value already
   * stored in it must stay reachable while the next element is evaluated --
   * the collector may run at any allocation. One open slot per nesting level
   * is enough, and it is cheap. */
  OvValue* open_root();
  void close_root();
  std::vector<OvValue*> root_slots_;

  static void gc_scan();
  static std::deque<OvValue> root_stack_;
  static Env* active_envs_head_;

  const ast::Program& prog_;
  std::string file_;
  std::shared_ptr<Env> globals_;
  std::unordered_map<std::string, FnDef> functions_;
  Signal signal_;
  int exit_code_ = 0;
  int depth_ = 0;
};

}  // namespace ov
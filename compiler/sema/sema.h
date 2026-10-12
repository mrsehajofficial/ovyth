// Ovyth :: sema/sema.h
// Type inference + static checks. No annotations required; annotations are
// honoured when present (function greet(name: String) -> String { ... }).
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ast/ast.h"

namespace ov {

enum class Ty {
  Unknown, Nil, Bool, Int, Float, Str, List, Map, Func, Any, Never, Error
};

const char* ty_name(Ty t);

struct FuncSig {
  std::string name;
  std::vector<std::string> params;
  Ty ret = Ty::Unknown;
  bool variadic = false;
  int decl_line = 0;
};

struct Scope {
  std::unordered_map<std::string, Ty> vars;
  bool is_function = false;
};

// Type refinement: tracks whether a variable is *proven* to be Int/Float
// (not just possibly), so the codegen can lower it to a raw register.
struct TyInfo {
  Ty ty = Ty::Unknown;
  bool is_proven_int = false;   // definitely Int, never widened to Float
  bool is_proven_float = false; // definitely Float
};

class Sema {
 public:
  // Returns true when the program is free of errors.
  bool run(ast::Program& program);

  const std::vector<Diagnostic>& diagnostics() const { return diags_; }
  const std::unordered_map<std::string, FuncSig>& functions() const { return funcs_; }

  // Inferred type of a top-level variable, for `ovc --types`.
  std::unordered_map<std::string, Ty> globals;
  
  // Proven type info for codegen optimization.
  //
  // Keyed by proven_key(): a global is program-wide and keyed by its bare
  // name, a local by the function that owns it. The same name can be an int
  // counter in one function and a string in another, and a flat per-name map
  // silently applies whichever was analysed last to both — which is how a
  // float local came to be emitted as int64_t.
  std::unordered_map<std::string, TyInfo> proven_types;

  // Names bound to a list built only from proven-int pushes.
  //
  // This is what lets codegen emit an Int64Array instead of a boxed list for
  // `xs = []` followed by `xs.push(i)`: 8 bytes per element instead of 16, no
  // box on the push, and nothing for the collector to scan. Keyed like
  // proven_types, and an entry is *removed* the moment something disqualifies
  // the name — a reassignment, or a single push whose argument is not
  // provably an int.
  std::unordered_map<std::string, bool> int_coll;

  // Names with any assignment that is not provably an int, gathered by a
  // flow-insensitive pre-pass before the body is checked.
  //
  // Sema walks statements in source order, so a `i = 2.5` that *follows* a
  // push in source but executes *before* it at runtime (a loop's back edge)
  // would not have been seen yet when the push was checked. That is exactly
  // how a float would end up in an Int64Array, where it is silently
  // truncated. Scanning the whole body first closes the gap.
  std::unordered_set<std::string> non_int_assigned_;

  // The function whose body is being checked; empty at top level.
  std::string cur_fn_;

  // Key for proven_types / int_coll. Public because codegen has to look a
  // name up under the function it is emitting -- the whole point of the keying
  // is that a name in one function is unrelated to the same name in another.
  std::string proven_key(const std::string& name) const {
    return cur_fn_.empty() ? name : cur_fn_ + "/" + name;
  }

  // Is `name` an int collection inside function `fn` ("" for top level)?
  //
  // The function is passed in rather than taken from cur_fn_, because codegen
  // asks this while emitting one specific function -- and by then sema's run
  // is over and cur_fn_ is whatever was checked last. Asking sema itself for
  // the key made every function-scoped lookup miss, which is why only a
  // top-level `xs = []` ever became an Int64Array.
  bool is_int_collection(const std::string& fn, const std::string& name) const {
    auto it = int_coll.find(fn.empty() ? name : fn + "/" + name);
    // The value, not the presence: a retired entry is left in the map as
    // `false` so a later check can see it was considered and rejected.
    // Counting instead of reading here made every retirement a no-op, and a
    // `push(1.5)` slipped into an Int64Array as a truncated 1.
    return it != int_coll.end() && it->second;
  }

 private:
  void scan_int_assignments(const ast::StmtList& body);

  void error(const ast::Pos& pos, const std::string& msg);
  void note(const ast::Pos& pos, const std::string& msg);

  void push_scope(bool is_function = false);
  void pop_scope();

  Ty check_expr(ast::Expr* e);
  Ty check_stmt(ast::Stmt* s);
  Ty check_block(const ast::StmtList& body);

  Ty check_call(ast::Expr* e);
  Ty check_binary(ast::Expr* e);
  Ty check_unary(ast::Expr* e);

  bool lookup(const std::string& name, Ty* out);
  void  define(const std::string& name, Ty t);
  
  // Define with proven type info
  void define(const std::string& name, Ty t, bool proven_int, bool proven_float);

  Ty  type_from(const ast::TypeExpr* t);
  Ty  unify(Ty a, Ty b);
  bool is_numeric(Ty t);
  bool is_proven_int_expr(ast::Expr* e);  // for proven-int optimization

  std::vector<Scope> scopes_;
  std::unordered_map<std::string, FuncSig> funcs_;
  std::vector<Diagnostic> diags_;
  std::vector<Ty> loop_stack_;
  Ty current_return_ = Ty::Unknown;
  bool in_generator_ = false;
  int errors_ = 0;
};

}  // namespace ov
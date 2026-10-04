// Vayu :: sema/sema.h
// Type inference + static checks. No annotations required; annotations are
// honoured when present (function greet(name: String) -> String { ... }).
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../ast/ast.h"

namespace vy {

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

class Sema {
 public:
  // Returns true when the program is free of errors.
  bool run(ast::Program& program);

  const std::vector<Diagnostic>& diagnostics() const { return diags_; }
  const std::unordered_map<std::string, FuncSig>& functions() const { return funcs_; }

  // Inferred type of a top-level variable, for `vyc --types`.
  std::unordered_map<std::string, Ty> globals;

 private:
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

  Ty  type_from(const ast::TypeExpr* t);
  Ty  unify(Ty a, Ty b);
  bool is_numeric(Ty t);

  std::vector<Scope> scopes_;
  std::unordered_map<std::string, FuncSig> funcs_;
  std::vector<Diagnostic> diags_;
  std::vector<Ty> loop_stack_;
  Ty current_return_ = Ty::Unknown;
  bool in_generator_ = false;
  int errors_ = 0;
};

}  // namespace vy
// Ovyth :: ast/ast.h
// AST node definitions. Arena-allocated by the parser (see ast/arena.h).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../lexer/diagnostic.h"
#include "../lexer/token.h"

namespace ov {
namespace ast {

struct Expr;
struct Stmt;

using ExprList = std::vector<Expr*>;
using StmtList = std::vector<Stmt*>;

struct Pos {
  int line = 0;
  int col = 0;
  std::string file;
  std::string to_string() const {
    return file + ":" + std::to_string(line) + ":" + std::to_string(col);
  }
};

// ---------------------------------------------------------------------------
// Types (syntax only -- optional annotations; the checker infers the rest)
// ---------------------------------------------------------------------------
struct TypeExpr {
  enum class Kind { Named, List, Map } kind = Kind::Named;
  std::string name;              // Named: String / Int / Float / Bool / Any ...
  TypeExpr* element = nullptr;   // List element type
  TypeExpr* value = nullptr;     // Map value type
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------
enum class ExprKind {
  IntLit,
  FloatLit,
  StringLit,
  BoolLit,
  NullLit,
  Identifier,
  ListLit,
  MapLit,
  Interp,          // "text {expr} text"
  Unary,           // op, right        (not, -, ~)
  Binary,          // op, left, right
  Logical,         // and / or (short circuit)
  Assign,          // target, value
  Ternary,         // cond, then, else
  Call,            // callee, args, named_args
  Index,           // object, index
  Member,          // object, name           (response.json, .body, .status)
  Slice,           // object, lo, hi
  ListComp,        // element, generators     [e for x in it if cond]
  Closure,         // params, body  (anonymous function)
  Cast,            // expr as Type
  Error,
};

struct Param {
  std::string name;
  TypeExpr* type = nullptr;         // optional annotation:  name: String
  struct Expr* default_value = nullptr;
  int line = 0;
};

struct NamedArg {
  std::string name;
  Expr* value = nullptr;
};

struct Generator {
  std::string var;
  Expr* iterable = nullptr;
  Expr* cond = nullptr;   // optional `if` filter
  int line = 0;
};

struct Expr {
  ExprKind kind;
  Pos pos;

  // literals
  int64_t ival = 0;
  double fval = 0.0;
  bool bval = false;
  std::string sval;

  std::string name;         // Identifier / Member field
  Tok op = Tok::EOF_;       // Unary / Binary / Logical / Assign compound op

  Expr* a = nullptr;        // generic child slots (see below)
  Expr* b = nullptr;
  Expr* c = nullptr;

  ExprList items;                    // ListLit elements / Interp literal parts
  ExprList parts;                    // Interp expression parts
  std::vector<std::pair<Expr*, Expr*>> fields;  // MapLit: (key expr, value expr)
  ExprList args;                     // Call positional args
  std::vector<NamedArg> named_args; // Call named args  (json = ..., headers = ...)
  std::vector<Param> params;         // Closure params
  StmtList body;                     // Closure body
  std::vector<Generator> generators;// ListComp
  TypeExpr* type = nullptr;          // Cast target / Closure return type

  std::string str() const;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------
enum class StmtKind {
  ExprStmt,
  VarDecl,
  Assign,
  FuncDecl,
  Return,
  If,
  While,
  For,
  Block,
  Break,
  Continue,
  Try,
  Throw,
  Debug,
  Import,
};

struct Stmt {
  StmtKind kind;
  Pos pos;

  // VarDecl
  std::vector<std::string> names;
  std::vector<Expr*> values;
  std::vector<TypeExpr*> types;

  // generic expression slots
  Expr* expr = nullptr;
  Expr* expr2 = nullptr;
  Expr* expr3 = nullptr;

  StmtList body;
  StmtList else_body;
  StmtList init;        // For init clause
  std::vector<std::string> iter_vars;   // For `for a, b in ...`

  // FuncDecl
  std::string name;
  std::vector<Param> params;
  TypeExpr* return_type = nullptr;
  bool is_variadic = false;

  // Try
  std::string catch_var;

  // Import
  std::string import_path;  // `import "util.ov"`

  // compound assign op
  Tok op = Tok::ASSIGN;

  std::string str() const;
};

struct Program {
  StmtList statements;
  std::vector<Diagnostic> diags;
};

// AST dumper used by `ovc --dump-ast`.
void dump_ast(const Program& p, std::string& out);

}  // namespace ast
}  // namespace ov
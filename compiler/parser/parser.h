// Ovyth :: parser/parser.h
// Recursive-descent parser producing the AST.
#pragma once

#include <string>
#include <vector>

#include "../ast/ast.h"
#include "../lexer/lexer.h"

namespace ov {

// Thrown to abandon a parse once a diagnostic has been recorded. See
// Parser::error for why in-place recovery is not safe.
struct ParseAbort {};

class Parser {
 public:
  Parser(std::vector<Token> toks, std::string filename)
      : toks_(std::move(toks)), file_(std::move(filename)) {}

  ast::Program parse();

 private:
  // --- token stream helpers ---
  const Token& peek(int off = 0) const;
  const Token& previous() const { return toks_[prev_]; }
  bool check(Tok k) const { return peek().kind == k; }
  bool check_next(Tok k) const { return peek(1).kind == k; }
  bool at_end() const { return check(Tok::EOF_); }
  bool match(Tok k);
  bool match(Tok a, Tok b);
  bool match(Tok a, Tok b, Tok c);
  const Token& consume(Tok k, const char* what);
  const Token& advance();
  // Both record a diagnostic and then unwind (see Parser::error).
  [[noreturn]] void error(const Token&, const std::string& msg);
  [[noreturn]] void error_at_current(const std::string& msg);

  // --- grammar ---
  ast::StmtList statement_list();
  ast::Stmt* statement();
  ast::Stmt* var_decl();
  ast::Stmt* func_decl(bool is_pub = false);
  ast::Stmt* if_stmt();
  ast::Stmt* while_stmt();
  ast::Stmt* for_stmt();
  ast::Stmt* try_stmt();
  ast::Stmt* return_stmt();
  ast::Stmt* block();

  ast::Expr* expression();
  ast::Expr* assign();
  ast::Expr* ternary();
  ast::Expr* or_expr();
  ast::Expr* and_expr();
  ast::Expr* bit_or();
  ast::Expr* bit_xor();
  ast::Expr* bit_and();
  ast::Expr* equality();
  ast::Expr* comparison();
  ast::Expr* shift();
  ast::Expr* power();
  ast::Expr* term();
  ast::Expr* multiplicative();
  ast::Expr* unary();
  ast::Expr* postfix();
  ast::Expr* primary();
  ast::Expr* interpolation(const Token& start);
  // Speculatively parse a `{...}` hole; nullptr when the text is not a
  // complete expression (which keeps literal braces, e.g. JSON, intact).
  ast::Expr* parse_hole(const std::string& text, const Token& origin);
  ast::Expr* list_or_map_literal();
  ast::TypeExpr* type_expr();
  std::vector<ast::Param> param_list(bool* variadic);

  // `for` comprehension detection: at '[' with a following `for` before any ']'
  bool is_list_comprehension() const;

  ast::Expr* mk(ast::ExprKind k, const Token& t);

  std::vector<Token> toks_;
  size_t cur_ = 0;
  size_t prev_ = 0;
  std::string file_;
  ast::Program program_;
  int loop_depth_ = 0;
  int fn_depth_ = 0;
};

}  // namespace ov
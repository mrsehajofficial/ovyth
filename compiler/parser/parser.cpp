#include "parser.h"

#include "../lexer/lexer.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ov {

using namespace ast;

namespace {
// throw-away expression for error recovery
Expr* error_expr(const Token& t) {
  auto* e = new Expr();
  e->kind = ExprKind::Error;
  e->pos = Pos{t.line, t.col, ""};
  return e;
}
Stmt* error_stmt(const Token& t) {
  auto* s = new Stmt();
  s->kind = StmtKind::Block;  // empty
  s->pos = Pos{t.line, t.col, ""};
  return s;
}
}  // namespace

// ---------------------------------------------------------------------------
// token helpers
// ---------------------------------------------------------------------------
const Token& Parser::peek(int off) const {
  size_t i = cur_ + (size_t)off;
  return i < toks_.size() ? toks_[i] : toks_.back();
}

const Token& Parser::advance() {
  prev_ = cur_;
  if (cur_ + 1 < toks_.size()) cur_++;
  return toks_[prev_];
}

bool Parser::match(Tok k) { if (check(k)) { advance(); return true; } return false; }
bool Parser::match(Tok a, Tok b) {
  if (check(a) && check_next(b)) { advance(); advance(); return true; }
  return false;
}
bool Parser::match(Tok a, Tok b, Tok c) {
  if (check(a) && peek(1).kind == b && peek(2).kind == c) {
    advance(); advance(); advance();
    return true;
  }
  return false;
}

const Token& Parser::consume(Tok k, const char* what) {
  if (check(k)) return advance();
  error(peek(), std::string("expected '") + what + "' but found '" +
                       tok_name(peek().kind) + "'");
}

void Parser::error(const Token& t, const std::string& msg) {
  std::string full = msg;
  if (t.kind != Tok::EOF_ && !t.text.empty()) {
    full += " (got '" + std::string(t.text) + "')";
  }
  if (t.kind == Tok::EOF_) full += " (end of file)";
  program_.diags.push_back(Diagnostic{file_, t.line, t.col, full, DiagKind::Error});
  /* Unwind out of the recursive-descent parser.
   *
   * Recovering in place is not safe here: the caller has usually just failed
   * to consume a token, so cur_ still points at the offending token and the
   * enclosing loop would re-enter the same production forever -- an unterminated
   * string such as print("{\") spins until the process runs out of memory.
   * Recording the diagnostic and unwinding gives a clean, bounded failure; the
   * driver reports it and exits. */
  throw ParseAbort{};
}

void Parser::error_at_current(const std::string& msg) { error(peek(), msg); }

Expr* Parser::mk(ExprKind k, const Token& t) {
  auto* e = new Expr();
  e->kind = k;
  e->pos = Pos{t.line, t.col, file_};
  return e;
}

// ---------------------------------------------------------------------------
// program / statements
// ---------------------------------------------------------------------------
Program Parser::parse() {
  // The `cur_ == before` guard below guarantees forward progress between
  // statements, but it cannot help a production that fails *without*
  // consuming (error() unwinds for exactly that reason). Catch the unwind so
  // the caller still gets a program plus the recorded diagnostic.
  try {
    while (!check(Tok::EOF_)) {
      if (match(Tok::SEMICOLON)) continue;
      size_t before = cur_;
      Stmt* s = statement();
      if (s) program_.statements.push_back(s);
      if (cur_ == before) advance();  // guarantee progress
    }
  } catch (const ParseAbort&) {
    // program_.diags already holds the message.
  }
  return std::move(program_);
}

StmtList Parser::statement_list() {
  StmtList list;
  while (!check(Tok::RBRACE) && !check(Tok::EOF_)) {
    if (match(Tok::SEMICOLON)) continue;
    size_t before = cur_;
    Stmt* s = statement();
    if (s) list.push_back(s);
    if (cur_ == before) advance();
  }
  return list;
}

Stmt* Parser::block() {
  const Token& open = consume(Tok::LBRACE, "{");
  Stmt* s = new Stmt();
  s->kind = StmtKind::Block;
  s->pos = Pos{open.line, open.col, file_};
  s->body = statement_list();
  consume(Tok::RBRACE, "}");
  return s;
}

Stmt* Parser::statement() {
  switch (peek().kind) {
    case Tok::KW_FUNCTION: return func_decl();
    case Tok::KW_RETURN: return return_stmt();
    case Tok::KW_IF: return if_stmt();
    case Tok::KW_WHILE: return while_stmt();
    case Tok::KW_FOR: return for_stmt();
    case Tok::KW_TRY: return try_stmt();
    case Tok::KW_IMPORT: return import_stmt();
    case Tok::LBRACE: return block();
    case Tok::KW_BREAKPOINT: {
      const Token& t = advance();
      Stmt* s = new Stmt();
      s->kind = StmtKind::Debug;
      s->pos = Pos{t.line, t.col, file_};
      return s;
    }
    case Tok::KW_THROW: {
      const Token& t = advance();
      Stmt* s = new Stmt();
      s->kind = StmtKind::Throw;
      s->pos = Pos{t.line, t.col, file_};
      if (!check(Tok::SEMICOLON) && !check(Tok::RBRACE) && !check(Tok::EOF_)) {
        s->expr = expression();
      }
      match(Tok::SEMICOLON);
      return s;
    }
    case Tok::KW_BREAK: {
      const Token& t = advance();
      if (loop_depth_ == 0)
        error(t, "'break' used outside of a loop");
      Stmt* s = new Stmt();
      s->kind = StmtKind::Break;
      s->pos = Pos{t.line, t.col, file_};
      return s;
    }
    case Tok::KW_CONTINUE: {
      const Token& t = advance();
      if (loop_depth_ == 0)
        error(t, "'continue' used outside of a loop");
      Stmt* s = new Stmt();
      s->kind = StmtKind::Continue;
      s->pos = Pos{t.line, t.col, file_};
      return s;
    }
    case Tok::KW_ENUM: {
      // enum Color { Red, Green }  -- lowered to integer constants
      const Token& t = advance();
      consume(Tok::IDENT, "enum name");
      std::string enum_name(previous().text.data(), previous().text.size());
      consume(Tok::LBRACE, "{");
      Stmt* s = new Stmt();
      s->kind = StmtKind::Block;
      s->pos = Pos{t.line, t.col, file_};
      int idx = 0;
      while (!check(Tok::RBRACE) && !check(Tok::EOF_)) {
        if (!check(Tok::IDENT)) { error_at_current("expected enum member name"); break; }
        const Token& m = advance();
        Stmt* d = new Stmt();
        d->kind = StmtKind::VarDecl;
        d->pos = Pos{m.line, m.col, file_};
        d->names.push_back(enum_name + "." + std::string(m.text));
        Expr* v = new Expr();
        v->kind = ExprKind::IntLit;
        v->ival = idx++;
        v->pos = d->pos;
        d->values.push_back(v);
        s->body.push_back(d);
        if (!match(Tok::COMMA)) break;
      }
      consume(Tok::RBRACE, "}");
      return s;
    }
    default: break;
  }
  return var_decl();  // also handles plain expression statements
}

Stmt* Parser::var_decl() {
  const Token& start = peek();

  // How many names does this declaration bind? 0 means it is a plain
  // expression statement.  Detected by lookahead so `a, b = 1, 2` parses as a
  // two-name declaration rather than as an expression followed by a stray
  // comma.
  int n_names = 0;
  if (check(Tok::IDENT)) {
    Tok n = peek(1).kind;
    if (n == Tok::ASSIGN || n == Tok::COLON) {
      n_names = 1;
    } else if (n == Tok::COMMA) {
      size_t i = 2;
      while (peek(i).kind == Tok::IDENT) {
        i++;
        if (peek(i).kind == Tok::COMMA) i++; else break;
      }
      if (peek(i).kind == Tok::ASSIGN || peek(i).kind == Tok::COLON)
        n_names = (int)((i + 1) / 2);
    }
  }

  if (n_names == 0) {
    Stmt* s = new Stmt();
    s->kind = StmtKind::ExprStmt;
    s->pos = Pos{start.line, start.col, file_};
    s->expr = expression();
    return s;
  }

  // Declaration: bind `n_names` names, each optionally type-annotated.
  std::vector<std::string> names;
  std::vector<TypeExpr*> types;
  std::vector<Expr*> values;
  for (int k = 0; k < n_names; k++) {
    names.push_back(std::string(advance().text));  // consume the identifier
    if (check(Tok::COLON)) { advance(); types.push_back(type_expr()); }
    else types.push_back(nullptr);
    if (k + 1 < n_names) match(Tok::COMMA);
  }
  if (check(Tok::ASSIGN)) {
    advance();
    values.push_back(expression());
    while (check(Tok::COMMA) && !check(Tok::RPAREN) && !check(Tok::RBRACE) &&
           !check(Tok::RBRACKET)) {
      if ((int)values.size() >= n_names) break;
      advance();
      values.push_back(expression());
    }
  }
  if (values.empty()) {
    error(start, "variable '" + names[0] +
                       "' has no value (Ovyth has no implicit null initialiser)");
    while ((int)values.size() < n_names) values.push_back(error_expr(start));
  }
  // NOTE: a single value for multiple names is left as-is (`a, b = [1, 2]`
  // unpacks in the backend); padding with nullptrs would hide that case.

  Stmt* s = new Stmt();
  s->kind = StmtKind::VarDecl;
  s->pos = Pos{start.line, start.col, file_};
  s->names = std::move(names);
  s->types = std::move(types);
  s->values = std::move(values);
  return s;
}

std::vector<Param> Parser::param_list(bool* variadic) {
  std::vector<Param> params;
  if (variadic) *variadic = false;
  consume(Tok::LPAREN, "(");
  while (!check(Tok::RPAREN) && !check(Tok::EOF_)) {
    if (match(Tok::ELLIPSIS)) {
      if (variadic) *variadic = true;
    }
    if (!check(Tok::IDENT)) { error_at_current("expected parameter name"); break; }
    const Token& p = advance();
    Param param;
    param.name = std::string(p.text);
    param.line = p.line;
    if (match(Tok::COLON)) param.type = type_expr();
    if (match(Tok::ASSIGN)) {
      Expr* d = expression();
      param.default_value = d;
    }
    params.push_back(std::move(param));
    if (!match(Tok::COMMA)) break;
  }
  consume(Tok::RPAREN, ")");
  return params;
}

Stmt* Parser::func_decl(bool) {
  const Token& kw = advance();  // 'function'
  if (!check(Tok::IDENT)) {
    error_at_current("expected function name after 'function'");
    return error_stmt(kw);
  }
  const Token& nameTok = advance();
  Stmt* s = new Stmt();
  s->kind = StmtKind::FuncDecl;
  s->pos = Pos{nameTok.line, nameTok.col, file_};
  s->name = std::string(nameTok.text);
  s->params = param_list(&s->is_variadic);
  if (match(Tok::ARROW)) s->return_type = type_expr();
  consume(Tok::LBRACE, "{");
  fn_depth_++;
  s->body = statement_list();
  fn_depth_--;
  consume(Tok::RBRACE, "}");
  return s;
}

Stmt* Parser::return_stmt() {
  const Token& t = advance();
  Stmt* s = new Stmt();
  s->kind = StmtKind::Return;
  s->pos = Pos{t.line, t.col, file_};
  if (fn_depth_ == 0) error(t, "'return' used outside of a function");
  if (!check(Tok::SEMICOLON) && !check(Tok::RBRACE) && !check(Tok::EOF_)) {
    s->expr = expression();
  }
  return s;
}

Stmt* Parser::if_stmt() {
  const Token& t = advance();  // 'if'
  Stmt* s = new Stmt();
  s->kind = StmtKind::If;
  s->pos = Pos{t.line, t.col, file_};
  s->expr = expression();
  consume(Tok::LBRACE, "{");
  s->body = statement_list();
  consume(Tok::RBRACE, "}");
  if (match(Tok::KW_ELSE)) {
    if (check(Tok::KW_IF)) {
      s->else_body.push_back(if_stmt());
    } else {
      consume(Tok::LBRACE, "{");
      s->else_body = statement_list();
      consume(Tok::RBRACE, "}");
    }
  }
  return s;
}

Stmt* Parser::while_stmt() {
  const Token& t = advance();
  Stmt* s = new Stmt();
  s->kind = StmtKind::While;
  s->pos = Pos{t.line, t.col, file_};
  s->expr = expression();
  consume(Tok::LBRACE, "{");
  loop_depth_++;
  s->body = statement_list();
  loop_depth_--;
  consume(Tok::RBRACE, "}");
  return s;
}

Stmt* Parser::for_stmt() {
  const Token& t = advance();  // 'for'
  Stmt* s = new Stmt();
  s->kind = StmtKind::For;
  s->pos = Pos{t.line, t.col, file_};
  if (!check(Tok::IDENT)) { error_at_current("expected loop variable name"); return error_stmt(t); }
  s->iter_vars.push_back(std::string(advance().text));
  while (match(Tok::COMMA) && check(Tok::IDENT)) {
    s->iter_vars.push_back(std::string(advance().text));
  }
  consume(Tok::KW_IN, "in");
  s->expr = expression();
  consume(Tok::LBRACE, "{");
  loop_depth_++;
  s->body = statement_list();
  loop_depth_--;
  consume(Tok::RBRACE, "}");
  return s;
}

Stmt* Parser::try_stmt() {
  const Token& t = advance();
  Stmt* s = new Stmt();
  s->kind = StmtKind::Try;
  s->pos = Pos{t.line, t.col, file_};
  consume(Tok::LBRACE, "{");
  s->body = statement_list();
  consume(Tok::RBRACE, "}");
  if (match(Tok::KW_CATCH)) {
    if (check(Tok::IDENT)) s->catch_var = std::string(advance().text);
    consume(Tok::LBRACE, "{");
    s->else_body = statement_list();
    consume(Tok::RBRACE, "}");
  } else {
    error(previous(), "'try' without 'catch' is not supported in v0.1");
  }
  return s;
}

Stmt* Parser::import_stmt() {
  const Token& t = advance();  // 'import'
  Stmt* s = new Stmt();
  s->kind = StmtKind::Import;
  s->pos = Pos{t.line, t.col, file_};
  if (check(Tok::STRING)) {
    s->import_path = std::string(advance().str);
  } else {
    error_at_current("expected a string path after 'import'");
  }
  match(Tok::SEMICOLON);
  return s;
}

// ---------------------------------------------------------------------------
// types
// ---------------------------------------------------------------------------
TypeExpr* Parser::type_expr() {
  if (check(Tok::LBRACKET)) {  // List<T>
    advance();
    TypeExpr* t = new TypeExpr();
    t->kind = TypeExpr::Kind::List;
    t->element = type_expr();
    consume(Tok::RBRACKET, "]");
    return t;
  }
  if (check(Tok::LBRACE)) {  // Map<K, V>
    advance();
    TypeExpr* t = new TypeExpr();
    t->kind = TypeExpr::Kind::Map;
    t->element = type_expr();
    consume(Tok::COLON, ":");
    t->value = type_expr();
    consume(Tok::RBRACE, "}");
    return t;
  }
  if (!check(Tok::IDENT)) { error_at_current("expected a type name"); return nullptr; }
  TypeExpr* t = new TypeExpr();
  t->kind = TypeExpr::Kind::Named;
  t->name = std::string(advance().text);
  if (check(Tok::DOT)) {  // e.g. http.Response -- treat as opaque
    advance();
    if (check(Tok::IDENT)) t->name += "." + std::string(advance().text);
  }
  return t;
}

// ---------------------------------------------------------------------------
// expressions -- precedence climbing
// ---------------------------------------------------------------------------
Expr* Parser::expression() { return assign(); }

Expr* Parser::assign() {
  Expr* e = ternary();
  static const Tok kAssign[] = {Tok::ASSIGN,        Tok::PLUS_EQUAL, Tok::MINUS_EQUAL,
                                 Tok::STAR_EQUAL,    Tok::SLASH_EQUAL, Tok::PERCENT_EQUAL};
  for (Tok k : kAssign) {
    if (check(k)) {
      const Token& op = advance();
      (void)0;
      Expr* rhs = assign();
      if (e->kind != ExprKind::Identifier && e->kind != ExprKind::Index &&
          e->kind != ExprKind::Member) {
        error(op, "invalid assignment target");
      }
      Expr* n = mk(ExprKind::Assign, op);
      n->op = k;
      n->a = e;
      n->b = rhs;
      return n;
    }
  }
  return e;
}

Expr* Parser::ternary() {
  Expr* c = or_expr();
  if (check(Tok::QUESTION)) {
    const Token& t = advance();
    Expr* a = expression();
    consume(Tok::COLON, ":");
    Expr* b = expression();
    Expr* n = mk(ExprKind::Ternary, t);
    n->a = c; n->b = a; n->c = b;
    return n;
  }
  return c;
}

Expr* Parser::or_expr() {
  Expr* e = and_expr();
  while (check(Tok::PIPE) || check(Tok::KW_OR)) {  // '|' / '||' and 'or'
    const Token& t = advance();
    Expr* n = mk(ExprKind::Logical, t);
    n->op = Tok::KW_OR;
    n->a = e;
    n->b = and_expr();
    e = n;
  }
  return e;
}

Expr* Parser::and_expr() {
  Expr* e = bit_or();
  while (check(Tok::AMP) || check(Tok::KW_AND)) {  // '&' / '&&' and 'and'
    const Token& t = advance();
    Expr* n = mk(ExprKind::Logical, t);
    n->op = Tok::KW_AND;
    n->a = e;
    n->b = bit_or();
    e = n;
  }
  return e;
}

Expr* Parser::bit_or() {
  Expr* e = bit_xor();
  while (check(Tok::PIPE)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = Tok::PIPE; n->a = e; n->b = bit_xor();
    e = n;
  }
  return e;
}

Expr* Parser::bit_xor() {
  Expr* e = bit_and();
  while (check(Tok::CARET)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = Tok::CARET; n->a = e; n->b = bit_and();
    e = n;
  }
  return e;
}

Expr* Parser::bit_and() {
  Expr* e = equality();
  while (check(Tok::AMP)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = Tok::AMP; n->a = e; n->b = equality();
    e = n;
  }
  return e;
}

Expr* Parser::equality() {
  Expr* e = comparison();
  while (check(Tok::EQUAL) || check(Tok::BANG_EQUAL)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = t.kind; n->a = e; n->b = comparison();
    e = n;
  }
  return e;
}

Expr* Parser::comparison() {
  Expr* e = shift();
  while (check(Tok::GREATER) || check(Tok::GREATER_EQUAL) || check(Tok::LESS) ||
         check(Tok::LESS_EQUAL) || check(Tok::KW_IN)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = t.kind; n->a = e; n->b = shift();
    e = n;
  }
  return e;
}

Expr* Parser::shift() {
  Expr* e = term();
  while (check(Tok::SHL) || check(Tok::SHR)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = t.kind; n->a = e; n->b = term();
    e = n;
  }
  return e;
}

Expr* Parser::power() {
  Expr* e = unary();
  if (check(Tok::DOUBLE_STAR)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = Tok::DOUBLE_STAR; n->a = e; n->b = power();  // right-associative
    e = n;
  }
  return e;
}

Expr* Parser::term() {
  // additive level:  a + b - c   (binds looser than * / %)
  Expr* e = multiplicative();
  while (check(Tok::PLUS) || check(Tok::MINUS)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = t.kind; n->a = e; n->b = multiplicative();
    e = n;
  }
  return e;
}

Expr* Parser::multiplicative() {
  // multiplicative level:  a * b / c % d   (binds tighter than + -)
  Expr* e = power();
  while (check(Tok::STAR) || check(Tok::SLASH) || check(Tok::PERCENT)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Binary, t);
    n->op = t.kind; n->a = e; n->b = power();
    e = n;
  }
  return e;
}

Expr* Parser::unary() {
  if (check(Tok::KW_NOT) || check(Tok::BANG) || check(Tok::MINUS) ||
      check(Tok::TILDE)) {
    const Token& t = advance();
    Expr* n = mk(ExprKind::Unary, t);
    n->op = (t.kind == Tok::BANG) ? Tok::KW_NOT : t.kind;
    n->a = unary();
    return n;
  }
  if (check(Tok::PLUS)) { advance(); return unary(); }  // unary plus
  return postfix();
}

Expr* Parser::postfix() {
  Expr* e = primary();
  for (;;) {
    if (check(Tok::LBRACKET)) {
      const Token& t = advance();
      Expr* lo = nullptr;
      if (!check(Tok::COLON)) lo = expression();
      if (check(Tok::COLON)) {  // slice  a[1:2]
        advance();
        Expr* hi = check(Tok::RBRACKET) ? nullptr : expression();
        consume(Tok::RBRACKET, "]");
        Expr* n = mk(ExprKind::Slice, t);
        n->a = e; n->b = lo; n->c = hi;
        e = n;
        continue;
      }
      consume(Tok::RBRACKET, "]");
      Expr* n = mk(ExprKind::Index, t);
      n->a = e; n->b = lo;
      e = n;
      continue;
    }
    if (check(Tok::DOT)) {
      const Token& t = advance();
      if (!check(Tok::IDENT) && !check(Tok::INT)) {
        error_at_current("expected a field name after '.'");
        break;
      }
      const Token& nameTok = advance();
      Expr* n = mk(ExprKind::Member, t);
      n->a = e;
      n->name = std::string(nameTok.text);
      e = n;
      continue;
    }
    if (check(Tok::LPAREN)) {
      const Token& t = advance();
      Expr* n = mk(ExprKind::Call, t);
      n->a = e;
      while (!check(Tok::RPAREN) && !check(Tok::EOF_)) {
        // named argument:  name = expr   or   name: expr
        if (check(Tok::IDENT) && (check_next(Tok::ASSIGN) || check_next(Tok::COLON))) {
          NamedArg na;
          na.name = std::string(advance().text);
          advance();  // '=' or ':'
          na.value = expression();
          n->named_args.push_back(std::move(na));
        } else {
          n->args.push_back(expression());
        }
        if (!match(Tok::COMMA)) break;
      }
      consume(Tok::RPAREN, ")");
      e = n;
      continue;
    }
    break;
  }
  return e;
}

bool Parser::is_list_comprehension() const {
  // scan ahead from '[' for a top-level `for` before the matching ']'
  int depth = 0;
  for (size_t i = cur_; i < toks_.size(); ++i) {
    Tok k = toks_[i].kind;
    if (k == Tok::LBRACKET || k == Tok::LBRACE || k == Tok::LPAREN) depth++;
    else if (k == Tok::RBRACKET || k == Tok::RBRACE || k == Tok::RPAREN) {
      if (depth == 0) return false;
      depth--;
      if (depth == 0) return false;  // reached the matching ']': stop
    } else if (k == Tok::KW_FOR && depth == 1) {
      return true;
    }
  }
  return false;
}

Expr* Parser::list_or_map_literal() {
  const Token& open = peek();  // '[' or '{'

  if (open.kind == Tok::LBRACKET && is_list_comprehension()) {
    advance();  // '['
    Expr* n = mk(ExprKind::ListComp, open);
    n->items.push_back(expression());
    while (match(Tok::KW_FOR)) {
      Generator g;
      if (!check(Tok::IDENT)) { error_at_current("expected loop variable"); break; }
      g.line = peek().line;
      g.var = std::string(advance().text);
      consume(Tok::KW_IN, "in");
      g.iterable = or_expr();
      if (match(Tok::KW_IF)) g.cond = or_expr();
      n->generators.push_back(std::move(g));
    }
    consume(Tok::RBRACKET, "]");
    return n;
  }

  if (open.kind == Tok::LBRACKET) {
    advance();
    Expr* n = mk(ExprKind::ListLit, open);
    while (!check(Tok::RBRACKET) && !check(Tok::EOF_)) {
      n->items.push_back(expression());
      if (!match(Tok::COMMA)) break;
    }
    consume(Tok::RBRACKET, "]");
    return n;
  }

  // map literal
  advance();  // '{'
  Expr* n = mk(ExprKind::MapLit, open);
  while (!check(Tok::RBRACE) && !check(Tok::EOF_)) {
    Expr* key = expression();
    consume(Tok::COLON, ":");
    Expr* val = expression();
    n->fields.emplace_back(key, val);
    if (!match(Tok::COMMA)) break;
  }
  consume(Tok::RBRACE, "}");
  return n;
}

Expr* Parser::interpolation(const Token& start) {
  // Interpolation scans the *decoded string*, not the token stream.
  //
  // A string literal arrives as a single STRING token, so walking tokens here
  // can only ever accumulate the whole literal and never find a hole -- which
  // is why a plain payload such as json.parse("{\"a\":1}") used to fail with
  // "expected ')'". Scanning the text also means a '{' only opens a hole when
  // it is followed by something that actually parses as an expression;
  // otherwise it stays literal text, so JSON braces survive intact.
  const std::string& src = start.str;
  Expr* n = mk(ExprKind::Interp, start);
  std::string lit;
  auto flush_lit = [&]() {
    if (lit.empty()) return;
    Expr* e = mk(ExprKind::StringLit, start);
    e->sval = lit;
    n->items.push_back(e);
    lit.clear();
  };

  size_t i = 0;
  while (i < src.size()) {
    if (src[i] != '{') { lit.push_back(src[i++]); continue; }

    // Find the matching '}' (no nesting: a hole is one expression).
    size_t j = i + 1;
    while (j < src.size() && src[j] != '}') j++;
    if (j >= src.size()) {           // unterminated brace: literal
      lit += src.substr(i);
      break;
    }
    std::string inner = src.substr(i + 1, j - i - 1);
    if (inner.empty()) {             // "{}" is literal text
      lit.append(src, i, j - i + 1);
      i = j + 1;
      continue;
    }

    // Try to parse the hole speculatively: lex + parse the fragment, and only
    // accept it if it parses cleanly and yields exactly one expression.
    Expr* e = parse_hole(inner, start);
    if (!e) {                        // not an expression: keep it as text
      lit.append(src, i, j - i + 1);
      i = j + 1;
      continue;
    }
    flush_lit();
    n->parts.push_back(e);
    i = j + 1;
  }

  if (n->parts.empty()) {
    // no holes: it was just a plain string
    Expr* s = mk(ExprKind::StringLit, start);
    s->sval = lit.empty() ? src : lit;
    return s;
  }
  flush_lit();
  return n;
}

// Parse `text` as a single expression in isolation. Returns nullptr when the
// text is not a complete expression (which is what keeps literal braces such as
// JSON payloads or "{ok}" from being mistaken for interpolation holes).
Expr* Parser::parse_hole(const std::string& text, const Token& origin) {
  Lexer lx(text, file_);
  std::vector<Token> toks = lx.scan();
  if (!lx.diagnostics().empty()) return nullptr;
  if (toks.size() < 2 || toks.back().kind != Tok::EOF_) return nullptr;

  // Parse the fragment in a throw-away parser. If it does not come back as
  // exactly one well-formed expression statement, the braces were literal text.
  Parser sub(toks, file_);
  Program p = sub.parse();
  if (!p.diags.empty()) return nullptr;
  if (p.statements.size() != 1) return nullptr;
  const Stmt* st = p.statements[0];
  if (!st || st->kind != StmtKind::ExprStmt || !st->expr) return nullptr;
  if (!sub.at_end()) return nullptr;   // e.g. "a b" is two names, not one expr

  Expr* e = st->expr;
  e->pos.line = origin.line;
  e->pos.col = origin.col;
  return e;
}


Expr* Parser::primary() {
  const Token& t = peek();
  switch (t.kind) {
    case Tok::INT: {
      advance();
      Expr* n = mk(ExprKind::IntLit, t);
      n->ival = t.ival;
      return n;
    }
    case Tok::FLOAT: {
      advance();
      Expr* n = mk(ExprKind::FloatLit, t);
      n->fval = t.fval;
      return n;
    }
    case Tok::STRING: {
      advance();
      if (t.str.find('{') == std::string::npos) {
        Expr* n = mk(ExprKind::StringLit, t);
        n->sval = t.str;
        return n;
      }
      return interpolation(t);
    }
    case Tok::KW_TRUE:
    case Tok::KW_FALSE: {
      advance();
      Expr* n = mk(ExprKind::BoolLit, t);
      n->bval = (t.kind == Tok::KW_TRUE);
      return n;
    }
    case Tok::KW_NULL: {
      advance();
      return mk(ExprKind::NullLit, t);
    }
    case Tok::IDENT: {
      advance();
      Expr* n = mk(ExprKind::Identifier, t);
      n->name = std::string(t.text);
      return n;
    }
    case Tok::LBRACKET:
    case Tok::LBRACE:
      return list_or_map_literal();
    case Tok::LPAREN: {
      advance();
      Expr* inner = expression();
      consume(Tok::RPAREN, ")");
      return inner;
    }
    case Tok::KW_FUNCTION: {
      advance();
      Expr* n = mk(ExprKind::Closure, t);
      bool variadic = false;
      n->params = param_list(&variadic);
      if (match(Tok::ARROW)) n->type = type_expr();  // reuse slot for return type
      consume(Tok::LBRACE, "{");
      fn_depth_++;
      n->body = statement_list();
      fn_depth_--;
      consume(Tok::RBRACE, "}");
      return n;
    }
    default:
      break;
  }
  error_at_current("unexpected token in expression");
  advance();
  return error_expr(t);
}

}  // namespace ov

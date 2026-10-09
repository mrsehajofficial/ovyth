#include "ast.h"

#include <sstream>

namespace ov {
namespace ast {

static const char* type_name(ExprKind k) {
  switch (k) {
    case ExprKind::IntLit: return "Int";
    case ExprKind::FloatLit: return "Float";
    case ExprKind::StringLit: return "String";
    case ExprKind::BoolLit: return "Bool";
    case ExprKind::NullLit: return "Null";
    case ExprKind::Identifier: return "Ident";
    case ExprKind::ListLit: return "List";
    case ExprKind::MapLit: return "Map";
    case ExprKind::Interp: return "Interp";
    case ExprKind::Unary: return "Unary";
    case ExprKind::Binary: return "Binary";
    case ExprKind::Logical: return "Logical";
    case ExprKind::Assign: return "Assign";
    case ExprKind::Ternary: return "Ternary";
    case ExprKind::Call: return "Call";
    case ExprKind::Index: return "Index";
    case ExprKind::Member: return "Member";
    case ExprKind::Slice: return "Slice";
    case ExprKind::ListComp: return "ListComp";
    case ExprKind::Closure: return "Closure";
    case ExprKind::Cast: return "Cast";
    case ExprKind::Error: return "Error";
  }
  return "?";
}

std::string Expr::str() const {
  std::ostringstream o;
  o << type_name(kind);
  switch (kind) {
    case ExprKind::IntLit: o << "(" << ival << ")"; break;
    case ExprKind::FloatLit: o << "(" << fval << ")"; break;
    case ExprKind::BoolLit: o << "(" << (bval ? "true" : "false") << ")"; break;
    case ExprKind::NullLit: break;
    case ExprKind::StringLit: o << "(\"" << sval << "\")"; break;
    case ExprKind::Identifier: o << "(" << name << ")"; break;
    case ExprKind::Member: o << " . " << name; break;
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Logical: o << " " << tok_name(op) << " "; break;
    case ExprKind::Cast: break;
    default: break;
  }
  return o.str();
}

std::string Stmt::str() const {
  switch (kind) {
    case StmtKind::ExprStmt: return "ExprStmt";
    case StmtKind::VarDecl: return "VarDecl";
    case StmtKind::Assign: return "Assign";
    case StmtKind::FuncDecl: return "FuncDecl";
    case StmtKind::Return: return "Return";
    case StmtKind::If: return "If";
    case StmtKind::While: return "While";
    case StmtKind::For: return "For";
    case StmtKind::Block: return "Block";
    case StmtKind::Break: return "Break";
    case StmtKind::Continue: return "Continue";
    case StmtKind::Try: return "Try";
    case StmtKind::Throw: return "Throw";
    case StmtKind::Debug: return "Debug";
    case StmtKind::Import: return "Import";
  }
  return "?";
}

// ---------------------------------------------------------------------------
void dump_ast(const Program& p, std::string& out) {
  std::ostringstream o;
  struct D {
    std::ostringstream& o;
    void expr(Expr* e, int ind) {
      if (!e) return;
      std::string pad(ind * 2, ' ');
      o << pad << e->str() << "\n";
      switch (e->kind) {
        case ExprKind::Unary:
          expr(e->a, ind + 1); break;
        case ExprKind::Binary:
        case ExprKind::Logical:
        case ExprKind::Assign:
          expr(e->a, ind + 1);
          expr(e->b, ind + 1);
          break;
        case ExprKind::Ternary:
          expr(e->a, ind + 1); expr(e->b, ind + 1); expr(e->c, ind + 1);
          break;
        case ExprKind::Index:
          expr(e->a, ind + 1); expr(e->b, ind + 1); break;
        case ExprKind::Member:
          expr(e->a, ind + 1); break;
        case ExprKind::Slice:
          expr(e->a, ind + 1); expr(e->b, ind + 1); expr(e->c, ind + 1); break;
        case ExprKind::Cast:
          expr(e->a, ind + 1); break;
        case ExprKind::Call:
          expr(e->a, ind + 1);
          for (auto* x : e->args) expr(x, ind + 1);
          for (auto& na : e->named_args) {
            o << std::string(ind * 2, ' ') << "arg " << na.name << "=\n";
            expr(na.value, ind + 1);
          }
          break;
        case ExprKind::ListLit:
        case ExprKind::Interp:
          for (auto* x : e->items) expr(x, ind + 1);
          for (auto* x : e->parts) expr(x, ind + 1);
          break;
        case ExprKind::ListComp:
          for (auto* x : e->items) expr(x, ind + 1);
          for (auto& g : e->generators) {
            o << std::string(ind * 2, ' ') << "for " << g.var << " in\n";
            expr(g.iterable, ind + 1);
            if (g.cond) { o << std::string(ind*2,' ') << "if\n"; expr(g.cond, ind + 1); }
          }
          break;
        case ExprKind::Closure:
          o << std::string(ind * 2, ' ') << "params:";
          for (auto& p : e->params) o << " " << p.name;
          o << "\n";
          for (auto* s : e->body) stmt(s, ind + 1);
          break;
        case ExprKind::MapLit:
          for (auto& f : e->fields) {
            o << std::string(ind * 2, ' ') << "key:\n";
            expr(f.first, ind + 1);
            o << std::string(ind * 2, ' ') << "value:\n";
            expr(f.second, ind + 1);
          }
          break;
        default: break;
      }
    }
    void stmt(Stmt* s, int ind) {
      std::string pad(ind * 2, ' ');
      if (!s) return;
      o << pad << s->str();
      if (s->kind == StmtKind::VarDecl || s->kind == StmtKind::FuncDecl) {
        o << "(";
        for (size_t i = 0; i < s->names.size(); ++i) {
          o << s->names[i];
          if (i + 1 < s->names.size()) o << ", ";
        }
        o << ")";
      }
      o << "\n";
      if (s->kind == StmtKind::FuncDecl && s->expr) {
        o << pad << " body:\n";
        for (auto* x : s->body) stmt(x, ind + 1);
        return;
      }
      expr(s->expr, ind + 1);
      expr(s->expr2, ind + 1);
      expr(s->expr3, ind + 1);
      for (auto* x : s->body) stmt(x, ind + 1);
      for (auto* x : s->else_body) stmt(x, ind + 1);
    }
  } d{o};
  for (auto* s : p.statements) d.stmt(s, 0);
  out = o.str();
}

}  // namespace ast
}  // namespace ov
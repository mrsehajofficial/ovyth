#include "fmt.h"

#include <cmath>
#include <sstream>

namespace ov {
using namespace ast;

namespace {

struct Fmt {
  std::ostringstream o;
  int indent = 0;

  void nl() { o << "\n"; }
  void pad() { for (int i = 0; i < indent; i++) o << "    "; }
  void blank() {
    // collapse runs of blank lines to one
    std::string s = o.str();
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, "\n\n") == 0) return;
    o << "\n";
  }

  std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
      switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:   out.push_back(c);
      }
    }
    out += "\"";
    return out;
  }

  std::string num(double d) {
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
      std::ostringstream t;
      t << (long long)d << ".0";
      return t.str();
    }
    std::ostringstream t;
    t.precision(15);
    t << d;
    return t.str();
  }

  // An interpolated string: re-encode the literal runs, keep {expr} holes.
  std::string interp(const Expr* e) {
    std::string out = "\"";
    size_t li = 0;
    for (size_t i = 0; i < e->parts.size(); i++) {
      if (li < e->items.size()) {
        std::string t = e->items[li]->sval;
        for (char c : t) {
          switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out.push_back(c);
          }
        }
        li++;
      }
      out += "{";
      out += expr(e->parts[i]);
      out += "}";
    }
    for (; li < e->items.size(); li++) {
      for (char c : e->items[li]->sval) {
        if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
      }
    }
    out += "\"";
    return out;
  }

  std::string expr(const Expr* e) {
    if (!e) return "nil";
    switch (e->kind) {
      case ExprKind::IntLit:   return std::to_string(e->ival);
      case ExprKind::FloatLit: return num(e->fval);
      case ExprKind::BoolLit:  return e->bval ? "true" : "false";
      case ExprKind::NullLit:  return "nil";
      case ExprKind::StringLit: return quote(e->sval);
      case ExprKind::Interp:   return interp(e);
      case ExprKind::Identifier: return e->name;

      case ExprKind::ListLit: {
        std::string out = "[";
        for (size_t i = 0; i < e->items.size(); i++) {
          if (i) out += ", ";
          out += expr(e->items[i]);
        }
        return out + "]";
      }

      case ExprKind::MapLit: {
        std::string out = "{";
        for (size_t i = 0; i < e->fields.size(); i++) {
          if (i) out += ", ";
          out += expr(e->fields[i].first);
          out += ": ";
          out += expr(e->fields[i].second);
        }
        return out + "}";
      }

      case ExprKind::ListComp: {
        std::string out = "[" + expr(e->items[0]);
        for (const auto& g : e->generators) {
          out += " for " + g.var + " in " + expr(g.iterable);
          if (g.cond) out += " if " + expr(g.cond);
        }
        return out + "]";
      }

      case ExprKind::Unary: {
        std::string op = e->op == Tok::KW_NOT ? "not " : tok_name(e->op);
        return op + expr(e->a);
      }

      case ExprKind::Binary:
        return expr(e->a) + " " + tok_name(e->op) + " " + expr(e->b);

      case ExprKind::Logical:
        return expr(e->a) + (e->op == Tok::KW_AND ? " and " : " or ") + expr(e->b);

      case ExprKind::Assign: {
        if (e->op == Tok::ASSIGN) return expr(e->a) + " = " + expr(e->b);
        return expr(e->a) + " " + tok_name(e->op) + " " + expr(e->b);
      }

      case ExprKind::Ternary:
        return expr(e->a) + " if not nil else " + expr(e->c) + " when " + expr(e->b);

      case ExprKind::Index:
        return expr(e->a) + "[" + expr(e->b) + "]";

      case ExprKind::Member:
        return expr(e->a) + "." + e->name;

      case ExprKind::Slice: {
        std::string out = expr(e->a) + "[";
        if (e->b) out += expr(e->b);
        out += ":";
        if (e->c) out += expr(e->c);
        return out + "]";
      }

      case ExprKind::Call: {
        std::string out = expr(e->a) + "(";
        bool first = true;
        for (auto* a : e->args) {
          if (!first) out += ", ";
          out += expr(a);
          first = false;
        }
        for (const auto& na : e->named_args) {
          if (!first) out += ", ";
          out += na.name + " = " + expr(na.value);
          first = false;
        }
        return out + ")";
      }

      case ExprKind::Closure: {
        std::string out = "function(";
        for (size_t i = 0; i < e->params.size(); i++) {
          if (i) out += ", ";
          out += e->params[i].name;
        }
        out += ") {\n";
        for (auto* s : e->body) out += stmt(s, 1);
        return out + indent_str() + "}";
      }

      case ExprKind::Cast:
        return expr(e->a) + " as " + (e->type ? e->type->name : "Any");

      case ExprKind::Error:
        return "<error>";
    }
    return "<?>";
  }

  std::string indent_str() { std::string s; for (int i = 0; i < indent; i++) s += "    "; return s; }

  std::string stmt(const Stmt* s, int ind) {
    std::string pad;
    for (int i = 0; i < ind; i++) pad += "    ";
    if (!s) return "";
    switch (s->kind) {
      case StmtKind::VarDecl: {
        std::string out;
        for (size_t i = 0; i < s->names.size(); i++) {
          if (i) out += ", ";
          out += s->names[i];
          if (i < s->types.size() && s->types[i])
            out += ": " + (s->types[i]->kind == TypeExpr::Kind::Named
                               ? s->types[i]->name
                               : "[...]");
        }
        out += " = ";
        for (size_t i = 0; i < s->values.size(); i++) {
          if (i) out += ", ";
          out += expr(s->values[i]);
        }
        return pad + out + "\n";
      }
      case StmtKind::ExprStmt: return pad + expr(s->expr) + "\n";
      case StmtKind::Assign: {
        std::string op = s->op == Tok::ASSIGN ? " = " : std::string(" ") + tok_name(s->op) + " ";
        return pad + expr(s->expr) + op + expr(s->expr2) + "\n";
      }
      case StmtKind::FuncDecl: {
        std::string out = pad + "function " + s->name + "(";
        for (size_t i = 0; i < s->params.size(); i++) {
          if (i) out += ", ";
          out += s->params[i].name;
        }
        out += ") {\n";
        for (auto* b : s->body) out += stmt(b, ind + 1);
        out += pad + "}\n";
        return out;
      }
      case StmtKind::Return:
        return pad + "return" + (s->expr ? " " + expr(s->expr) : "") + "\n";
      case StmtKind::If: {
        std::string out = pad + "if " + expr(s->expr) + " {\n";
        for (auto* b : s->body) out += stmt(b, ind + 1);
        out += pad + "}";
        if (!s->else_body.empty()) {
          out += " else {\n";
          for (auto* b : s->else_body) out += stmt(b, ind + 1);
          out += pad + "}";
        }
        return out + "\n";
      }
      case StmtKind::While: {
        std::string out = pad + "while " + expr(s->expr) + " {\n";
        for (auto* b : s->body) out += stmt(b, ind + 1);
        return out + pad + "}\n";
      }
      case StmtKind::For: {
        std::string names;
        for (size_t i = 0; i < s->iter_vars.size(); i++) {
          if (i) names += ", ";
          names += s->iter_vars[i];
        }
        std::string out = pad + "for " + names + " in " + expr(s->expr) + " {\n";
        for (auto* b : s->body) out += stmt(b, ind + 1);
        return out + pad + "}\n";
      }
      case StmtKind::Block: {
        std::string out = pad + "{\n";
        for (auto* b : s->body) out += stmt(b, ind + 1);
        return out + pad + "}\n";
      }
      case StmtKind::Break:    return pad + "break\n";
      case StmtKind::Continue: return pad + "continue\n";
      case StmtKind::Try: {
        std::string out = pad + "try {\n";
        for (auto* b : s->body) out += stmt(b, ind + 1);
        out += pad + "} catch " + s->catch_var + " {\n";
        for (auto* b : s->else_body) out += stmt(b, ind + 1);
        return out + pad + "}\n";
      }
      case StmtKind::Throw: return pad + "throw" + (s->expr ? " " + expr(s->expr) : "") + "\n";
      case StmtKind::Debug: return pad + "debug\n";
    }
    return "";
  }
};

}  // namespace

std::string format_program(const Program& program, const std::string& source) {
  (void)source;
  Fmt f;
  for (auto* s : program.statements) f.o << f.stmt(s, 0);
  return f.o.str();
}

}  // namespace ov
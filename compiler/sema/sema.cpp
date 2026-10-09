#include "sema.h"

#include <algorithm>

namespace ov {

using namespace ast;

const char* ty_name(Ty t) {
  switch (t) {
    case Ty::Unknown: return "unknown";
    case Ty::Nil:     return "nil";
    case Ty::Bool:    return "Bool";
    case Ty::Int:     return "Int";
    case Ty::Float:   return "Float";
    case Ty::Str:     return "String";
    case Ty::List:    return "List";
    case Ty::Map:     return "Map";
    case Ty::Func:    return "Function";
    case Ty::Any:     return "any";
    case Ty::Never:   return "never";
    case Ty::Error:   return "<error>";
  }
  return "?";
}

void Sema::error(const Pos& pos, const std::string& msg) {
  diags_.push_back(Diagnostic{pos.file, pos.line, pos.col, msg, DiagKind::Error});
  errors_++;
}

void Sema::note(const Pos& pos, const std::string& msg) {
  diags_.push_back(Diagnostic{pos.file, pos.line, pos.col, msg, DiagKind::Note});
}

void Sema::push_scope(bool is_function) {
  scopes_.push_back(Scope{});
  scopes_.back().is_function = is_function;
}

void Sema::pop_scope() { scopes_.pop_back(); }

bool Sema::lookup(const std::string& name, Ty* out) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->vars.find(name);
    if (f != it->vars.end()) {
      *out = f->second;
      return true;
    }
  }
  return false;
}

void Sema::define(const std::string& name, Ty t) {
  scopes_.back().vars[name] = t;
}

void Sema::define(const std::string& name, Ty t, bool proven_int, bool proven_float) {
  define(name, t);
  if (proven_int || proven_float) {
    TyInfo info;
    info.ty = t;
    info.is_proven_int = proven_int;
    info.is_proven_float = proven_float;
    proven_types[name] = info;
  }
}

Ty Sema::type_from(const TypeExpr* t) {
  if (!t) return Ty::Unknown;
  switch (t->kind) {
    case TypeExpr::Kind::Named: {
      const std::string& n = t->name;
      if (n == "String" || n == "Str") return Ty::Str;
      if (n == "Int")   return Ty::Int;
      if (n == "Float" || n == "Double" || n == "Number") return Ty::Float;
      if (n == "Bool")  return Ty::Bool;
      if (n == "List")  return Ty::List;
      if (n == "Map" || n == "Dict" || n == "Object") return Ty::Map;
      if (n == "Nil" || n == "None") return Ty::Nil;
      if (n == "Any")   return Ty::Any;
      if (n == "Function" || n == "Fn" || n == "Callable") return Ty::Func;
      return Ty::Unknown;  // unresolved names are treated as opaque types
    }
    case TypeExpr::Kind::List:  return Ty::List;
    case TypeExpr::Kind::Map:   return Ty::Map;
  }
  return Ty::Unknown;
}

bool Sema::is_numeric(Ty t) {
  return t == Ty::Int || t == Ty::Float || t == Ty::Bool || t == Ty::Unknown ||
         t == Ty::Any || t == Ty::Error;
}

Ty Sema::unify(Ty a, Ty b) {
  if (a == b) return a;
  if (a == Ty::Unknown || a == Ty::Any || a == Ty::Error) return b;
  if (b == Ty::Unknown || b == Ty::Any || b == Ty::Error) return a;
  // Int widens to Float, mirroring what the runtime does.
  if ((a == Ty::Int && b == Ty::Float) || (a == Ty::Float && b == Ty::Int))
    return Ty::Float;
  return Ty::Unknown;
}

bool Sema::run(Program& program) {
  // Builtins are visible as ordinary names so that `print(...)` reads like
  // a function call rather than a special form.
  static const char* kBuiltins[] = {
      "print", "input", "env", "len", "type", "str", "int", "float", "bool",
      "abs", "min", "max", "sum", "round", "floor", "ceil", "sqrt", "pow",
      "range", "push", "pop", "keys", "values", "get", "set", "has", "delete",
      "join", "split", "upper", "lower", "trim", "startswith", "endswith",
      "contains", "replace", "indexof", "sort", "reverse", "append", "extend",
      "json", "http", "time", "args", "exit", "throw", "assert", "parseInt", "number",
      "eprint", "env_or", "getenv", "getenv_or", "setenv", "gc", "pad", "clock", "now",
  };
  push_scope(true);
  for (const char* b : kBuiltins) define(b, Ty::Func);

  for (auto* s : program.statements) check_stmt(s);

  for (const auto& d : diags_) program.diags.push_back(d);
  return errors_ == 0;
}

// ---------------------------------------------------------------------------
Ty Sema::check_block(const StmtList& body) {
  Ty last = Ty::Nil;
  for (auto* s : body) last = check_stmt(s);
  return last;
}

Ty Sema::check_stmt(Stmt* s) {
  if (!s) return Ty::Unknown;
  switch (s->kind) {
    case StmtKind::VarDecl: {
      std::vector<Ty> vt;
      bool unpack = s->names.size() > 1 && s->values.size() == 1;
      Ty elem = Ty::Unknown;
      if (unpack) {
        elem = check_expr(s->values[0]);
        // `a, b = [1, 2, 3]`: each name gets the container's element type,
        // which a plain list value does not record, so fall back to Any.
        if (elem != Ty::Any) elem = Ty::Any;
      }
      for (size_t i = 0; i < s->names.size(); i++) {
        Ty declared = i < s->types.size() && s->types[i] ? type_from(s->types[i]) : Ty::Unknown;
        Ty inferred = (!unpack && i < s->values.size() && s->values[i])
                          ? check_expr(s->values[i]) : (unpack ? elem : Ty::Unknown);
        Ty t = declared == Ty::Unknown ? inferred : declared;
        if (declared != Ty::Unknown && inferred != Ty::Unknown && inferred != Ty::Any &&
            !is_numeric(declared) && declared != inferred &&
            declared != Ty::Any) {
          error(s->pos, "'" + s->names[i] + "' is declared " + ty_name(declared) +
                            " but the value is " + ty_name(inferred));
        }
        if (t == Ty::Error) t = Ty::Unknown;

        // Track proven types for all pure-int expressions - enables raw C arithmetic
        bool proven_int = false, proven_float = false;
        if (!unpack && i < s->values.size() && s->values[i]) {
          proven_int  = is_proven_int_expr(s->values[i]);
          proven_float = !proven_int && s->values[i]->kind == ExprKind::FloatLit;
        }
        define(s->names[i], t, proven_int, proven_float);
        vt.push_back(t);
        if (scopes_.size() == 1) globals[s->names[i]] = t;
      }
      return Ty::Nil;
    }


    case StmtKind::ExprStmt:
      check_expr(s->expr);
      return Ty::Nil;

    case StmtKind::Assign: {
      Ty target = check_expr(s->expr);
      Ty value = check_expr(s->expr2);
      if (s->op != Tok::ASSIGN) {
        if (!is_numeric(target) && target != Ty::Str && target != Ty::List)
          error(s->pos, std::string("cannot apply '") + tok_name(s->op) +
                            "' to " + ty_name(target));
        if (target == Ty::List && s->op != Tok::PLUS_EQUAL)
          error(s->pos, "only '+=' is supported on lists");
      } else if (s->expr && s->expr->kind == ExprKind::Identifier) {
        Ty declared;
        if (lookup(s->expr->name, &declared) && declared != Ty::Unknown &&
            value != Ty::Unknown && declared != Ty::Any && value != Ty::Any &&
            !is_numeric(declared) && declared != value &&
            !(declared == Ty::Float && value == Ty::Int)) {
          note(s->pos, std::string("'") + s->expr->name + "' was inferred as " +
                          ty_name(declared) + ", now assigned " + ty_name(value));
        }
      }
      return Ty::Nil;
    }

    case StmtKind::FuncDecl: {
      FuncSig sig;
      sig.name = s->name;
      sig.ret = type_from(s->return_type);
      sig.variadic = s->is_variadic;
      sig.decl_line = s->pos.line;
      for (auto& p : s->params) sig.params.push_back(p.name);
      funcs_[s->name] = sig;

      push_scope(true);
      Ty saved_ret = current_return_;
      current_return_ = sig.ret;
      for (size_t i = 0; i < s->params.size(); i++) {
        Ty t = s->params[i].type ? type_from(s->params[i].type) : Ty::Unknown;
        define(s->params[i].name, t);
      }
      check_block(s->body);
      if (sig.ret != Ty::Unknown && sig.ret != Ty::Any) {
        // look for a return of the annotated type on the top level
        bool found = false;
        for (auto* st : s->body)
          if (st && st->kind == StmtKind::Return && st->expr) found = true;
        if (!found)
          note(s->pos, "function '" + s->name + "' declares -> " +
                          ty_name(sig.ret) + " but has no return statement");
      }
      current_return_ = saved_ret;
      pop_scope();
      return Ty::Nil;
    }

    case StmtKind::Return: {
      Ty t = s->expr ? check_expr(s->expr) : Ty::Nil;
      if (current_return_ != Ty::Unknown && current_return_ != Ty::Any &&
          s->expr && !is_numeric(current_return_) && t != Ty::Unknown &&
          current_return_ != t) {
        error(s->pos, std::string("returning ") + ty_name(t) + " from a function declared -> " +
                          ty_name(current_return_));
      }
      return Ty::Never;
    }

    case StmtKind::If:
      check_expr(s->expr);
      push_scope();
      check_block(s->body);
      pop_scope();
      push_scope();
      check_block(s->else_body);
      pop_scope();
      return Ty::Nil;

    case StmtKind::While:
      check_expr(s->expr);
      loop_stack_.push_back(Ty::Nil);
      /* fallthrough-free: the body is checked in its own scope below */
      push_scope();
      check_block(s->body);
      pop_scope();
      loop_stack_.pop_back();
      return Ty::Nil;

    case StmtKind::For: {
      const Ty it = check_expr(s->expr);
      if (it != Ty::List && it != Ty::Str && it != Ty::Map &&
          it != Ty::Unknown && it != Ty::Any && it != Ty::Error)
        error(s->pos, std::string("cannot loop over ") + ty_name(it) +
                          " (expected List, String or Map)");
      push_scope();
      for (auto& v : s->iter_vars) define(v, Ty::Unknown);
      loop_stack_.push_back(Ty::Nil);
      check_block(s->body);
      loop_stack_.pop_back();
      pop_scope();
      return Ty::Nil;
    }

    case StmtKind::Block: {
      push_scope();
      const Ty t = check_block(s->body);
      pop_scope();
      return t;
    }

    case StmtKind::Break:
      if (loop_stack_.empty()) error(s->pos, "'break' outside a loop");
      return Ty::Never;

    case StmtKind::Continue:
      if (loop_stack_.empty()) error(s->pos, "'continue' outside a loop");
      return Ty::Never;

    case StmtKind::Try: {
      push_scope();
      check_block(s->body);
      pop_scope();
      push_scope();
      if (!s->catch_var.empty()) define(s->catch_var, Ty::Str);
      check_block(s->else_body);
      pop_scope();
      return Ty::Nil;
    }

    case StmtKind::Throw: {
      if (s->expr) check_expr(s->expr);
      return Ty::Never;
    }

    case StmtKind::Debug:
      return Ty::Nil;
    case StmtKind::Import:
      return Ty::Nil;  // consumed by the loader before sema
  }
  return Ty::Unknown;
}

// ---------------------------------------------------------------------------
Ty Sema::check_unary(Expr* e) {
  Ty t = check_expr(e->a);
  switch (e->op) {
    case Tok::KW_NOT:
      if (t != Ty::Bool && t != Ty::Unknown && t != Ty::Any)
        error(e->pos, std::string("'not' expects a Bool, got ") + ty_name(t));
      return Ty::Bool;
    case Tok::MINUS:
    case Tok::TILDE:
      if (!is_numeric(t)) error(e->pos, "unary '-' expects a number");
      return (t == Ty::Unknown || t == Ty::Any) ? Ty::Int : t;
    default:
      return Ty::Unknown;
  }
}

Ty Sema::check_binary(Expr* e) {
  Ty l = check_expr(e->a);
  Ty r = check_expr(e->b);

  switch (e->op) {
    case Tok::PLUS:
      if (l == Ty::List || r == Ty::List) {
        if (l != Ty::List || r != Ty::List)
          error(e->pos, std::string("cannot add a list and a ") + ty_name(l == Ty::List ? r : l));
        return Ty::List;
      }
      if (l == Ty::Map || r == Ty::Map)
        error(e->pos, "maps use '.get' / '.set' / 'merge', not '+'");
      if (l == Ty::Str || r == Ty::Str) {
        if (l == Ty::Str && r != Ty::Str && r != Ty::Int && r != Ty::Float &&
            r != Ty::Bool && r != Ty::Nil && r != Ty::Unknown && r != Ty::Any)
          error(e->pos, std::string("cannot concatenate String and ") + ty_name(r));
        return Ty::Str;
      }
      if (!is_numeric(l) || !is_numeric(r))
        error(e->pos, std::string("cannot add ") + ty_name(l) + " and " + ty_name(r));
      return unify(l, r) == Ty::Float ? Ty::Float : Ty::Int;

    case Tok::MINUS:
    case Tok::STAR:
    case Tok::SLASH:
      if (e->op == Tok::STAR && l == Ty::Str && r == Ty::Int) return Ty::Str;
      if (!is_numeric(l) || !is_numeric(r))
        error(e->pos, std::string("arithmetic needs numbers, got ") +
                          ty_name(l) + " and " + ty_name(r));
      if (e->op == Tok::SLASH && l == Ty::Int && r == Ty::Int)
        note(e->pos, "integer division; use / with a float for a real quotient");
      return unify(l, r) == Ty::Float ? Ty::Float : Ty::Int;

    case Tok::PERCENT:
      if (!is_numeric(l) || !is_numeric(r))
        error(e->pos, "'%' needs two integers");
      return Ty::Int;

    case Tok::EQUAL:
    case Tok::BANG_EQUAL:
      return Ty::Bool;

    case Tok::GREATER_EQUAL:
    case Tok::LESS_EQUAL:
      if (l != Ty::Unknown && r != Ty::Unknown && l != Ty::Any && r != Ty::Any) {
        bool ok = (is_numeric(l) && is_numeric(r)) || l == r ||
                  (l == Ty::Str && r == Ty::Str) || l == Ty::List || r == Ty::List;
        if (!ok)
          error(e->pos, std::string("cannot compare ") + ty_name(l) + " with " +
                            ty_name(r));
      }
      return Ty::Bool;

    case Tok::KW_IN:
      return Ty::Bool;

    case Tok::SHL:
    case Tok::SHR:
    case Tok::AMP:
    case Tok::PIPE:
    case Tok::CARET:
      return Ty::Int;

    default:
      return Ty::Unknown;
  }
}

Ty Sema::check_call(Expr* e) {
  Expr* callee = e->a;

  // method-style builtin namespaces: print(...), http.get(...), json.parse(...)
  std::string qualified;
  if (callee->kind == ExprKind::Identifier) {
    qualified = callee->name;
  } else if (callee->kind == ExprKind::Member && callee->a &&
             callee->a->kind == ExprKind::Identifier) {
    qualified = callee->a->name + "." + callee->name;
  }

  for (auto* a : e->args) check_expr(a);
  for (auto& na : e->named_args) check_expr(na.value);

  if (qualified == "http.get" || qualified == "http.post" ||
      qualified == "http.put" || qualified == "http.patch" ||
      qualified == "http.delete" || qualified == "http.head" ||
      qualified == "http.request") {
    static const std::vector<std::string> known = {
        "url", "headers", "json", "body", "params", "timeout", "content_type"};
    for (auto& na : e->named_args) {
      if (std::find(known.begin(), known.end(), na.name) == known.end())
        error(na.value->pos, "http.* does not take an argument named '" +
                                 na.name + "' (expected one of url, headers, json, "
                                 "body, params, timeout, content_type)");
    }
    return Ty::Map;  // the response is a map; fields read off it dynamically
  }

  if (qualified == "json.parse" || qualified == "json.stringify" ||
      qualified == "json.valid" || qualified == "json.extract") {
    return Ty::Any;
  }

  if (qualified.rfind("str.", 0) == 0) return Ty::Str;
  if (qualified == "str" || qualified == "print" || qualified == "eprint" ||
      qualified == "setenv") return Ty::Nil;
  if (qualified == "input") return Ty::Str;
  if (qualified == "env" || qualified == "getenv") return Ty::Str;
  /* env_or falls back to its default argument, which may be any type; gc("stats")
   * returns a String while gc() returns nil. */
  if (qualified == "env_or" || qualified == "getenv_or" || qualified == "gc")
    return Ty::Any;
  if (qualified == "pad") return Ty::Str;
  if (qualified == "time.clock" || qualified == "clock") return Ty::Float;
  if (qualified == "time.now" || qualified == "now") return Ty::Int;
  if (qualified == "len") return Ty::Int;
  if (qualified == "type") return Ty::Str;
  if (qualified == "int" || qualified == "parseInt") return Ty::Int;
  if (qualified == "float" || qualified == "number") return Ty::Float;
  if (qualified == "bool") return Ty::Bool;
  if (qualified == "range") return Ty::List;
  if (qualified == "keys" || qualified == "values" || qualified == "split" ||
      qualified == "push" || qualified == "append" || qualified == "extend" ||
      qualified == "sort" || qualified == "reverse" || qualified == "get" ||
      qualified == "args" || qualified == "list")
    return Ty::List;
  if (qualified == "set")   return Ty::Map;
  if (qualified == "has" || qualified == "delete") return Ty::Bool;
  if (qualified == "exit") return Ty::Never;
  if (qualified == "throw") return Ty::Never;
  if (qualified == "assert") return Ty::Nil;
  if (qualified == "min" || qualified == "max" || qualified == "sum" ||
      qualified == "abs" || qualified == "round" || qualified == "floor" ||
      qualified == "ceil" || qualified == "sqrt" || qualified == "pow")
    return Ty::Any;

  if (callee->kind == ExprKind::Identifier) {
    Ty local;
    if (lookup(callee->name, &local)) {
      if (local != Ty::Func && local != Ty::Unknown && local != Ty::Any)
        error(callee->pos, "'" + callee->name + "' is a " + ty_name(local) +
                               ", not something you can call");
      return Ty::Any;
    }
    auto f = funcs_.find(callee->name);
    if (f != funcs_.end()) {
      size_t want = f->second.params.size();
      size_t got = e->args.size() + e->named_args.size();
      if (!f->second.variadic && got != want) {
        error(e->pos, "'" + callee->name + "' takes " + std::to_string(want) +
                          " argument(s) but got " + std::to_string(got));
      }
      return f->second.ret == Ty::Unknown ? Ty::Any : f->second.ret;
    }
    error(e->pos, "unknown function '" + callee->name + "'");
    return Ty::Error;
  }

  Ty base = check_expr(callee);
  return base == Ty::Func ? Ty::Any : base;
}

// Check if an expression is a pure integer literal or combination thereof
// (no side effects, no float promotion) - enables raw int64_t lowering
bool Sema::is_proven_int_expr(Expr* e) {
  if (!e) return false;
  switch (e->kind) {
    case ExprKind::IntLit:
      return true;
    case ExprKind::Unary:
      if (e->op == Tok::MINUS) return is_proven_int_expr(e->a);
      return false;
    case ExprKind::Binary: {
      // Only ops that preserve int->int
      switch (e->op) {
        case Tok::PLUS: case Tok::MINUS: case Tok::STAR: case Tok::PERCENT:
        case Tok::AMP: case Tok::PIPE: case Tok::CARET:
        case Tok::SHL: case Tok::SHR:
          return is_proven_int_expr(e->a) && is_proven_int_expr(e->b);
        default:
          return false;
      }
    }
    default:
      return false;
  }
}

Ty Sema::check_expr(Expr* e) {
  if (!e) return Ty::Unknown;
  switch (e->kind) {
    case ExprKind::IntLit:   return Ty::Int;
    case ExprKind::FloatLit: return Ty::Float;
    case ExprKind::StringLit: return Ty::Str;
    case ExprKind::BoolLit:  return Ty::Bool;
    case ExprKind::NullLit:  return Ty::Nil;

    case ExprKind::Identifier: {
      Ty t;
      if (lookup(e->name, &t)) return t;
      if (funcs_.count(e->name)) return Ty::Func;
      error(e->pos, "unknown name '" + e->name + "'");
      return Ty::Error;
    }

    case ExprKind::ListLit: {
      for (auto* x : e->items) check_expr(x);
      return Ty::List;
    }

    case ExprKind::ListComp: {
      for (size_t i = 0; i < e->generators.size(); i++) {
        const auto& g = e->generators[i];
        Ty it = check_expr(g.iterable);
        if (it != Ty::List && it != Ty::Str && it != Ty::Unknown && it != Ty::Any)
          error(g.iterable->pos, std::string("cannot iterate over ") + ty_name(it));
        push_scope();
        define(g.var, Ty::Unknown);
        check_expr(e->items[0]);
        if (g.cond) check_expr(g.cond);
        pop_scope();
      }
      return Ty::List;
    }

    case ExprKind::MapLit: {
      for (auto& f : e->fields) {
        Ty k = check_expr(f.first);
        if (k != Ty::Str && k != Ty::Int && k != Ty::Unknown && k != Ty::Any)
          error(f.first->pos, std::string("map keys must be String or Int, got ") + ty_name(k));
        check_expr(f.second);
      }
      return Ty::Map;
    }

    case ExprKind::Interp:
      for (auto* x : e->items) check_expr(x);
      for (auto* x : e->parts) check_expr(x);
      return Ty::Str;

    case ExprKind::Unary:  return check_unary(e);
    case ExprKind::Binary: return check_binary(e);

    case ExprKind::Logical:
      check_expr(e->a);
      check_expr(e->b);
      return Ty::Bool;

    case ExprKind::Assign:
      check_expr(e->a);
      check_expr(e->b);
      return Ty::Nil;

    case ExprKind::Ternary:
      check_expr(e->a);
      check_expr(e->b);
      check_expr(e->c);
      return Ty::Any;

    case ExprKind::Call: return check_call(e);

    case ExprKind::Index: {
      Ty base = check_expr(e->a);
      Ty idx = check_expr(e->b);
      if (base == Ty::Map) {
        if (idx != Ty::Str && idx != Ty::Int && idx != Ty::Unknown && idx != Ty::Any)
          error(e->pos, "map keys must be String or Int");
        return Ty::Any;
      }
      if (base == Ty::Str) return Ty::Str;
      if (base == Ty::List) {
        if (idx != Ty::Int && idx != Ty::Unknown && idx != Ty::Any)
          error(e->pos, std::string("list index must be an Int, got ") + ty_name(idx));
        return Ty::Any;
      }
      return Ty::Any;
    }

    case ExprKind::Member: {
      Ty base = check_expr(e->a);
      if (base == Ty::Str) {
        static const std::vector<std::string> fields = {
            "length", "upper", "lower", "trim", "split", "startswith",
            "endswith", "contains", "replace", "indexof", "chars", "bytes",
            "repeat", "find", "slice", "pad"};
        if (std::find(fields.begin(), fields.end(), e->name) == fields.end()) {
          // response.body / data.choices on a dynamic map is fine
          if (base != Ty::Map) {
            note(e->pos, "String has no field '" + e->name +
                             "' (did you mean .length?)");
          }
        }
        return Ty::Any;
      }
      return Ty::Any;
    }

    case ExprKind::Slice:
      check_expr(e->a);
      if (e->b) check_expr(e->b);
      if (e->c) check_expr(e->c);
      return Ty::Any;

    case ExprKind::Closure: {
      push_scope(true);
      for (auto& p : e->params) define(p.name, p.type ? type_from(p.type) : Ty::Unknown);
      check_block(e->body);
      pop_scope();
      return Ty::Func;
    }

    case ExprKind::Cast:
      check_expr(e->a);
      return type_from(e->type);

    case ExprKind::Error:
      return Ty::Error;
  }
  return Ty::Unknown;
}

}  // namespace ov
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>

#include "interp.h"

namespace ov {
using namespace ast;

static const int kMaxCallDepth = 2000;

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------
OvValue Interp::eval(const Expr* e, std::shared_ptr<Env> env) {
  switch (e->kind) {
    case ExprKind::IntLit:   return ov_int(e->ival);
    case ExprKind::FloatLit: return ov_float(e->fval);
    case ExprKind::BoolLit:  return ov_bool(e->bval);
    case ExprKind::NullLit:  return ov_nil();
    case ExprKind::StringLit:
      return ov_str(ov_str_new(e->sval.data(), e->sval.size()));

    case ExprKind::Identifier: {
      OvValue* slot = env ? env->find(e->name) : nullptr;
      if (slot) return *slot;
      if (functions_.count(e->name)) return ov_nil();  // bare name, rare
      throw Throw{ov_s(std::string("unknown name '") + e->name + "'")};
    }

    /* Building a list or map allocates, so every value already stored has to
     * stay reachable while the next element is evaluated (the collector may
     * run at any allocation). `root` is a shadow-stack slot the GC scans. */
    case ExprKind::ListLit: {
      OvList* l = ov_list_new();
      Root root(ov_list(l));
      for (auto* x : e->items) {
        OvValue v = eval(x, env);
        ov_list_push(l, v);
      }
      return root.get();
    }

    case ExprKind::MapLit: {
      OvMap* m = ov_map_new();
      Root root(ov_map(m));
      for (auto& f : e->fields) {
        Root k(eval(f.first, env));
        OvValue v = eval(f.second, env);
        ov_map_set(m, k.get(), v);
      }
      return root.get();
    }

    case ExprKind::Interp:
      return interpolate(e, env);

    case ExprKind::Unary: {
      OvValue v = eval(e->a, env);
      switch (e->op) {
        case Tok::KW_NOT: return ov_bool(!ov_truthy(v));
        case Tok::MINUS:
          if (ov_tagof(v) == OV_FLOAT) return ov_float(-v.f);
          return ov_int(-v.i);
        case Tok::PLUS: return v;
        case Tok::TILDE: return ov_int(~v.i);
        default: return ov_nil();
      }
    }

    case ExprKind::Binary: {
      // short-circuit and lazy cases first
      if (e->op == Tok::KW_IN) {
        Root needle(eval(e->a, env));
        OvValue hay = eval(e->b, env);
        return ov_bool(ov_in(needle.get(), hay));
      }
      Root l(eval(e->a, env));
      OvValue r = eval(e->b, env);
      switch (e->op) {
        case Tok::PLUS:         return ov_add(l.get(), r);
        case Tok::MINUS:        return ov_sub(l.get(), r);
        case Tok::STAR:         return ov_mul(l.get(), r);
        case Tok::SLASH:        return ov_div(l.get(), r);
        case Tok::PERCENT:      return ov_mod(l.get(), r);
        case Tok::DOUBLE_STAR: return ov_pow(l.get(), r);
        case Tok::AMP:          return ov_bitand(l.get(), r);
        case Tok::PIPE:         return ov_bitor(l.get(), r);
        case Tok::CARET:        return ov_bitxor(l.get(), r);
        case Tok::SHL:          return ov_lshift(l.get(), r);
        case Tok::SHR:          return ov_rshift(l.get(), r);
        case Tok::EQUAL:        return ov_bool(ov_eq(l.get(), r));
        case Tok::BANG_EQUAL:   return ov_bool(!ov_eq(l.get(), r));
        case Tok::LESS:         return ov_bool(ov_cmp(l.get(), r) < 0);
        case Tok::GREATER:      return ov_bool(ov_cmp(l.get(), r) > 0);
        case Tok::LESS_EQUAL:   return ov_bool(ov_cmp(l.get(), r) <= 0);
        case Tok::GREATER_EQUAL:return ov_bool(ov_cmp(l.get(), r) >= 0);
        default:
          throw Throw{ov_s(std::string("unsupported operator '") + tok_name(e->op) + "'")};
      }
    }

    case ExprKind::Logical: {
      Root l(eval(e->a, env));
      if (e->op == Tok::KW_AND) return ov_bool(ov_truthy(l.get()) && ov_truthy(eval(e->b, env)));
      return ov_bool(ov_truthy(l.get()) || ov_truthy(eval(e->b, env)));
    }

    case ExprKind::Ternary:
      return ov_truthy(eval(e->a, env)) ? eval(e->b, env) : eval(e->c, env);

    case ExprKind::Assign: {
      Root v(eval(e->b, env));
      if (e->op == Tok::ASSIGN) {
        assign_to(e->a, v.get(), env);
        return v.get();
      }
      OvValue cur = eval(e->a, env);
      OvValue out = ov_nil();
      switch (e->op) {
        case Tok::PLUS_EQUAL:    out = ov_add(cur, v.get()); break;
        case Tok::MINUS_EQUAL:   out = ov_sub(cur, v.get()); break;
        case Tok::STAR_EQUAL:    out = ov_mul(cur, v.get()); break;
        case Tok::SLASH_EQUAL:   out = ov_div(cur, v.get()); break;
        case Tok::PERCENT_EQUAL: out = ov_mod(cur, v.get()); break;
        default: break;
      }
      assign_to(e->a, out, env);
      return out;
    }

    case ExprKind::Index: {
      Root base(eval(e->a, env));
      OvValue idx = eval(e->b, env);
      return index_get(base.get(), idx);
    }

    case ExprKind::Slice: {
      Root base(eval(e->a, env));
      int64_t n = (ov_tagof(base.get()) == OV_STRING) ? base.get().str->len
                  : (ov_tagof(base.get()) == OV_LIST)   ? base.get().list->len
                                                 : -1;
      if (n < 0) throw Throw{ov_s(std::string("cannot slice ") + ov_type_name(base.get()))};
      Root lo(e->b ? eval(e->b, env) : ov_int(0));
      OvValue hi = e->c ? eval(e->c, env) : ov_int(n);
      int64_t a = lo.get().i, b = hi.i;
      if (a < 0) a += n;
      if (b < 0) b += n;
      if (a < 0) a = 0;
      if (b > n) b = n;
      if (b < a) b = a;
      if (ov_tagof(base.get()) == OV_STRING)
        return ov_str(ov_str_slice(base.get().str, a, b));
      OvList* out = ov_list_new();
      for (int64_t i = a; i < b; i++) ov_list_push(out, ov_list_get(base.get().list, i));
      return ov_list(out);
    }

    case ExprKind::Member: {
      OvValue base = eval(e->a, env);
      return member_get(base, e->name, e);
    }

    case ExprKind::ListComp: {
      OvList* out = ov_list_new();
      Root root(ov_list(out));
      std::function<void(size_t, std::shared_ptr<Env>)> recurse =
          [&](size_t gi, std::shared_ptr<Env> cur_env) {
        if (gi == e->generators.size()) {
          OvValue v = eval(e->items[0], cur_env);
          ov_list_push(out, v);
          return;
        }
        const Generator& g = e->generators[gi];
        Root it(eval(g.iterable, cur_env));
        if (ov_tagof(it.get()) == OV_LIST) {
          for (uint32_t i = 0; i < it.get().list->len; i++) {
            auto inner = std::make_shared<Env>(cur_env);
            inner->vars[g.var] = ov_list_get(it.get().list, i);
            if (g.cond && !ov_truthy(eval(g.cond, inner))) continue;
            recurse(gi + 1, inner);
          }
        } else if (ov_tagof(it.get()) == OV_STRING) {
          Root chars(ov_str_chars(it.get().str));
          for (uint32_t i = 0; i < chars.get().list->len; i++) {
            auto inner = std::make_shared<Env>(cur_env);
            inner->vars[g.var] = chars.get().list->items[i];
            if (g.cond && !ov_truthy(eval(g.cond, inner))) continue;
            recurse(gi + 1, inner);
          }
        }
      };
      recurse(0, env);
      return root.get();
    }

    case ExprKind::Closure: {
      // Capture the current environment by shared_ptr so the closure can
      // read outer locals even after the enclosing scope exits.
      auto* cl = new ClosureObj{e, env, "closure"};
      OvFunc* f = (OvFunc*)calloc(1, sizeof(OvFunc));
      f->name = ov_str_cstr("closure");
      f->arity = (int)e->params.size();
      f->upvals = cl;
      f->call = nullptr;
      return ov_func(f);
    }

    case ExprKind::Cast: {
      OvValue v = eval(e->a, env);
      if (!e->type) return v;
      const std::string& n = e->type->name;
      if (n == "Int") {
        if (ov_tagof(v) == OV_FLOAT) return ov_int((int64_t)v.f);
        if (ov_tagof(v) == OV_BOOL) return ov_int(v.b);
        if (ov_tagof(v) == OV_INT) return v;
      }
      if (n == "Float") {
        if (ov_tagof(v) == OV_INT) return ov_float((double)v.i);
        if (ov_tagof(v) == OV_FLOAT) return v;
      }
      if (n == "String" || n == "Str") return str_of(v);
      return v;
    }

    case ExprKind::Call: {
      // Resolve the callee to a value so local closures and user functions
      // become callable. A bare identifier that is a local / closure value is
      // read from the environment; a `Member` callee (e.g. `xs.push`) is left
      // as nil so the qualified dispatch in call_builtin handles the value
      // method; an unknown bare name degrades to nil and the dispatch below
      // knows the global builtins.
      OvValue callee = ov_nil();
      if (e->a && e->a->kind == ExprKind::Identifier) {
        OvValue* slot = env ? env->find(e->a->name) : nullptr;
        if (slot) callee = *slot;
      } else if (e->a && e->a->kind != ExprKind::Member) {
        // Calling the result of an expression: a closure stored in a list or
        // map (pair[0](...)), or any other computed callee. Evaluate it to a
        // value; call_value dispatches it through the OV_FUNC closure path.
        callee = eval(e->a, env);
      }
      // Forward the named arguments: `call_value` re-evaluates both the
      // positional `call->args` and `e->named_args` internally, so only the
      // named vector needs to reach the named-argument dispatch.
      return call_value(callee, e, env, {}, e->named_args);
    }

    case ExprKind::Error:
      return ov_nil();
  }
  return ov_nil();
}

// ---------------------------------------------------------------------------
// calls
// ---------------------------------------------------------------------------
OvValue Interp::call_closure(const ClosureObj& cl, const std::vector<OvValue>& args) {
  const Expr* e = cl.expr;
  // Create a new scope whose parent is the captured environment, so the
  // closure body can read outer locals from the lexical scope it closed over.
  auto local = std::make_shared<Env>(cl.env, Env::FUNCTION);
  for (size_t i = 0; i < e->params.size(); i++) {
    OvValue v = i < args.size() ? args[i] : ov_nil();
    if (i >= args.size() && e->params[i].default_value)
      v = eval(e->params[i].default_value, cl.env);
    local->vars[e->params[i].name] = v;
  }
  Signal saved = signal_;
  signal_ = Signal{};
  exec_block(e->body, local);
  OvValue out = signal_.flow == Flow::Return ? signal_.value : ov_nil();
  signal_ = saved;
  return out;
}

OvValue Interp::call_function(const FnDef& fn, const ExprList& arg_exprs,
                               std::shared_ptr<Env> env) {
  const Stmt* decl = fn.decl;
  if (++depth_ > kMaxCallDepth) {
    depth_--;
    throw Throw{ov_s("maximum call depth exceeded (" + std::to_string(kMaxCallDepth) + ")")};
  }
  auto local = std::make_shared<Env>(fn.closure, Env::FUNCTION);
  for (size_t i = 0; i < decl->params.size(); i++) {
    OvValue v;
    if (i < arg_exprs.size()) {
      v = eval(arg_exprs[i], env);
    } else if (decl->params[i].default_value) {
      v = eval(decl->params[i].default_value, env);
    } else {
      v = ov_nil();
    }
    local->vars[decl->params[i].name] = v;
  }
  Signal saved = signal_;
  signal_ = Signal{};
  exec_block(decl->body, local);
  OvValue out = signal_.flow == Flow::Return ? signal_.value : ov_nil();
  signal_ = saved;
  depth_--;
  return out;
}

OvValue Interp::call_value(OvValue callee, const Expr* site, std::shared_ptr<Env> env,
                            const std::vector<OvValue>& args,
                            const std::vector<NamedArg>& named) {
  (void)named;
  const Expr* call = site;
  const Expr* callee_expr = call->a;

  // named-argument call on a user function: bind by parameter name
  if (!named.empty() && callee_expr->kind == ExprKind::Identifier) {
    auto it = functions_.find(callee_expr->name);
    if (it != functions_.end()) {
      const Stmt* decl = it->second.decl;
      std::vector<OvValue> byname(decl->params.size(), ov_nil());
      size_t i = 0;
      for (auto* a : call->args) {
        if (i < byname.size()) byname[i++] = eval(a, env);
      }
      for (const auto& na : named) {
        bool found = false;
        for (size_t k = 0; k < decl->params.size(); k++)
          if (decl->params[k].name == na.name) {
            byname[k] = eval(na.value, env);
            found = true;
          }
        if (!found)
          throw Throw{ov_s(std::string("'") + decl->name + "' has no parameter named '" + na.name + "'")};
      }
      if (++depth_ > kMaxCallDepth) { depth_--; throw Throw{ov_s("stack overflow")}; }
      auto local = std::make_shared<Env>(it->second.closure, Env::FUNCTION);
      for (size_t k = 0; k < decl->params.size(); k++) {
        if (i > k && ov_isnil(byname[k]) && decl->params[k].default_value)
          byname[k] = eval(decl->params[k].default_value, env);
        local->vars[decl->params[k].name] = byname[k];
      }
      Signal saved = signal_;
      signal_ = Signal{};
      exec_block(decl->body, local);
      OvValue out = signal_.flow == Flow::Return ? signal_.value : ov_nil();
      signal_ = saved;
      depth_--;
      return out;
    }
  }

  // user function
  if (callee_expr->kind == ExprKind::Identifier) {
    auto it = functions_.find(callee_expr->name);
    if (it != functions_.end()) {
      std::vector<OvValue> pos;
      for (auto* a : call->args) pos.push_back(eval(a, env));
      if (++depth_ > kMaxCallDepth) { depth_--; throw Throw{ov_s("stack overflow")}; }
      auto local = std::make_shared<Env>(it->second.closure, Env::FUNCTION);
      const Stmt* decl = it->second.decl;
      for (size_t i = 0; i < decl->params.size(); i++) {
        OvValue v = i < pos.size() ? pos[i] : ov_nil();
        if (i >= pos.size() && decl->params[i].default_value)
          v = eval(decl->params[i].default_value, env);
        local->vars[decl->params[i].name] = v;
      }
      Signal saved = signal_;
      signal_ = Signal{};
      exec_block(decl->body, local);
      OvValue out = signal_.flow == Flow::Return ? signal_.value : ov_nil();
      signal_ = saved;
      depth_--;
      return out;
    }
  }

  // closure value: dispatch through the captured ClosureObj
  if (ov_tagof(callee) == OV_FUNC && callee.fn->upvals) {
    auto* cl = (ClosureObj*)callee.fn->upvals;
    std::vector<OvValue> pos;
    for (auto* a : call->args) pos.push_back(eval(a, env));
    if (++depth_ > kMaxCallDepth) { depth_--; throw Throw{ov_s("stack overflow")}; }
    OvValue out = call_closure(*cl, pos);
    depth_--;
    return out;
  }

  bool handled = false;
  OvValue out = call_builtin("", call, env, call->args, call->named_args, &handled);
  if (handled) return out;

  throw Throw{ov_s("not callable")};
}

}  // namespace ov
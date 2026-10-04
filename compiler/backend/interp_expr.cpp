#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstring>

#include "interp.h"

namespace vy {
using namespace ast;

static const int kMaxCallDepth = 2000;

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------
VyValue Interp::eval(const Expr* e, Env& env) {
  switch (e->kind) {
    case ExprKind::IntLit:   return vy_int(e->ival);
    case ExprKind::FloatLit: return vy_float(e->fval);
    case ExprKind::BoolLit:  return vy_bool(e->bval);
    case ExprKind::NullLit:  return vy_nil();
    case ExprKind::StringLit:
      return vy_str(vy_str_new(e->sval.data(), e->sval.size()));

    case ExprKind::Identifier: {
      VyValue* slot = env.find(e->name);
      if (slot) return *slot;
      if (functions_.count(e->name)) return vy_nil();  // bare name, rare
      throw Throw{vy_s(std::string("unknown name '") + e->name + "'")};
    }

    /* Building a list or map allocates, so every value already stored has to
     * stay reachable while the next element is evaluated (the collector may
     * run at any allocation). `root` is a shadow-stack slot the GC scans. */
    case ExprKind::ListLit: {
      VyList* l = vy_list_new();
      VyValue root = vy_list(l);
      VyValue* slot = open_root();
      *slot = root;
      for (auto* x : e->items) {
        VyValue v = eval(x, env);
        vy_list_push(l, v);
        *slot = root;  // re-assert: eval() may have moved the stack
      }
      close_root();
      return root;
    }

    case ExprKind::MapLit: {
      VyMap* m = vy_map_new();
      VyValue root = vy_map(m);
      VyValue* slot = open_root();
      *slot = root;
      for (auto& f : e->fields) {
        VyValue k = eval(f.first, env);
        VyValue v = eval(f.second, env);
        *slot = root;
        vy_map_set(m, k, v);
      }
      close_root();
      return root;
    }

    case ExprKind::Interp:
      return interpolate(e, env);

    case ExprKind::Unary: {
      VyValue v = eval(e->a, env);
      switch (e->op) {
        case Tok::KW_NOT: return vy_bool(!vy_truthy(v));
        case Tok::MINUS:
          if (vy_tagof(v) == VY_FLOAT) return vy_float(-v.f);
          return vy_int(-v.i);
        case Tok::PLUS: return v;
        case Tok::TILDE: return vy_int(~v.i);
        default: return vy_nil();
      }
    }

    case ExprKind::Binary: {
      // short-circuit and lazy cases first
      if (e->op == Tok::KW_IN) {
        VyValue needle = eval(e->a, env);
        VyValue hay = eval(e->b, env);
        return vy_bool(vy_in(needle, hay));
      }
      VyValue l = eval(e->a, env);
      VyValue r = eval(e->b, env);
      switch (e->op) {
        case Tok::PLUS:         return vy_add(l, r);
        case Tok::MINUS:        return vy_sub(l, r);
        case Tok::STAR:         return vy_mul(l, r);
        case Tok::SLASH:        return vy_div(l, r);
        case Tok::PERCENT:      return vy_mod(l, r);
        case Tok::DOUBLE_STAR: return vy_pow(l, r);
        case Tok::AMP:          return vy_bitand(l, r);
        case Tok::PIPE:         return vy_bitor(l, r);
        case Tok::CARET:        return vy_bitxor(l, r);
        case Tok::SHL:          return vy_lshift(l, r);
        case Tok::SHR:          return vy_rshift(l, r);
        case Tok::EQUAL:        return vy_bool(vy_eq(l, r));
        case Tok::BANG_EQUAL:   return vy_bool(!vy_eq(l, r));
        case Tok::LESS:         return vy_bool(vy_cmp(l, r) < 0);
        case Tok::GREATER:      return vy_bool(vy_cmp(l, r) > 0);
        case Tok::LESS_EQUAL:   return vy_bool(vy_cmp(l, r) <= 0);
        case Tok::GREATER_EQUAL:return vy_bool(vy_cmp(l, r) >= 0);
        default:
          throw Throw{vy_s(std::string("unsupported operator '") + tok_name(e->op) + "'")};
      }
    }

    case ExprKind::Logical: {
      VyValue l = eval(e->a, env);
      if (e->op == Tok::KW_AND) return vy_bool(vy_truthy(l) && vy_truthy(eval(e->b, env)));
      return vy_bool(vy_truthy(l) || vy_truthy(eval(e->b, env)));
    }

    case ExprKind::Ternary:
      return vy_truthy(eval(e->a, env)) ? eval(e->b, env) : eval(e->c, env);

    case ExprKind::Assign: {
      VyValue v = eval(e->b, env);
      if (e->op == Tok::ASSIGN) {
        assign_to(e->a, v, env);
        return v;
      }
      VyValue cur = eval(e->a, env);
      VyValue out = vy_nil();
      switch (e->op) {
        case Tok::PLUS_EQUAL:    out = vy_add(cur, v); break;
        case Tok::MINUS_EQUAL:   out = vy_sub(cur, v); break;
        case Tok::STAR_EQUAL:    out = vy_mul(cur, v); break;
        case Tok::SLASH_EQUAL:   out = vy_div(cur, v); break;
        case Tok::PERCENT_EQUAL: out = vy_mod(cur, v); break;
        default: break;
      }
      assign_to(e->a, out, env);
      return out;
    }

    case ExprKind::Index: {
      VyValue base = eval(e->a, env);
      VyValue idx = eval(e->b, env);
      return index_get(base, idx);
    }

    case ExprKind::Slice: {
      VyValue base = eval(e->a, env);
      int64_t n = (vy_tagof(base) == VY_STRING) ? base.str->len
                  : (vy_tagof(base) == VY_LIST)   ? base.list->len
                                                 : -1;
      if (n < 0) throw Throw{vy_s(std::string("cannot slice ") + vy_type_name(base))};
      VyValue lo = e->b ? eval(e->b, env) : vy_int(0);
      VyValue hi = e->c ? eval(e->c, env) : vy_int(n);
      int64_t a = lo.i, b = hi.i;
      if (a < 0) a += n;
      if (b < 0) b += n;
      if (a < 0) a = 0;
      if (b > n) b = n;
      if (b < a) b = a;
      if (vy_tagof(base) == VY_STRING)
        return vy_str(vy_str_slice(base.str, a, b));
      VyList* out = vy_list_new();
      for (int64_t i = a; i < b; i++) vy_list_push(out, vy_list_get(base.list, i));
      return vy_list(out);
    }

    case ExprKind::Member: {
      VyValue base = eval(e->a, env);
      return member_get(base, e->name, e);
    }

    case ExprKind::ListComp: {
      VyList* out = vy_list_new();
      VyValue root = vy_list(out);
      VyValue* slot = open_root();
      *slot = root;
      std::function<void(size_t)> recurse = [&](size_t gi) {
        if (gi == e->generators.size()) {
          VyValue v = eval(e->items[0], env);
          *slot = root;
          vy_list_push(out, v);
          return;
        }
        const Generator& g = e->generators[gi];
        VyValue it = eval(g.iterable, env);
        *slot = root;
        if (vy_tagof(it) == VY_LIST) {
          for (uint32_t i = 0; i < it.list->len; i++) {
            env.vars[g.var] = vy_list_get(it.list, i);
            if (g.cond && !vy_truthy(eval(g.cond, env))) continue;
            recurse(gi + 1);
          }
        } else if (vy_tagof(it) == VY_STRING) {
          VyValue chars = vy_str_chars(it.str);
          for (uint32_t i = 0; i < chars.list->len; i++) {
            env.vars[g.var] = chars.list->items[i];
            if (g.cond && !vy_truthy(eval(g.cond, env))) continue;
            recurse(gi + 1);
          }
        }
      };
      // the comprehension's loop variables are visible to later generators,
      // which is why this reuses the caller's env
      recurse(0);
      close_root();
      return root;
    }

    case ExprKind::Closure: {
      auto* cl = new ClosureObj{e, &env, "closure"};
      VyFunc* f = (VyFunc*)calloc(1, sizeof(VyFunc));
      f->name = vy_str_cstr("closure");
      f->arity = (int)e->params.size();
      f->upvals = cl;
      f->call = nullptr;
      return vy_func(f);
    }

    case ExprKind::Cast: {
      VyValue v = eval(e->a, env);
      if (!e->type) return v;
      const std::string& n = e->type->name;
      if (n == "Int") {
        if (vy_tagof(v) == VY_FLOAT) return vy_int((int64_t)v.f);
        if (vy_tagof(v) == VY_BOOL) return vy_int(v.b);
        if (vy_tagof(v) == VY_INT) return v;
      }
      if (n == "Float") {
        if (vy_tagof(v) == VY_INT) return vy_float((double)v.i);
        if (vy_tagof(v) == VY_FLOAT) return v;
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
      VyValue callee = vy_nil();
      if (e->a && e->a->kind == ExprKind::Identifier) {
        VyValue* slot = env.find(e->a->name);
        if (slot) callee = *slot;
      }
      // Forward the named arguments: `call_value` re-evaluates both the
      // positional `call->args` and `e->named_args` internally, so only the
      // named vector needs to reach the named-argument dispatch.
      return call_value(callee, e, env, {}, e->named_args);
    }

    case ExprKind::Error:
      return vy_nil();
  }
  return vy_nil();
}

// ---------------------------------------------------------------------------
// calls
// ---------------------------------------------------------------------------
VyValue Interp::call_closure(const ClosureObj& cl, const std::vector<VyValue>& args) {
  const Expr* e = cl.expr;
  Env local(cl.env);
  for (size_t i = 0; i < e->params.size(); i++) {
    VyValue v = i < args.size() ? args[i] : vy_nil();
    if (i >= args.size() && e->params[i].default_value)
      v = eval(e->params[i].default_value, *cl.env);
    local.vars[e->params[i].name] = v;
  }
  Signal saved = signal_;
  signal_ = Signal{};
  exec_block(e->body, local);
  VyValue out = signal_.flow == Flow::Return ? signal_.value : vy_nil();
  signal_ = saved;
  return out;
}

VyValue Interp::call_function(const FnDef& fn, const ExprList& arg_exprs, Env& env) {
  const Stmt* decl = fn.decl;
  if (++depth_ > kMaxCallDepth) {
    depth_--;
    throw Throw{vy_s("maximum call depth exceeded (" + std::to_string(kMaxCallDepth) + ")")};
  }
  Env local(fn.closure);
  for (size_t i = 0; i < decl->params.size(); i++) {
    VyValue v;
    if (i < arg_exprs.size()) {
      v = eval(arg_exprs[i], env);
    } else if (decl->params[i].default_value) {
      v = eval(decl->params[i].default_value, env);
    } else {
      v = vy_nil();
    }
    local.vars[decl->params[i].name] = v;
  }
  Signal saved = signal_;
  signal_ = Signal{};
  exec_block(decl->body, local);
  VyValue out = signal_.flow == Flow::Return ? signal_.value : vy_nil();
  signal_ = saved;
  depth_--;
  return out;
}

VyValue Interp::call_value(VyValue callee, const Expr* site, Env& env,
                            const std::vector<VyValue>& args,
                            const std::vector<NamedArg>& named) {
  (void)named;
  const Expr* call = site;
  const Expr* callee_expr = call->a;

  // named-argument call on a user function: bind by parameter name
  if (!named.empty() && callee_expr->kind == ExprKind::Identifier) {
    auto it = functions_.find(callee_expr->name);
    if (it != functions_.end()) {
      const Stmt* decl = it->second.decl;
      std::vector<VyValue> byname(decl->params.size(), vy_nil());
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
          throw Throw{vy_s(std::string("'") + decl->name + "' has no parameter named '" + na.name + "'")};
      }
      if (++depth_ > kMaxCallDepth) { depth_--; throw Throw{vy_s("stack overflow")}; }
      Env local(it->second.closure);
      for (size_t k = 0; k < decl->params.size(); k++) {
        if (i > k && vy_isnil(byname[k]) && decl->params[k].default_value)
          byname[k] = eval(decl->params[k].default_value, env);
        local.vars[decl->params[k].name] = byname[k];
      }
      Signal saved = signal_;
      signal_ = Signal{};
      exec_block(decl->body, local);
      VyValue out = signal_.flow == Flow::Return ? signal_.value : vy_nil();
      signal_ = saved;
      depth_--;
      return out;
    }
  }

  // user function
  if (callee_expr->kind == ExprKind::Identifier) {
    auto it = functions_.find(callee_expr->name);
    if (it != functions_.end()) {
      std::vector<VyValue> pos;
      for (auto* a : call->args) pos.push_back(eval(a, env));
      if (++depth_ > kMaxCallDepth) { depth_--; throw Throw{vy_s("stack overflow")}; }
      Env local(it->second.closure);
      const Stmt* decl = it->second.decl;
      for (size_t i = 0; i < decl->params.size(); i++) {
        VyValue v = i < pos.size() ? pos[i] : vy_nil();
        if (i >= pos.size() && decl->params[i].default_value)
          v = eval(decl->params[i].default_value, env);
        local.vars[decl->params[i].name] = v;
      }
      Signal saved = signal_;
      signal_ = Signal{};
      exec_block(decl->body, local);
      VyValue out = signal_.flow == Flow::Return ? signal_.value : vy_nil();
      signal_ = saved;
      depth_--;
      return out;
    }
  }

  // closure value: dispatch through the captured ClosureObj
  if (vy_tagof(callee) == VY_FUNC && callee.fn->upvals) {
    auto* cl = (ClosureObj*)callee.fn->upvals;
    std::vector<VyValue> pos;
    for (auto* a : call->args) pos.push_back(eval(a, env));
    if (++depth_ > kMaxCallDepth) { depth_--; throw Throw{vy_s("stack overflow")}; }
    VyValue out = call_closure(*cl, pos);
    depth_--;
    return out;
  }

  bool handled = false;
  VyValue out = call_builtin("", call, env, call->args, call->named_args, &handled);
  if (handled) return out;

  throw Throw{vy_s("not callable")};
}

}  // namespace vy
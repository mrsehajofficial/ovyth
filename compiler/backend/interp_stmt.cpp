#include "interp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace vy {

using namespace ast;

// ---------------------------------------------------------------------------
Interp::Interp(const Program& program, std::string filename)
    : prog_(program), file_(std::move(filename)), globals_(new Env(nullptr)) {}

Interp::~Interp() {}

// The shadow stack lives in a deque-like chunked buffer so a VyValue* handed
// out stays valid even when the vector reallocates.
static std::deque<VyValue>* g_root_pool = nullptr;

VyValue* Interp::open_root() {
  if (!g_root_pool) g_root_pool = new std::deque<VyValue>();
  g_root_pool->emplace_back();
  VyValue* slot = &g_root_pool->back();
  vy_gc_register_root(slot);
  root_slots_.push_back(slot);
  return slot;
}

void Interp::close_root() {
  while (!root_slots_.empty()) {
    VyValue* slot = root_slots_.back();
    root_slots_.pop_back();
    vy_gc_unregister_root(slot);
    // the deque may have popped the slot we just released; rebuild lazily
  }
}

int Interp::run() {
  // hoist function declarations so order in the file does not matter
  for (auto* s : prog_.statements) {
    if (s && s->kind == StmtKind::FuncDecl) {
      FnDef def;
      def.decl = s;
      def.closure = globals_;
      def.name = s->name;
      functions_[s->name] = def;
      globals_->vars[s->name] = vy_nil();  // reserve the name
    }
  }
  // `http`, `json`, `str`, `math` and `time` are namespaces: maps holding their
  // methods as closures. `time` needs no entries -- `time.clock` / `time.now`
  // are recognised by name -- but the entry must exist, or a dotted call would
  // try to evaluate the bare identifier `time` and fail.
  VyMap* http_ns = vy_map_new();
  VyMap* json_ns = vy_map_new();
  VyMap* str_ns = vy_map_new();
  VyMap* math_ns = vy_map_new();
  VyMap* time_ns = vy_map_new();
  globals_->vars["http"] = vy_map(http_ns);
  globals_->vars["json"] = vy_map(json_ns);
  globals_->vars["str"] = vy_map(str_ns);
  globals_->vars["math"] = vy_map(math_ns);
  globals_->vars["time"] = vy_map(time_ns);

  try {
    exec_block(prog_.statements, *globals_);
  } catch (Throw& t) {
    VyStr* s = vy_repr(t.value);
    fprintf(stderr, "\nvayu: uncaught error: %s\n", vy_str_data(s));
    vy_runtime_shutdown();
    return 70;
  }
  vy_runtime_shutdown();
  return exit_code_;
}

void Interp::exec_block(const StmtList& body, Env& env) {
  for (auto* s : body) {
    exec_stmt(s, env);
    if (signal_.flow != Flow::Normal) return;
  }
}

void Interp::exec_stmt(const Stmt* s, Env& env) {
  if (!s) return;
  switch (s->kind) {
    case StmtKind::VarDecl: {
      // multi-assignment `a, b = f()`: evaluate all value expressions before
      // binding, and keep every partial result reachable while the next one is
      // evaluated (the collector may run at any allocation).
      size_t n = s->names.size();
      std::vector<VyValue> tmp(n);
      for (size_t i = 0; i < n; i++) {
        VyValue* slot = open_root();
        *slot = tmp[i];
        tmp[i] = (i < s->values.size() && s->values[i]) ? eval(s->values[i], env) : vy_nil();
      }
      // unpack: `a, b = [1, 2]` puts a single iterable on the right.
      if (n > 1 && s->values.size() == 1 && vy_tagof(tmp[0]) == VY_LIST) {
        VyList* l = tmp[0].list;
        for (size_t i = 0; i < n; i++) {
          VyValue* slot = open_root();
          *slot = tmp[i];
          tmp[i] = i < l->len ? vy_list_get(l, (int64_t)i) : vy_nil();
        }
      }
      while (!root_slots_.empty()) {
        vy_gc_unregister_root(root_slots_.back());
        root_slots_.pop_back();
      }
      // Python-like rebinding: a name that already exists anywhere up the
      // scope chain is updated *in place*; only genuinely new names are bound
      // locally in this block. Without this, `for i in ... { total = total + i }`
      // would shadow the outer `total` and the accumulation would be lost.
      for (size_t i = 0; i < n; i++) {
        VyValue* slot = env.find(s->names[i]);
        if (slot) *slot = tmp[i];
        else env.vars[s->names[i]] = tmp[i];
      }
      return;
    }

    case StmtKind::ExprStmt:
      eval(s->expr, env);
      return;

    case StmtKind::Assign: {
      VyValue v = eval(s->expr2, env);
      if (s->op == Tok::ASSIGN) {
        assign_to(s->expr, v, env);
        return;
      }
      // compound:  x += e   =>   x = x + e
      VyValue cur = eval(s->expr, env);
      VyValue out = vy_nil();
      switch (s->op) {
        case Tok::PLUS_EQUAL:    out = vy_add(cur, v); break;
        case Tok::MINUS_EQUAL:   out = vy_sub(cur, v); break;
        case Tok::STAR_EQUAL:    out = vy_mul(cur, v); break;
        case Tok::SLASH_EQUAL:   out = vy_div(cur, v); break;
        case Tok::PERCENT_EQUAL: out = vy_mod(cur, v); break;
        default: break;
      }
      assign_to(s->expr, out, env);
      return;
    }

    case StmtKind::FuncDecl: {
      FnDef def;
      def.decl = s;
      def.closure = &env;
      def.name = s->name;
      functions_[s->name] = def;
      return;  // hoisted; the name already exists
    }

    case StmtKind::Return:
      signal_.flow = Flow::Return;
      signal_.value = s->expr ? eval(s->expr, env) : vy_nil();
      return;

    case StmtKind::If: {
      if (vy_truthy(eval(s->expr, env))) {
        Env inner(&env);
        exec_block(s->body, inner);
      } else if (!s->else_body.empty()) {
        Env inner(&env);
        exec_block(s->else_body, inner);
      }
      return;
    }

    case StmtKind::While: {
      for (;;) {
        if (!vy_truthy(eval(s->expr, env))) break;
        Env inner(&env);
        exec_block(s->body, inner);
        if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
        if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
        if (signal_.flow != Flow::Normal) return;
      }
      return;
    }

    case StmtKind::For: {
      VyValue it = eval(s->expr, env);
      switch (vy_tagof(it)) {
        case VY_LIST: {
          VyList* l = it.list;
          for (uint32_t i = 0; i < l->len; i++) {
            Env inner(&env);
            if (s->iter_vars.size() == 1) {
              inner.vars[s->iter_vars[0]] = vy_list_get(l, i);
            } else {
              VyValue pair = vy_list_get(l, i);
              for (size_t k = 0; k < s->iter_vars.size(); k++)
                inner.vars[s->iter_vars[k]] =
                    vy_list_get(pair.list, (int64_t)k);
            }
            exec_block(s->body, inner);
            if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
            if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
            if (signal_.flow != Flow::Normal) return;
          }
          return;
        }
        case VY_STRING: {
          VyValue chars = vy_str_chars(it.str);
          for (uint32_t i = 0; i < chars.list->len; i++) {
            Env inner(&env);
            inner.vars[s->iter_vars[0]] = chars.list->items[i];
            exec_block(s->body, inner);
            if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
            if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
            if (signal_.flow != Flow::Normal) return;
          }
          return;
        }
        case VY_MAP: {
          VyValue keys = vy_map_keys(it.map);
          for (uint32_t i = 0; i < keys.list->len; i++) {
            Env inner(&env);
            inner.vars[s->iter_vars[0]] = keys.list->items[i];
            exec_block(s->body, inner);
            if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
            if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
            if (signal_.flow != Flow::Normal) return;
          }
          return;
        }
        default:
          throw Throw{vy_s(std::string("cannot loop over ") + vy_type_name(it))};
      }
    }

    case StmtKind::Block: {
      Env inner(&env);
      exec_block(s->body, inner);
      return;
    }

    case StmtKind::Break:    signal_.flow = Flow::Break; return;
    case StmtKind::Continue: signal_.flow = Flow::Continue; return;

    case StmtKind::Try: {
      // Two layers of error: runtime C-level panics (e.g. division by zero)
      // unwind via the C setjmp/longjmp machinery, while user `throw` statements
      // raise a C++ Throw. Catch both so a single try/catch binds either to the
      // catch variable.
      jmp_buf* target = vy_try_push();
      if (setjmp(*target) == 0) {
        Env inner(&env);
        bool user_throw = false;
        VyValue user_val = vy_nil();
        try {
          exec_block(s->body, inner);
        } catch (Throw& t) {
          user_throw = true;
          user_val = t.value;
        }
        vy_try_pop();
        if (!user_throw) {
          if (signal_.flow == Flow::Exit || signal_.flow == Flow::Return) return;
          return;  // try body completed normally
        }
        // user throw: keep the value reachable while the catch block runs
        VyValue* slot = open_root();
        *slot = user_val;
        if (!s->catch_var.empty()) inner.vars[s->catch_var] = user_val;
        exec_block(s->else_body, inner);
        close_root();
      } else {
        // setjmp returned non-zero: a C-level raise() longjmp'd here.
        vy_try_pop();
        Env inner(&env);
        if (!s->catch_var.empty()) inner.vars[s->catch_var] = vy_caught;
        exec_block(s->else_body, inner);
      }
      return;
    }

    case StmtKind::Throw: {
      VyValue v = s->expr ? eval(s->expr, env) : vy_str_val("thrown");
      Throw t;
      t.value = v;
      throw t;
    }

    case StmtKind::Debug:
      fprintf(stderr, "[vayu] %s:%d\n", file_.c_str(), s->pos.line);
      return;
  }
}

void Interp::assign_to(Expr* target, VyValue value, Env& env) {
  switch (target->kind) {
    case ExprKind::Identifier: {
      VyValue* slot = env.find(target->name);
      if (slot) *slot = value;
      else env.vars[target->name] = value;
      return;
    }
    case ExprKind::Index: {
      VyValue base = eval(target->a, env);
      VyValue idx = eval(target->b, env);
      if (vy_tagof(base) == VY_LIST) {
        vy_list_set(base.list, idx.i, value);
        return;
      }
      if (vy_tagof(base) == VY_MAP) {
        vy_map_set(base.map, idx, value);
        return;
      }
      throw Throw{vy_s(std::string("cannot index-assign into ") + vy_type_name(base))};
    }
    case ExprKind::Member: {
      VyValue base = eval(target->a, env);
      if (vy_tagof(base) != VY_MAP)
        throw Throw{vy_s(std::string("cannot set field '") + target->name + "' on " + vy_type_name(base))};
      vy_map_set(base.map, vy_s(target->name), value);
      return;
    }
    default:
      throw Throw{vy_s("invalid assignment target")};
  }
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------
VyValue Interp::interpolate(const Expr* e, Env& env) {
  size_t n = e->items.size();
  size_t m = e->parts.size();
  VyStr* acc = vy_str_new("", 0);
  size_t li = 0;
  for (size_t i = 0; i < m; i++) {
    if (li < n) {
      const std::string& t = e->items[li++]->sval;
      acc = vy_str_concat(acc, vy_str_new(t.data(), t.size()));
    }
    VyValue v = eval(e->parts[i], env);
    acc = vy_str_concat(acc, vy_render(v));
  }
  for (; li < n; li++) {
    const std::string& t = e->items[li]->sval;
    acc = vy_str_concat(acc, vy_str_new(t.data(), t.size()));
  }
  return vy_str(acc);
}

VyValue Interp::index_get(VyValue base, VyValue idx) {
  switch (vy_tagof(base)) {
    case VY_LIST:
      if (vy_tagof(idx) == VY_LIST || vy_tagof(idx) == VY_MAP)
        throw Throw{vy_s("list index must be an Int")};
      return vy_list_get(base.list, vy_tagof(idx) == VY_FLOAT ? (int64_t)idx.f : idx.i);
    case VY_MAP:
      return vy_map_get(base.map, idx);
    case VY_STRING:
      if (vy_tagof(idx) == VY_STRING) {
        int64_t at = vy_str_find(base.str, idx.str, 0);
        return at < 0 ? vy_nil() : vy_int(at);
      }
      return vy_str(vy_str_slice(base.str, idx.i, idx.i + 1));
    default:
      throw Throw{vy_s(std::string("cannot index a ") + vy_type_name(base))};
  }
}

VyValue Interp::member_get(VyValue base, const std::string& name, const Expr* site) {
  (void)site;
  switch (vy_tagof(base)) {
    case VY_MAP:
      return vy_map_get(base.map, vy_s(name));
    case VY_STRING: {
      if (name == "length") return vy_int(base.str->len);
      auto s1 = vy_str(base.str);
      VyStr* r = nullptr;
      if (name == "upper") r = vy_str_upper(s1.str);
      else if (name == "lower") r = vy_str_lower(s1.str);
      else if (name == "trim") r = vy_str_trim(s1.str);
      else if (name == "chars") return vy_str_chars(s1.str);
      else if (name == "bytes") return vy_str_bytes(s1.str);
      else if (name == "split") return vy_str_split(s1.str, vy_str_new(",", 1));
      else throw Throw{vy_s(std::string("String has no field '") + name + "'")};
      return vy_str(r);
    }
    case VY_LIST:
      if (name == "length") return vy_int(base.list->len);
      throw Throw{vy_s(std::string("List has no field '") + name + "'")};
    case VY_NIL:
      throw Throw{vy_s(std::string("cannot read field '") + name + "' of nil")};
    default:
      throw Throw{vy_s(std::string("cannot read field '") + name + "' from " + vy_type_name(base))};
  }
}

}  // namespace vy
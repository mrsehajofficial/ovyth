#include "interp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace ov {

using namespace ast;

// ---------------------------------------------------------------------------
Interp::Interp(const Program& program, std::string filename)
    : prog_(program), file_(std::move(filename)), globals_(std::make_shared<Env>(nullptr)) {}

Interp::~Interp() {}

std::deque<OvValue> Interp::root_stack_;
Interp::Env* Interp::active_envs_head_ = nullptr;

Interp::Env::Env(std::shared_ptr<Env> p) : parent(std::move(p)) {
  if (active_envs_head_) active_envs_head_->prev_env = this;
  next_env = active_envs_head_;
  prev_env = nullptr;
  active_envs_head_ = this;
}

Interp::Env::~Env() {
  if (prev_env) prev_env->next_env = next_env;
  else active_envs_head_ = next_env;
  if (next_env) next_env->prev_env = prev_env;
}

Interp::Root::Root(OvValue v) {
  root_stack_.push_back(v);
  slot = &root_stack_.back();
}

Interp::Root::~Root() {
  root_stack_.pop_back();
}

void Interp::gc_scan() {
  for (Env* e = active_envs_head_; e; e = e->next_env) {
    for (const auto& pair : e->vars) {
      ov_gc_mark_value(pair.second);
    }
  }
  for (OvValue v : root_stack_) {
    ov_gc_mark_value(v);
  }
}

OvValue* Interp::open_root() {
  root_stack_.push_back(ov_nil());
  OvValue* slot = &root_stack_.back();
  root_slots_.push_back(slot);
  return slot;
}

void Interp::close_root() {
  if (!root_slots_.empty()) {
    root_slots_.pop_back();
    root_stack_.pop_back();
  }
}

int Interp::run() {
  struct GcGuard {
    GcGuard() { ov_gc_set_scanner(&Interp::gc_scan); }
    ~GcGuard() { ov_gc_set_scanner(nullptr); }
  } gc_guard;

  // hoist function declarations so order in the file does not matter
  for (auto* s : prog_.statements) {
    if (s && s->kind == StmtKind::FuncDecl) {
      FnDef def;
      def.decl = s;
      def.closure = globals_;
      def.name = s->name;
      functions_[s->name] = def;
      globals_->vars[s->name] = ov_nil();  // reserve the name
    }
  }
  // `http`, `json`, `str`, `math` and `time` are namespaces: maps holding their
  // methods as closures. `time` needs no entries -- `time.clock` / `time.now`
  // are recognised by name -- but the entry must exist, or a dotted call would
  // try to evaluate the bare identifier `time` and fail.
  OvMap* http_ns = ov_map_new();
  OvMap* json_ns = ov_map_new();
  OvMap* str_ns = ov_map_new();
  OvMap* math_ns = ov_map_new();
  OvMap* time_ns = ov_map_new();
  globals_->vars["http"] = ov_map(http_ns);
  globals_->vars["json"] = ov_map(json_ns);
  globals_->vars["str"] = ov_map(str_ns);
  globals_->vars["math"] = ov_map(math_ns);
  globals_->vars["time"] = ov_map(time_ns);

  try {
    exec_block(prog_.statements, globals_);
  } catch (Throw& t) {
    OvStr* s = ov_repr(t.value);
    fprintf(stderr, "\novyth: uncaught error: %s\n", ov_str_data(s));
    ov_runtime_shutdown();
    return 70;
  }
  ov_runtime_shutdown();
  return exit_code_;
}

void Interp::exec_block(const StmtList& body, std::shared_ptr<Env> env) {
  for (auto* s : body) {
    exec_stmt(s, env);
    if (signal_.flow != Flow::Normal) return;
  }
}

void Interp::exec_stmt(const Stmt* s, std::shared_ptr<Env> env) {
  if (!s) return;
  switch (s->kind) {
    case StmtKind::VarDecl: {
      size_t n = s->names.size();
      std::vector<Root> tmp;
      tmp.reserve(n);
      for (size_t i = 0; i < n; i++) {
        tmp.emplace_back((i < s->values.size() && s->values[i]) ? eval(s->values[i], env) : ov_nil());
      }
      // unpack: `a, b = [1, 2]` puts a single iterable on the right.
      if (n > 1 && s->values.size() == 1 && ov_tagof(tmp[0].get()) == OV_LIST) {
        OvList* l = tmp[0].get().list;
        for (size_t i = 0; i < n; i++) {
          tmp[i].set(i < l->len ? ov_list_get(l, (int64_t)i) : ov_nil());
        }
      }
      for (size_t i = 0; i < n; i++) {
        OvValue* slot = env ? env->find(s->names[i]) : nullptr;
        if (slot) *slot = tmp[i].get();
        else if (env) env->vars[s->names[i]] = tmp[i].get();
      }
      return;
    }

    case StmtKind::ExprStmt:
      eval(s->expr, env);
      return;

    case StmtKind::Assign: {
      OvValue v = eval(s->expr2, env);
      if (s->op == Tok::ASSIGN) {
        assign_to(s->expr, v, env);
        return;
      }
      // compound:  x += e   =>   x = x + e
      OvValue cur = eval(s->expr, env);
      OvValue out = ov_nil();
      switch (s->op) {
        case Tok::PLUS_EQUAL:    out = ov_add(cur, v); break;
        case Tok::MINUS_EQUAL:   out = ov_sub(cur, v); break;
        case Tok::STAR_EQUAL:    out = ov_mul(cur, v); break;
        case Tok::SLASH_EQUAL:   out = ov_div(cur, v); break;
        case Tok::PERCENT_EQUAL: out = ov_mod(cur, v); break;
        default: break;
      }
      assign_to(s->expr, out, env);
      return;
    }

    case StmtKind::FuncDecl: {
      FnDef def;
      def.decl = s;
      def.closure = env;
      def.name = s->name;
      functions_[s->name] = def;
      return;  // hoisted; the name already exists
    }

    case StmtKind::Return:
      signal_.flow = Flow::Return;
      signal_.value = s->expr ? eval(s->expr, env) : ov_nil();
      return;

    case StmtKind::If: {
      if (ov_truthy(eval(s->expr, env))) {
        auto inner = std::make_shared<Env>(env);
        exec_block(s->body, inner);
      } else if (!s->else_body.empty()) {
        auto inner = std::make_shared<Env>(env);
        exec_block(s->else_body, inner);
      }
      return;
    }

    case StmtKind::While: {
      for (;;) {
        if (!ov_truthy(eval(s->expr, env))) break;
        auto inner = std::make_shared<Env>(env);
        exec_block(s->body, inner);
        if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
        if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
        if (signal_.flow != Flow::Normal) return;
      }
      return;
    }

    case StmtKind::For: {
      Root it(eval(s->expr, env));
      switch (ov_tagof(it.get())) {
        case OV_LIST: {
          OvList* l = it.get().list;
          for (uint32_t i = 0; i < l->len; i++) {
            auto inner = std::make_shared<Env>(env);
            if (s->iter_vars.size() == 1) {
              inner->vars[s->iter_vars[0]] = ov_list_get(l, i);
            } else {
              OvValue pair = ov_list_get(l, i);
              for (size_t k = 0; k < s->iter_vars.size(); k++)
                inner->vars[s->iter_vars[k]] =
                    ov_list_get(pair.list, (int64_t)k);
            }
            exec_block(s->body, inner);
            if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
            if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
            if (signal_.flow != Flow::Normal) return;
          }
          return;
        }
        case OV_STRING: {
          Root chars(ov_str_chars(it.get().str));
          for (uint32_t i = 0; i < chars.get().list->len; i++) {
            auto inner = std::make_shared<Env>(env);
            inner->vars[s->iter_vars[0]] = chars.get().list->items[i];
            exec_block(s->body, inner);
            if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
            if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
            if (signal_.flow != Flow::Normal) return;
          }
          return;
        }
        case OV_MAP: {
          Root keys(ov_map_keys(it.get().map));
          for (uint32_t i = 0; i < keys.get().list->len; i++) {
            auto inner = std::make_shared<Env>(env);
            inner->vars[s->iter_vars[0]] = keys.get().list->items[i];
            exec_block(s->body, inner);
            if (signal_.flow == Flow::Break) { signal_ = Signal{}; break; }
            if (signal_.flow == Flow::Continue) { signal_ = Signal{}; continue; }
            if (signal_.flow != Flow::Normal) return;
          }
          return;
        }
        default:
          throw Throw{ov_s(std::string("cannot loop over ") + ov_type_name(it.get()))};
      }
    }

    case StmtKind::Block: {
      auto inner = std::make_shared<Env>(env);
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
      jmp_buf* target = ov_try_push();
      if (setjmp(*target) == 0) {
        auto inner = std::make_shared<Env>(env);
        bool user_throw = false;
        OvValue user_val = ov_nil();
        try {
          exec_block(s->body, inner);
        } catch (Throw& t) {
          user_throw = true;
          user_val = t.value;
        }
        ov_try_pop();
        if (!user_throw) {
          if (signal_.flow == Flow::Exit || signal_.flow == Flow::Return) return;
          return;  // try body completed normally
        }
        // user throw: keep the value reachable while the catch block runs
        Root slot(user_val);
        if (!s->catch_var.empty()) inner->vars[s->catch_var] = user_val;
        exec_block(s->else_body, inner);
      } else {
        // setjmp returned non-zero: a C-level raise() longjmp'd here.
        ov_try_pop();
        auto inner = std::make_shared<Env>(env);
        if (!s->catch_var.empty()) inner->vars[s->catch_var] = ov_caught;
        exec_block(s->else_body, inner);
      }
      return;
    }

    case StmtKind::Throw: {
      OvValue v = s->expr ? eval(s->expr, env) : ov_str_val("thrown");
      Throw t;
      t.value = v;
      throw t;
    }

    case StmtKind::Debug:
      fprintf(stderr, "[ovyth] %s:%d\n", file_.c_str(), s->pos.line);
      return;
  }
}

void Interp::assign_to(Expr* target, OvValue value, std::shared_ptr<Env> env) {
  Root val_root(value);
  switch (target->kind) {
    case ExprKind::Identifier: {
      OvValue* slot = env ? env->find(target->name) : nullptr;
      if (slot) *slot = val_root.get();
      else if (env) env->vars[target->name] = val_root.get();
      return;
    }
    case ExprKind::Index: {
      Root base(eval(target->a, env));
      OvValue idx = eval(target->b, env);
      if (ov_tagof(base.get()) == OV_LIST) {
        ov_list_set(base.get().list, idx.i, val_root.get());
        return;
      }
      if (ov_tagof(base.get()) == OV_MAP) {
        ov_map_set(base.get().map, idx, val_root.get());
        return;
      }
      throw Throw{ov_s(std::string("cannot index-assign into ") + ov_type_name(base.get()))};
    }
    case ExprKind::Member: {
      Root base(eval(target->a, env));
      if (ov_tagof(base.get()) != OV_MAP)
        throw Throw{ov_s(std::string("cannot set field '") + target->name + "' on " + ov_type_name(base.get()))};
      ov_map_set(base.get().map, ov_s(target->name), val_root.get());
      return;
    }
    default:
      throw Throw{ov_s("invalid assignment target")};
  }
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------
OvValue Interp::interpolate(const Expr* e, std::shared_ptr<Env> env) {
  size_t n = e->items.size();
  size_t m = e->parts.size();
  Root acc(ov_str(ov_str_new("", 0)));
  size_t li = 0;
  for (size_t i = 0; i < m; i++) {
    if (li < n) {
      const std::string& t = e->items[li++]->sval;
      acc.set(ov_str(ov_str_concat(acc.get().str, ov_str_new(t.data(), t.size()))));
    }
    OvValue v = eval(e->parts[i], env);
    acc.set(ov_str(ov_str_concat(acc.get().str, ov_render(v))));
  }
  for (; li < n; li++) {
    const std::string& t = e->items[li]->sval;
    acc.set(ov_str(ov_str_concat(acc.get().str, ov_str_new(t.data(), t.size()))));
  }
  return acc.get();
}

OvValue Interp::index_get(OvValue base, OvValue idx) {
  switch (ov_tagof(base)) {
    case OV_LIST:
      if (ov_tagof(idx) == OV_LIST || ov_tagof(idx) == OV_MAP)
        throw Throw{ov_s("list index must be an Int")};
      return ov_list_get(base.list, ov_tagof(idx) == OV_FLOAT ? (int64_t)idx.f : idx.i);
    case OV_MAP:
      return ov_map_get(base.map, idx);
    case OV_STRING:
      if (ov_tagof(idx) == OV_STRING) {
        int64_t at = ov_str_find(base.str, idx.str, 0);
        return at < 0 ? ov_nil() : ov_int(at);
      }
      return ov_str(ov_str_slice(base.str, idx.i, idx.i + 1));
    default:
      throw Throw{ov_s(std::string("cannot index a ") + ov_type_name(base))};
  }
}

OvValue Interp::member_get(OvValue base, const std::string& name, const Expr* site) {
  (void)site;
  switch (ov_tagof(base)) {
    case OV_MAP:
      return ov_map_get(base.map, ov_s(name));
    case OV_STRING: {
      if (name == "length") return ov_int(base.str->len);
      auto s1 = ov_str(base.str);
      OvStr* r = nullptr;
      if (name == "upper") r = ov_str_upper(s1.str);
      else if (name == "lower") r = ov_str_lower(s1.str);
      else if (name == "trim") r = ov_str_trim(s1.str);
      else if (name == "chars") return ov_str_chars(s1.str);
      else if (name == "bytes") return ov_str_bytes(s1.str);
      else if (name == "split") return ov_str_split(s1.str, ov_str_new(",", 1));
      else throw Throw{ov_s(std::string("String has no field '") + name + "'")};
      return ov_str(r);
    }
    case OV_LIST:
      if (name == "length") return ov_int(base.list->len);
      throw Throw{ov_s(std::string("List has no field '") + name + "'")};
    case OV_NIL:
      throw Throw{ov_s(std::string("cannot read field '") + name + "' of nil")};
    default:
      throw Throw{ov_s(std::string("cannot read field '") + name + "' from " + ov_type_name(base))};
  }
}

}  // namespace ov
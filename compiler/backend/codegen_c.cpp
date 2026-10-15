//
// Lowering the AST to C.
//
// The driver compiles the emitted C with clang and links it against
// libovrt.a, so the result is an ordinary native executable: no interpreter,
// no VM, no runtime dependency beyond libcurl/OpenSSL/zlib. The C compiler on
// the box *is* the LLVM toolchain, so `-O3 -flto` and
// `-ffunction-sections -fdata-sections` in the perf phase apply to generated
// code exactly as they would to hand-written C.
//
// Every Ovyth value is one `OvValue` (tag + unboxed payload), so an expression
// lowers to a single C expression of type OvValue. Constructs needing
// temporaries (list literals, calls, comparisons, interpolation) use Clang's
// statement-expression extension `({ ...; value; })`, which keeps the emitter
// small without changing the runtime ABI.
//
// Control flow maps 1:1 onto C. `return` sets a per-function `_ret` and jumps
// to that function's end label, so a return inside a `try` block or an `if`
// chain still exits the function -- C's own `return` would be legal here too,
// but routing through one label keeps `break`/`continue` inside `try`
// unambiguous and makes the return-label bookkeeping live in exactly one
// place.
#include "codegen_c.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov {
namespace {

using namespace ast;

std::string quote_c(const std::string& s) {
  std::string r = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':  r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n";  break;
      case '\t': r += "\\t";  break;
      case '\r': r += "\\r";  break;
      default:
        // Octal is always exactly three digits, so it cannot swallow a
        // following character the way \xNN can.
        if (c < 0x20 || c >= 0x7f) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\%03o", c);
          r += b;
        } else {
          r += (char)c;
        }
    }
  }
  return r + "\"";
}

// C identifiers for Ovyth names, and C function names for Ovyth functions.
std::string ident(const std::string& n, int uniq) {
  return "v_" + n + "_" + std::to_string(uniq);
}
std::string funcname(const std::string& n) { return "ov_fn_" + n; }

// A loop's label pair. `break`/`continue` lower to `goto` at these, so they
// also work from inside a `try` block (where a bare C break would bind to
// the wrong construct).
struct LoopLabels {
  std::string brk, cont;
};

// Tracks a `acc = acc + expr` string accumulation so the emitted code can use
// a growable builder instead of rebuilding the whole string each iteration
// (spec section 12). Deliberately narrow: it only fires for that shape.
struct BuilderInfo {
  bool active = false;
  std::string var;    // Ovyth name of the accumulator
  std::string cvar;   // generated C variable for the builder
};


// Does `body` accumulate into a string? Recognises the exact shape the builder
// is safe for: one variable initialised to "" and then only appended to.
// Recognise a string accumulator that is only ever appended to. `pre` is the
// name already known to hold "" before the loop (may be empty); `body` is the
// loop body. Returns the accumulator name when the pattern is safe to lower to
// a builder, i.e. there is exactly one candidate and the body contains no other
// assignment to it.
bool accumulates_string(const std::string& pre, const StmtList& body,
                        std::string* name) {
  // Pass 1: seed the candidates. Only a variable initialised to "" (either
  // before the loop or inside it) can be a string accumulator -- that is what
  // distinguishes `acc` from a loop counter that also happens to be `i = i + 1`.
  std::set<std::string> candidates;
  if (!pre.empty()) candidates.insert(pre);
  for (const Stmt* s : body) {
    if (!s) continue;
    if (s->kind == StmtKind::VarDecl && s->names.size() == 1 && s->values.size() == 1 &&
        s->values[0] && s->values[0]->kind == ExprKind::StringLit &&
        s->values[0]->sval.empty())
      candidates.insert(s->names[0]);
  }
  if (candidates.empty()) return false;

  // Pass 2: an append to one of those names, and nothing but appends.
  bool appends = false;
  for (const Stmt* s : body) {
    if (!s) continue;
    // `x = e` is a VarDecl when x is not already bound, and an Assign when it
    // is; both spellings mean the same thing for this analysis.
    std::string name;
    const Expr* rhs = nullptr;
    Tok op = Tok::ASSIGN;
    if (s->kind == StmtKind::Assign && s->expr &&
        s->expr->kind == ExprKind::Identifier) {
      name = s->expr->name;
      rhs = s->expr2;
      op = s->op;
    } else if (s->kind == StmtKind::VarDecl && s->names.size() == 1 &&
               s->values.size() == 1) {
      name = s->names[0];
      rhs = s->values[0];
      op = s->op;
    } else {
      continue;
    }
    if (!candidates.count(name)) continue;
    bool is_append =
        op == Tok::PLUS_EQUAL ||
        (op == Tok::ASSIGN && rhs && rhs->kind == ExprKind::Binary &&
         rhs->op == Tok::PLUS && rhs->a && rhs->a->kind == ExprKind::Identifier &&
         rhs->a->name == name);
    if (is_append) appends = true;
  }

  if (candidates.size() != 1 || !appends) return false;
  *name = *candidates.begin();
  return true;
}


// Does `body` use `continue` at this loop level? Nested loops get their own
// label, so a `continue` inside one does not count for the outer loop.
// `If` must be walked on both sides: a `continue` that appears only in the
// `else` branch still emits a `goto` to the label.
bool body_uses_continue(const StmtList& body) {
  for (const Stmt* s : body) {
    if (!s) continue;
    if (s->kind == StmtKind::Continue) return true;
    if (s->kind == StmtKind::While || s->kind == StmtKind::For) continue;
    if (s->kind == StmtKind::If && (body_uses_continue(s->body) ||
                                    body_uses_continue(s->else_body)))
      return true;
    if (s->kind == StmtKind::Block && body_uses_continue(s->body)) return true;
    if (s->kind == StmtKind::Try) {
      if (body_uses_continue(s->body)) return true;
      if (body_uses_continue(s->else_body)) return true;
    }
  }
  return false;
}

// The same walk for `break`. emit_for only jumps to its break label from an
// explicit `break`, so a `for` body without one would emit a label C compilers
// warn about as unused. Nested loops are skipped for the same reason as above:
// their `break` belongs to them.
bool body_uses_break(const StmtList& body) {
  for (const Stmt* s : body) {
    if (!s) continue;
    if (s->kind == StmtKind::Break) return true;
    if (s->kind == StmtKind::While || s->kind == StmtKind::For) continue;
    if (s->kind == StmtKind::If &&
        (body_uses_break(s->body) || body_uses_break(s->else_body)))
      return true;
    if (s->kind == StmtKind::Block && body_uses_break(s->body)) return true;
    if (s->kind == StmtKind::Try) {
      if (body_uses_break(s->body)) return true;
      if (body_uses_break(s->else_body)) return true;
    }
  }
  return false;
}

// Scan context for the boundary planner: which function's body is being
// scanned, and which function's claims the scan is collecting. `for_call_arg`
// marks the one place an own parameter may be assumed int -- the argument of
// a call to the function being planned itself, where the assumption is the
// signature under test. Return expressions and other functions' arguments
// must use the computed claim: a return of a string parameter is a return
// of a string, whatever the call sites pass.
struct ScanCtx {
  const std::string* enclosing = nullptr;  // function whose body this is
  const std::string* planning = nullptr;   // whose claims are being computed
  bool for_call_arg = false;               // scanning a self-call's argument
};

class Gen {
 public:
  std::string out;
  std::string err;

  // Set by the driver from `--stats`: emit the heap-telemetry call in main()'s
  // epilogue. Off by default, so a program never prints counters it was not
  // asked for -- a benchmark that instruments itself measures the wrong thing.
  bool stats_ = false;

  Gen(const ast::Program& p, Sema& s) : prog_(p), sema_(s) {}

  std::string run();

 private:
  const ast::Program& prog_;
  Sema& sema_;

  int indent_ = 0;
  int tmp_ = 0;
  int try_depth_ = 0;
  std::string cur_fn_;
  std::string ret_label_;
  bool fn_has_return_ = false;
  std::vector<LoopLabels> loops_;
  std::vector<std::unordered_map<std::string, std::string>> scopes_;
  std::vector<std::string> anon_defs_;  // closure bodies to emit at module scope
  std::vector<std::string> anon_names_;  // their C names, in creation order
  std::vector<std::string> pending_fns_; // user function bodies
  std::vector<std::string> cur_roots_;  // live local slots in the current function

  // Locals holding an Int64Array because sema proved the list is built only
  // from ints. A push into one is a raw int64 store: no box, 8 bytes per
  // element instead of 16, and nothing for the collector to mark. They are
  // ordinary GC objects, so one that escapes its function (it is returned, or
  // stored into a structure) stays reachable -- the collector owns them now,
  // which is why nothing here frees them.
  std::unordered_set<std::string> int_coll_;
  BuilderInfo sb_;                     // active string-builder accumulation
  std::string last_empty_str_;         // last `x = ""` emitted (builder hint)
  std::set<std::string> inline_roots_; // roots already declared at their use site
  int loop_depth_ = 0;                 // nesting, for builder ownership
  // Closure boxes: names currently shared through a 1-element box list.
  // box_of_[name] is the box OvValue expression in the enclosing scope;
  // box_writes_ marks names whose assignments must store through the box.
  // Saved/restored around closure bodies (which get their own box set).
  std::unordered_map<std::string, std::string> box_of_;
  std::set<std::string> box_writes_;

  // Proven numeric types: names that are proven Int/Float and can use raw registers
  std::unordered_set<std::string> proven_ints_;
  std::unordered_set<std::string> proven_floats_;

  // Check if a Ovyth variable name is proven int/float from sema in any scope
  //
  // The sema fallback is keyed by the function being emitted as well as the
  // name, so one function's int counter cannot make another function's string
  // of the same name an int64_t.
  bool is_proven_int_any_scope(const std::string& name) const {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) {
        return proven_ints_.count(it->second) > 0;
      }
    }
    // Not in current codegen scopes, check sema
    auto it = sema_.proven_types.find(sema_fn_key(name));
    return it != sema_.proven_types.end() && it->second.is_proven_int;
  }
  bool is_proven_float_any_scope(const std::string& name) const {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) {
        return proven_floats_.count(it->second) > 0;
      }
    }
    auto it = sema_.proven_types.find(sema_fn_key(name));
    return it != sema_.proven_types.end() && it->second.is_proven_float;
  }

  // The key sema recorded this name under: "fn/name" inside a function, the
  // bare name for a global. cur_fn_ is the function currently being emitted,
  // matching the one sema was checking.
  std::string sema_fn_key(const std::string& name) const {
    return cur_fn_.empty() ? name : cur_fn_ + "/" + name;
  }

  // Settle which unboxed form a declaration may use.
  //
  // sema's proven-type map is keyed by Ovyth name alone, with no function to
  // disambiguate, so the same name can be proven int in one function and
  // float in another. When a declaration's own initializer disagrees with the
  // claim, the initializer wins -- declaring `v = 0.0` as int64_t and reading
  // `.i` off the float union is how a float local became a raw bit pattern.
  //
  // The int test has to come first: is_float_expr() is deliberately
  // permissive (an int literal is a legal float operand), so testing it first
  // would turn every `i = 0` into a double.
  void resolve_unbox(const Expr* init, bool& pi, bool& pf) {
    if (pi && pf) {  // sema claims both: no unboxed form is safe
      pi = pf = false;
      return;
    }
    if (!init) return;
    if (pf && is_int_expr(init)) {
      pf = false;
      pi = true;
      return;
    }
    if (pi && is_float_expr(init) && !is_int_expr(init)) {
      pi = false;
      pf = true;
    }
  }

  // Seed the proven sets from sema.
  //
  // Only globals: a local's entry is keyed by its function, and seeding it here
  // under a bare C name would re-create the cross-function leak this keying
  // exists to prevent. Locals are resolved while their function is emitted.
  void init_proven_types() {
    for (const auto& kv : sema_.proven_types) {
      if (kv.first.find('/') != std::string::npos) continue;  // a function local
      const std::string& name = kv.first;
      const TyInfo& info = kv.second;
      std::string cname = ident(name, 0);  // base name, uniq=0 for globals/params
      if (info.is_proven_int) { proven_ints_.insert(cname); }
      if (info.is_proven_float) proven_floats_.insert(cname);
    }
  }

  // ---------------------------------------------------------------- output
  void line(const std::string& s) {
    out.append(indent_ * 2, ' ');
    out += s;
    out += '\n';
  }
  void nl() { out += '\n'; }
  std::string fresh() { return "_t" + std::to_string(tmp_++); }
  std::string lbl(const char* p) { return p + std::to_string(tmp_++); }

  // ---------------------------------------------------------------- scopes
  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() { scopes_.pop_back(); }
  std::string* lookup(const std::string& n) {
    for (size_t i = scopes_.size(); i-- > 0;) {
      auto it = scopes_[i].find(n);
      if (it != scopes_[i].end()) return &it->second;
    }
    return nullptr;
  }
  const std::string* lookup(const std::string& n) const {
    for (size_t i = scopes_.size(); i-- > 0;) {
      auto it = scopes_[i].find(n);
      if (it != scopes_[i].end()) return &it->second;
    }
    return nullptr;
  }
  // Bind a name to a C variable. `rhs` may be empty for an uninitialised
  // declaration. Returns the assignment statement.
  // Assign `name = rhs`. Because every function declares its locals up front
  // (and registers them as GC roots there), this always emits an assignment --
  // never a declaration.
  // For proven Int/Float, we use raw C types instead of OvValue and skip GC registration.
  std::string bind(const std::string& name, const std::string& rhs, bool is_proven_int = false, bool is_proven_float = false) {
    if (box_writes_.count(name) && box_of_.count(name)) {
      return "ov_list_set((" + box_of_[name] + ").list, 0, " + rhs + ")";
    }
    if (is_proven_int || is_proven_float) {
      // Use raw C type, no GC registration needed
      if (std::string* c = lookup(name)) return *c + " = " + rhs;
      std::string c = ident(name, tmp_++);
      scopes_.back()[name] = c;
      if (is_proven_int) {
        proven_ints_.insert(c);   // track by C name
        cur_roots_.push_back("@int:" + c);  // sentinel: raw int64_t
        return "int64_t " + c + " = " + rhs;
      } else {
        proven_floats_.insert(c);
        cur_roots_.push_back("@float:" + c);  // sentinel: raw double
        return "double " + c + " = " + rhs;
      }
    }
    // Regular OvValue - needs GC registration
    if (std::string* c = lookup(name)) return *c + " = " + rhs;
    std::string c = ident(name, tmp_++);
    scopes_.back()[name] = c;
    cur_roots_.push_back(c);   // GC must see this slot
    return c + " = " + rhs;
  }


  // Emit the entry prologue: one declaration per local (so the body's
  // assignments below have somewhere to land) plus root registration for each.
  //
  // This has to happen at function entry rather than at the point of
  // declaration because the collector runs on *any* allocation: a local that
  // is live but not yet registered would be swept the moment the heap crosses
  // its threshold. Two passes make that safe -- the body is lowered first (to
  // learn the locals), then the prologue, then the body.
  void emit_root_prologue() {
    if (cur_roots_.empty()) return;
    std::string decls, regs;
    bool first = true;
    for (size_t i = 0; i < cur_roots_.size(); i++) {
      const std::string& slot = cur_roots_[i];
      // Sentinels emitted inline by bind() for proven int/float vars.
      // "@int:cvar" means we already emitted `int64_t cvar = ...;` at the
      // binding site; nothing more needed here.
      if (slot.size() > 5 && slot.substr(0, 5) == "@int:") continue;
      if (slot.size() > 7 && slot.substr(0, 7) == "@float:") continue;
      // Roots declared at their use site (the string-builder result) are
      // already emitted; redeclaring them would shadow the live slot.
      if (inline_roots_.count(slot)) continue;
      if (!first) { decls += " "; regs += " "; }
      first = false;
      decls += "OvValue " + slot + " = ov_nil();";
      regs += "ov_gc_register_root(&" + slot + ");";
    }
    if (!decls.empty()) line(decls);
    if (!regs.empty()) line(regs);
  }

  bool fail(const std::string& m) {
    if (err.empty()) err = m;
    return false;
  }

  // ----------------------------------------------------------- expressions
  std::string ex(const Expr* e);
  std::string emit_interp(const Expr* e);
  std::string emit_slice(const Expr* e);
  std::string emit_listcomp(const Expr* e);
  std::string emit_assign(const Expr* target, const std::string& val);
  std::string emit_call(const Expr* e);
  std::string emit_closure(const Expr* e);

  // Proven-int fast path: returns true when `e` is statically known to be
  // a pure int64_t expression with no side effects that could allocate.
  // ex_int() returns the raw C int64_t expression; never empty when
  // is_int_expr() is true.
  bool is_int_expr(const Expr* e) const;
  std::string ex_int(const Expr* e);  // raw int64_t C expression

  // The C condition text for an `if`/`while` whose test is a comparison of
  // two provably int expressions, or "" when the test does not qualify.
  // The general path materialises an OvValue and switches on its tag on
  // every evaluation; in a tight recursive function that is most of the
  // per-call cost, and in a 20M-iteration loop it is the loop's tax.
  std::string raw_int_condition(const Expr* e);

  // A raw int64 expression for a value that is not provably an int: coerces
  // through the runtime's cast, which throws for a non-number. Only a guard --
  // sema retires any collection whose push is not provably int, so a reachable
  // non-int push would be a bug in the analysis, not in this expression.
  std::string ex_int_of(const Expr* e) {
    if (is_int_expr(e)) return ex_int(e);
    std::string v = fresh();
    return "({ OvValue " + v + " = " + ex(e) + "; int _threw" + v +
           " = 0; OvValue _n" + v + " = ov_h_to_int(" + v + ", &_threw" + v +
           "); if (_threw" + v + ") ov_throw_value(ov_h_err_value()); _n" + v +
           ".i; })";
  }

  // Proven-float fast path: same for double expressions.
  bool is_float_expr(const Expr* e) const;
  std::string ex_float(const Expr* e);  // raw double C expression

  // ------------------------------------------------------------ statements
  void stmt(const Stmt* s);
  bool try_sb_append(const std::string& name, const Expr* rhs, Tok op);
  void block(const StmtList& body, bool own_scope);
  void emit_for(const Stmt* s);

  // --------------------------------------------------------------- functions
  void collect_functions(const StmtList& body);
  void emit_function(const Stmt* s);
  std::string call_value_method(const Expr* e, const std::string& base);
  std::string call_value_method_closure(const std::string& name, const Expr* e);
  std::string call_global(const std::string& q, const Expr* e);
  std::string call_namespace(const std::string& ns, const std::string& name,
                             const Expr* e);
  void plan_function_boundaries();
  // The `try` blocks currently open, innermost last. Every path that leaves
  // one other than its own end -- return, break, continue -- must run its
  // cleanup; the exit statements ask for the list here.
  std::vector<std::pair<std::string, std::string>> try_cleanups_;
  void emit_try_cleanups();
  // The C signature for a user function: raw parameters and a raw return
  // where the planner proved them. Forward declarations and definitions
  // both come from here.
  std::string fn_signature(const Stmt* s) const;
  // A direct call to a top-level user function whose return value crosses the
  // boundary as a raw int64_t (see plan_function_boundaries). False when the
  // callee is shadowed by a local, because then the call goes through a value.
  bool call_returns_raw_int(const Expr* e) const;
  bool fn_ret_is_raw(const std::string& fn) const;
  bool fn_param_is_raw(const std::string& fn, size_t idx) const;
  // The argument for parameter `idx` as a raw int64_t expression, or a hard
  // error when the plan said raw but this site is not provably int.
  std::string raw_arg_or_fail(const Expr* a, const std::string& fn, size_t idx);
  // The planner's statically-int test; ScanCtx is defined with the planner.
  bool pre_int(const Expr* e, const ScanCtx& ctx) const;

  // A hoisted user function, plus whether it is variadic.
  std::unordered_map<std::string, const Stmt*> userfns_;

  // Callee names bound anywhere as a local: a call to one of those is a
  // value call and must not constrain (or trust) a signature.
  std::set<std::string> bound_names_;

  // Which user functions cross their call boundary as raw int64_t: per
  // parameter (arguments) and as a whole (the return value).
  struct FnPlan {
    bool raw_ret = false;
    std::vector<bool> raw_param;
  };
  std::unordered_map<std::string, FnPlan> fn_plans_;
};

// ----------------------------------------------------------- const folding
// Fold literal arithmetic at emit time (performance spec section 28). After
// the ovrt.h fast paths, clang folds most of this again at -O3, so this is
// what makes -O0/debug builds and constant subexpressions free too.
// Deliberately NOT folded: division/modulo by literal zero and int edges
// (the runtime must still raise its usual error), and `**` (ov_pow always
// returns Float -- folding it to an Int would change observable types).
static std::string fold_int(int64_t v) {
  return "ov_int(" + std::to_string(v) + "LL)";
}
static std::string fold_float(double v) {
  std::ostringstream o;
  o.precision(17);
  o << v;
  std::string s = o.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
    s += ".0";
  return "ov_float(" + s + ")";
}

static bool fold_const_binop(const Expr* e, std::string& out) {
  const Expr* x = e->a;
  const Expr* y = e->b;
  if (!x || !y) return false;
  const bool xi = x->kind == ExprKind::IntLit, xf = x->kind == ExprKind::FloatLit;
  const bool yi = y->kind == ExprKind::IntLit, yf = y->kind == ExprKind::FloatLit;
  if (!(xi || xf) || !(yi || yf)) return false;
  const bool allint = xi && yi;
  auto emit_bool = [&](bool v) { out = v ? "ov_bool(1)" : "ov_bool(0)"; return true; };

  // Comparisons: same semantics as ov_eq / ov_cmp for every literal numeric
  // combination (mixed int/float promotes to double, exactly as the runtime).
  switch (e->op) {
    case Tok::EQUAL:
      if (allint) return emit_bool(x->ival == y->ival);
      if (xi && yf) return emit_bool((double)x->ival == y->fval);
      if (xf && yi) return emit_bool(x->fval == (double)y->ival);
      if (xf && yf) return emit_bool(x->fval == y->fval);
      return false;
    case Tok::BANG_EQUAL:
      if (allint) return emit_bool(x->ival != y->ival);
      if (xi && yf) return emit_bool((double)x->ival != y->fval);
      if (xf && yi) return emit_bool(x->fval != (double)y->ival);
      if (xf && yf) return emit_bool(x->fval != y->fval);
      return false;
    case Tok::LESS: case Tok::LESS_EQUAL:
    case Tok::GREATER: case Tok::GREATER_EQUAL: {
      bool r;
      if (allint) {
        int64_t a = x->ival, b = y->ival;
        r = (e->op == Tok::LESS)        ? a <  b
          : (e->op == Tok::LESS_EQUAL)  ? a <= b
          : (e->op == Tok::GREATER)     ? a >  b
          :                               a >= b;
      } else {
        double a = xi ? (double)x->ival : x->fval;
        double b = yi ? (double)y->ival : y->fval;
        r = (e->op == Tok::LESS)        ? a <  b
          : (e->op == Tok::LESS_EQUAL)  ? a <= b
          : (e->op == Tok::GREATER)     ? a >  b
          :                               a >= b;
      }
      return emit_bool(r);
    }
    default: break;
  }

  if (allint) {
    int64_t p = 0;
    // negative / -1 is skipped wholesale: INT64_MIN / -1 is UB in the C
    // compiler doing the folding. The runtime handles it as it always has.
    switch (e->op) {
      case Tok::PLUS:  p = (int64_t)((uint64_t)x->ival + (uint64_t)y->ival); break;
      case Tok::MINUS: p = (int64_t)((uint64_t)x->ival - (uint64_t)y->ival); break;
      case Tok::STAR:  p = (int64_t)((uint64_t)x->ival * (uint64_t)y->ival); break;
      case Tok::SLASH:
        if (y->ival == 0) return false;
        if (y->ival == -1 && x->ival < 0) return false;
        p = x->ival / y->ival; break;
      case Tok::PERCENT:
        if (y->ival == 0) return false;
        if (y->ival == -1 && x->ival < 0) return false;
        p = x->ival % y->ival; break;
      case Tok::AMP:   p = x->ival & y->ival; break;
      case Tok::PIPE:  p = x->ival | y->ival; break;
      case Tok::CARET: p = x->ival ^ y->ival; break;
      case Tok::SHL:
        if (y->ival < 0 || y->ival > 63) return false;
        p = (int64_t)((uint64_t)x->ival << (unsigned long long)y->ival); break;
      case Tok::SHR:
        if (y->ival < 0 || y->ival > 63) return false;
        p = x->ival >> y->ival; break;
      default: return false;
    }
    out = fold_int(p);
    return true;
  }

  double a = xi ? (double)x->ival : x->fval;
  double b = yi ? (double)y->ival : y->fval;
  switch (e->op) {
    case Tok::PLUS:  out = fold_float(a + b); return true;
    case Tok::MINUS: out = fold_float(a - b); return true;
    case Tok::STAR:  out = fold_float(a * b); return true;
    case Tok::SLASH:
      if (b == 0.0) return false;   // runtime raises division-by-zero
      out = fold_float(a / b); return true;
    default: return false;
  }
}

static bool fold_const_unary(const Expr* e, std::string& out) {
  const Expr* x = e->a;
  if (!x) return false;
  switch (e->op) {
    case Tok::MINUS:
      if (x->kind == ExprKind::IntLit) {
        out = fold_int((int64_t)(0u - (uint64_t)x->ival));
        return true;
      }
      if (x->kind == ExprKind::FloatLit) { out = fold_float(-x->fval); return true; }
      return false;
    case Tok::TILDE:
      if (x->kind == ExprKind::IntLit) { out = fold_int(~x->ival); return true; }
      return false;
    case Tok::KW_NOT:
      if (x->kind == ExprKind::BoolLit) { out = x->bval ? "ov_bool(0)" : "ov_bool(1)"; return true; }
      return false;
    default: return false;
  }
}

// ---------------------------------------------------------------- proven-int helpers
std::string Gen::raw_int_condition(const Expr* e) {
  if (!e || e->kind != ExprKind::Binary) return "";
  if (!is_int_expr(e->a) || !is_int_expr(e->b)) return "";
  const std::string a = ex_int(e->a), b = ex_int(e->b);
  switch (e->op) {
    case Tok::EQUAL:         return "(" + a + ") == (" + b + ")";
    case Tok::BANG_EQUAL:    return "(" + a + ") != (" + b + ")";
    case Tok::LESS:          return "(" + a + ") < (" + b + ")";
    case Tok::GREATER:       return "(" + a + ") > (" + b + ")";
    case Tok::LESS_EQUAL:    return "(" + a + ") <= (" + b + ")";
    case Tok::GREATER_EQUAL: return "(" + a + ") >= (" + b + ")";
    default:                 return "";
  }
}

// Returns true when `e` is provably a pure int64_t expression:
//   - IntLit
//   - Identifier whose C name is in proven_ints_
//   - Binary(+,-,*,%,&,|,^,<<,>>) of two such expressions
// Does NOT include SLASH (might be int/int but we still want ov_div's
// zero-check semantics in the general case; fold_const_binop handles literals).
bool Gen::is_int_expr(const Expr* e) const {
  if (!e) return false;
  if (e->kind == ExprKind::IntLit) return true;
  // A call whose function the planner proved returns a raw int. Its
  // arguments are verified at the boundary (the planner checked every call
  // site), so the call is an int expression wherever its result is used.
  if (e->kind == ExprKind::Call) return call_returns_raw_int(e);
  if (e->kind == ExprKind::Identifier) {
    // Look up the C variable name and check if it's proven int.
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(e->name);
      if (it != scopes_[i].end()) {
        return proven_ints_.count(it->second) > 0;
      }
    }
    return false;
  }
  if (e->kind == ExprKind::Unary && e->op == Tok::MINUS)
    return is_int_expr(e->a);
  if (e->kind != ExprKind::Binary) return false;
  // Ops that keep int->int:
  switch (e->op) {    case Tok::PLUS: case Tok::MINUS: case Tok::STAR: case Tok::PERCENT:
    case Tok::AMP: case Tok::PIPE: case Tok::CARET:
    case Tok::SHL: case Tok::SHR:
      return is_int_expr(e->a) && is_int_expr(e->b);
    default: return false;
  }
}

// Returns a raw int64_t C expression. Only call when is_int_expr() is true.
std::string Gen::ex_int(const Expr* e) {
  // A call to a raw-returning function is already an int64_t C expression;
  // no boxing wrapper is wanted on this path.
  if (e->kind == ExprKind::Call) {
    if (call_returns_raw_int(e)) return emit_call(e);
    fail("internal: ex_int called on a call that does not return a raw int");
    return "0LL";
  }
  if (e->kind == ExprKind::IntLit)
    return std::to_string(e->ival) + "LL";
  if (e->kind == ExprKind::Identifier) {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(e->name);
      if (it != scopes_[i].end()) return it->second;
    }
    return "0LL";
  }
  if (e->kind == ExprKind::Unary && e->op == Tok::MINUS)
    return "(-(int64_t)(" + ex_int(e->a) + "))";
  // Binary case:
  std::string ai = ex_int(e->a);
  std::string bi = ex_int(e->b);
  switch (e->op) {
    case Tok::PLUS:    return "(" + ai + ")+(" + bi + ")";
    case Tok::MINUS:   return "(" + ai + ")-(" + bi + ")";
    case Tok::STAR:    return "(" + ai + ")*(" + bi + ")";
    case Tok::PERCENT: return "(" + bi + ")?(" + ai + ")%(" + bi + "):(ov_zero_error(),0LL)";
    case Tok::AMP:     return "(" + ai + ")&(" + bi + ")";
    case Tok::PIPE:    return "(" + ai + ")|(" + bi + ")";
    case Tok::CARET:   return "(" + ai + ")^(" + bi + ")";
    case Tok::SHL:     return "(" + ai + ")<<(" + bi + ")";
    case Tok::SHR:     return "(" + ai + ")>>(" + bi + ")";
    default: return "0LL";
  }
}

// Proven-float fast path helpers
bool Gen::is_float_expr(const Expr* e) const {
  if (!e) return false;
  if (e->kind == ExprKind::FloatLit) return true;
  if (e->kind == ExprKind::IntLit) return true;  // Int promotes to float
  if (e->kind == ExprKind::Identifier) {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(e->name);
      if (it != scopes_[i].end()) {
        return proven_floats_.count(it->second) > 0 || proven_ints_.count(it->second) > 0;
      }
    }
    return false;
  }
  if (e->kind == ExprKind::Unary && e->op == Tok::MINUS)
    return is_float_expr(e->a);
  if (e->kind != ExprKind::Binary) return false;
  switch (e->op) {
    case Tok::PLUS: case Tok::MINUS: case Tok::STAR: case Tok::SLASH:
      return is_float_expr(e->a) && is_float_expr(e->b);
    default: return false;
  }
}

std::string Gen::ex_float(const Expr* e) {
  if (e->kind == ExprKind::FloatLit) {
    std::ostringstream o;
    o.precision(17);
    o << e->fval;
    std::string s = o.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
      s += ".0";
    return s;
  }
  if (e->kind == ExprKind::IntLit)
    return std::to_string(e->ival) + ".0";
  if (e->kind == ExprKind::Identifier) {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(e->name);
      if (it != scopes_[i].end()) return it->second;
    }
    return "0.0";
  }
  if (e->kind == ExprKind::Unary && e->op == Tok::MINUS)
    return "(-(" + ex_float(e->a) + "))";
  // Binary case:
  std::string af = ex_float(e->a);
  std::string bf = ex_float(e->b);
  switch (e->op) {
    case Tok::PLUS:  return "(" + af + ")+(" + bf + ")";
    case Tok::MINUS: return "(" + af + ")-(" + bf + ")";
    case Tok::STAR:  return "(" + af + ")*(" + bf + ")";
    case Tok::SLASH: return "(" + bf + ")?(" + af + ")/(" + bf + "):(ov_zero_error(),0.0)";
    default: return "0.0";
  }
}

// ---------------------------------------------------------------- literals
std::string Gen::ex(const Expr* e) {
  if (!e) return "ov_nil()";

  switch (e->kind) {
    case ExprKind::IntLit:
      return "ov_int(" + std::to_string(e->ival) + "LL)";
    case ExprKind::FloatLit: {
      std::ostringstream o;
      o.precision(17);
      o << e->fval;
      std::string s = o.str();
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
      return "ov_float(" + s + ")";
    }
    case ExprKind::BoolLit:  return e->bval ? "ov_bool(1)" : "ov_bool(0)";
    case ExprKind::NullLit:  return "ov_nil()";
    case ExprKind::StringLit:
      // One pinned allocation per literal site, cached in a block-local
      // static. The old shape calloc'd a fresh OvStr on EVERY evaluation --
      // that was most of strconcat's cost (40k literal allocs per run) and
      // half of mapops' (200k "key" allocs).
      return "({ static OvStr* ov_lit_; if (!ov_lit_) ov_lit_ = ov_str_lit(" +
             quote_c(e->sval) + ", " + std::to_string(e->sval.size()) +
             "); ov_str(ov_lit_); })";

    case ExprKind::Identifier: {
      if (std::string* c = lookup(e->name)) {
        // If this is a proven int/float variable, wrap it in OvValue for general use.
        // The fast paths (is_int_expr/ex_int, is_float_expr/ex_float) bypass this.
        if (proven_ints_.count(*c)) return "ov_int(" + *c + ")";
        if (proven_floats_.count(*c)) return "ov_float(" + *c + ")";
        return *c;
      }
      if (userfns_.count(e->name)) return "ov_nil()";
      fail("unknown name '" + e->name + "'");
      return "ov_nil()";
    }

    case ExprKind::ListLit: {
      // Check if all items have the same proven type (int/float/string)
      bool all_int = true, all_float = true, all_string = true;
      for (const Expr* it : e->items) {
        if (!is_int_expr(it)) all_int = false;
        if (!is_float_expr(it)) all_float = false;
        if (it->kind != ExprKind::StringLit) all_string = false;
      }
      
      if (all_int && !e->items.empty()) {
        // Emit as OvInt64Array
        std::string arr = fresh(), idx = fresh();
        std::string s = "({ OvInt64Array* " + arr + " = ov_i64a_new_cap(" + std::to_string(e->items.size()) + "); ";
        for (size_t i = 0; i < e->items.size(); i++) {
          s += arr + "->data[" + std::to_string(i) + "] = " + ex_int(e->items[i]) + "; ";
        }
        s += arr + "->len = " + std::to_string(e->items.size()) + "; ";
        s += "ov_i64a_val(" + arr + "); })";
        return s;
      }
      if (all_float && !e->items.empty()) {
        // Emit as OvFloat64Array
        std::string arr = fresh(), idx = fresh();
        std::string s = "({ OvFloat64Array* " + arr + " = ov_f64a_new_cap(" + std::to_string(e->items.size()) + "); ";
        for (size_t i = 0; i < e->items.size(); i++) {
          s += arr + "->data[" + std::to_string(i) + "] = " + ex_float(e->items[i]) + "; ";
        }
        s += arr + "->len = " + std::to_string(e->items.size()) + "; ";
        s += "ov_f64a_val(" + arr + "); })";
        return s;
      }
      if (all_string && !e->items.empty()) {
        // Emit as OvStringArray
        std::string arr = fresh(), idx = fresh();
        std::string s = "({ OvStringArray* " + arr + " = ov_stra_new_cap(" + std::to_string(e->items.size()) + "); ";
        for (size_t i = 0; i < e->items.size(); i++) {
          s += arr + "->data[" + std::to_string(i) + "] = " + ex(e->items[i]) + ".str; ";
        }
        s += arr + "->len = " + std::to_string(e->items.size()) + "; ";
        s += "ov_stra_val(" + arr + "); })";
        return s;
      }
      // Fallback to generic OvList. The list is registered as a
      // root while it is being filled: an element expression may
      // allocate (and so trigger a collection), and an unrooted
      // half-built list would be swept out from under the push.
      std::string l = fresh(), v = fresh(), rm = fresh();
      std::string s = "({ size_t " + rm + " = ov_gc_roots_mark(); OvList* " + l +
                      " = ov_list_new(); OvValue " + v + " = ov_list(" + l + "); " +
                      "ov_gc_register_root(&" + v + "); ";
      for (const Expr* it : e->items)
        s += "ov_list_push(" + l + ", " + ex(it) + "); ";
      s += "ov_gc_roots_restore(" + rm + "); ";
      return s + v + "; })";
    }

    case ExprKind::MapLit: {
      // Same rooting as the list literal: values may allocate
      // while the map is being filled.
      std::string m = fresh(), v = fresh(), rm = fresh();
      std::string s = "({ size_t " + rm + " = ov_gc_roots_mark(); OvMap* " + m +
                      " = ov_map_new(); OvValue " + v + " = ov_map(" + m + "); " +
                      "ov_gc_register_root(&" + v + "); ";
      for (const auto& f : e->fields)
        s += "ov_map_set(" + m + ", " + ex(f.first) + ", " + ex(f.second) + "); ";
      s += "ov_gc_roots_restore(" + rm + "); ";
      return s + v + "; })";
    }

    case ExprKind::Interp: return emit_interp(e);

    case ExprKind::Unary: {
      if (std::string cf; fold_const_unary(e, cf)) return cf;
      std::string a = fresh();
      std::string s = "({ OvValue " + a + " = " + ex(e->a) + "; ";
      switch (e->op) {
        case Tok::KW_NOT: s += "ov_bool(!ov_truthy(" + a + "))"; break;
        case Tok::MINUS:   s += "ov_neg(" + a + ")"; break;
        case Tok::PLUS:    s += a; break;
        case Tok::TILDE:   s += "ov_int(~((" + a + ").i))"; break;
        default:
          fail("unsupported unary operator");
          s += a;
      }
      return s + "; })";
    }

    case ExprKind::Binary: {
      if (std::string cf; fold_const_binop(e, cf)) return cf;
      if (e->op == Tok::KW_IN) {
        std::string n = fresh(), h = fresh();
        return "({ OvValue " + n + " = " + ex(e->a) + "; OvValue " + h + " = " +
               ex(e->b) + "; ov_bool(ov_in(" + n + ", " + h + ")); })";
      }
      // Proven-int fast path: both operands are statically Int.
      // Emit raw int64_t arithmetic; no OvValue temporaries, no tag checks.
      if (is_int_expr(e->a) && is_int_expr(e->b)) {
        std::string ai = ex_int(e->a);
        std::string bi = ex_int(e->b);
        switch (e->op) {
          case Tok::PLUS:          return "ov_int((" + ai + ") + (" + bi + "))";
          case Tok::MINUS:         return "ov_int((" + ai + ") - (" + bi + "))";
          case Tok::STAR:          return "ov_int((" + ai + ") * (" + bi + "))";
          case Tok::PERCENT:       return "ov_int((" + bi + ") ? (" + ai + ") % (" + bi + ") : (ov_zero_error(),0LL))";
          case Tok::AMP:           return "ov_int((" + ai + ") & (" + bi + "))";
          case Tok::PIPE:          return "ov_int((" + ai + ") | (" + bi + "))";
          case Tok::CARET:         return "ov_int((" + ai + ") ^ (" + bi + "))";
          case Tok::SHL:           return "ov_int((" + ai + ") << (" + bi + "))";
          case Tok::SHR:           return "ov_int((" + ai + ") >> (" + bi + "))";
          case Tok::EQUAL:         return "ov_bool((" + ai + ") == (" + bi + "))";
          case Tok::BANG_EQUAL:    return "ov_bool((" + ai + ") != (" + bi + "))";
          case Tok::LESS:          return "ov_bool((" + ai + ") < (" + bi + "))";
          case Tok::GREATER:       return "ov_bool((" + ai + ") > (" + bi + "))";
          case Tok::LESS_EQUAL:    return "ov_bool((" + ai + ") <= (" + bi + "))";
          case Tok::GREATER_EQUAL: return "ov_bool((" + ai + ") >= (" + bi + "))";
          // SLASH falls through to OvValue path (div-by-zero handled by ov_div)
          default: break;
        }
      }
      // Proven-float fast path: both operands are statically Float/Int.
      // Emit raw double arithmetic; no OvValue temporaries, no tag checks.
      if (is_float_expr(e->a) && is_float_expr(e->b)) {
        std::string af = ex_float(e->a);
        std::string bf = ex_float(e->b);
        switch (e->op) {
          case Tok::PLUS:       return "ov_float((" + af + ") + (" + bf + "))";
          case Tok::MINUS:      return "ov_float((" + af + ") - (" + bf + "))";
          case Tok::STAR:       return "ov_float((" + af + ") * (" + bf + "))";
          case Tok::SLASH:      return "ov_float((" + bf + ") ? (" + af + ") / (" + bf + ") : (ov_zero_error(),0.0))";
          case Tok::EQUAL:      return "ov_bool((" + af + ") == (" + bf + "))";
          case Tok::BANG_EQUAL: return "ov_bool((" + af + ") != (" + bf + "))";
          case Tok::LESS:       return "ov_bool((" + af + ") < (" + bf + "))";
          case Tok::GREATER:    return "ov_bool((" + af + ") > (" + bf + "))";
          case Tok::LESS_EQUAL: return "ov_bool((" + af + ") <= (" + bf + "))";
          case Tok::GREATER_EQUAL: return "ov_bool((" + af + ") >= (" + bf + "))";
          default: break;
        }
      }
      std::string a = fresh(), b = fresh();
      const std::string pre = "({ OvValue " + a + " = " + ex(e->a) + "; OvValue " +
                              b + " = " + ex(e->b) + "; ";
      const std::string& A = a;
      const std::string& B = b;
      switch (e->op) {
        case Tok::PLUS:          return pre + "ov_add(" + A + ", " + B + "); })";
        case Tok::MINUS:         return pre + "ov_sub(" + A + ", " + B + "); })";
        case Tok::STAR:          return pre + "ov_mul(" + A + ", " + B + "); })";
        case Tok::SLASH:         return pre + "ov_div(" + A + ", " + B + "); })";
        case Tok::PERCENT:       return pre + "ov_mod(" + A + ", " + B + "); })";
        case Tok::DOUBLE_STAR:   return pre + "ov_pow(" + A + ", " + B + "); })";
        case Tok::AMP:           return pre + "ov_bitand(" + A + ", " + B + "); })";
        case Tok::PIPE:          return pre + "ov_bitor(" + A + ", " + B + "); })";
        case Tok::CARET:         return pre + "ov_bitxor(" + A + ", " + B + "); })";
        case Tok::SHL:           return pre + "ov_lshift(" + A + ", " + B + "); })";
        case Tok::SHR:           return pre + "ov_rshift(" + A + ", " + B + "); })";
        case Tok::EQUAL:         return pre + "ov_bool(ov_eq(" + A + ", " + B + ")); })";
        case Tok::BANG_EQUAL:    return pre + "ov_bool(!ov_eq(" + A + ", " + B + ")); })";
        case Tok::LESS:          return pre + "ov_bool(ov_cmp(" + A + ", " + B + ") < 0); })";
        case Tok::GREATER:       return pre + "ov_bool(ov_cmp(" + A + ", " + B + ") > 0); })";
        case Tok::LESS_EQUAL:    return pre + "ov_bool(ov_cmp(" + A + ", " + B + ") <= 0); })";
        case Tok::GREATER_EQUAL: return pre + "ov_bool(ov_cmp(" + A + ", " + B + ") >= 0); })";
        default:
          fail("unsupported binary operator");
          return pre + A + "; })";
      }
    }

    case ExprKind::Logical: {
      std::string a = fresh();
      const std::string& A = a;
      if (e->op == Tok::KW_AND)
        return "({ OvValue " + a + " = " + ex(e->a) + "; ov_bool(ov_truthy(" + A +
               ") && ov_truthy(" + ex(e->b) + ")); })";
      return "({ OvValue " + a + " = " + ex(e->a) + "; ov_bool(ov_truthy(" + A +
             ") || ov_truthy(" + ex(e->b) + ")); })";
    }

    case ExprKind::Ternary: {
      std::string c = fresh();
      return "({ OvValue " + c + " = " + ex(e->a) + "; ov_truthy(" + c + ") ? " +
             ex(e->b) + " : " + ex(e->c) + "; })";
    }

    case ExprKind::Assign: {
      // Fast path: target is a proven int/float Identifier
      if (e->op == Tok::ASSIGN && e->a && e->a->kind == ExprKind::Identifier) {
        const std::string& name = e->a->name;
        bool pi = is_proven_int_any_scope(name);
        bool pf = is_proven_float_any_scope(name);
        if (pi || pf) {
          std::string rhs;
          if (pi && is_int_expr(e->b)) {
            rhs = ex_int(e->b);
          } else if (pf && is_float_expr(e->b)) {
            rhs = ex_float(e->b);
          } else if (pi) {
            std::string tmp_v = fresh();
            // Note: we can't emit a line here, so we use a statement expression
            rhs = "({ OvValue " + tmp_v + " = " + ex(e->b) + "; (" + tmp_v + ").i; })";
          } else if (pf) {
            std::string tmp_v = fresh();
            rhs = "({ OvValue " + tmp_v + " = " + ex(e->b) + "; (" + tmp_v + ").f; })";
          } else {
            rhs = ex(e->b);
          }
          // Get the C variable name for the target
          std::string* c = lookup(name);
          if (c) {
            return "(" + *c + " = " + rhs + ", " + rhs + ")";
          }
          // Variable not yet in scope (shouldn't happen for assignments)
          return rhs;
        }
      }
      std::string v = fresh();
      if (e->op == Tok::ASSIGN) {
        return "({ OvValue " + v + " = " + ex(e->b) + "; " +
               emit_assign(e->a, v) + "; " + v + "; })";
      }
      // Compound: the interpreter evaluates the value, then the current
      // target, then applies the operator and rebinds.
      std::string cur = fresh();
      std::string rhs;
      const std::string& V = v;
      const std::string& C = cur;
      switch (e->op) {
        case Tok::PLUS_EQUAL:    rhs = "ov_add(" + C + ", " + V + ")"; break;
        case Tok::MINUS_EQUAL:   rhs = "ov_sub(" + C + ", " + V + ")"; break;
        case Tok::STAR_EQUAL:    rhs = "ov_mul(" + C + ", " + V + ")"; break;
        case Tok::SLASH_EQUAL:   rhs = "ov_div(" + C + ", " + V + ")"; break;
        case Tok::PERCENT_EQUAL: rhs = "ov_mod(" + C + ", " + V + ")"; break;
        default:
          fail("unsupported compound assignment");
          rhs = C;
      }
      return "({ OvValue " + v + " = " + ex(e->b) + "; OvValue " + cur + " = " +
             ex(e->a) + "; " + emit_assign(e->a, rhs) + "; " + rhs + "; })";
    }

    case ExprKind::Index: {
      std::string b = fresh(), i = fresh(), o = fresh();
      // Pin base and index via mutation guard: intermediate objects on the C
      // stack are not GC roots, so a collection triggered by ov_str_lit (or
      // any other allocation inside ex(e->b)) would free them. The mutation
      // counter is nestable so chained calls are safe.
      return "({ ov_gc_begin_mutation(); OvValue " + b + " = " + ex(e->a) +
             "; OvValue " + i + " = " + ex(e->b) + "; OvValue " + o +
             "; if (ov_h_index(" + b + ", " + i + ", &" + o + ")) { ov_gc_end_mutation(); ov_throw_value(ov_h_err_value()); } " +
             "ov_gc_end_mutation(); " + o + "; })";
    }

    case ExprKind::Member: {
      std::string b = fresh(), o = fresh();
      return "({ ov_gc_begin_mutation(); OvValue " + b + " = " + ex(e->a) +
             "; OvValue " + o + "; if (ov_h_member(" + b + ", " + quote_c(e->name) +
             ", &" + o + ")) { ov_gc_end_mutation(); ov_throw_value(ov_h_err_value()); } " +
             "ov_gc_end_mutation(); " + o + "; })";
    }

    case ExprKind::Slice:    return emit_slice(e);
    case ExprKind::ListComp: return emit_listcomp(e);
    case ExprKind::Closure:  return emit_closure(e);
    // A raw-returning call yields int64_t at the C level; a value context
    // needs the 16-byte tagged form, so it is boxed here. The int fast
    // paths never come through here -- is_int_expr/ex_int use the raw call.
    case ExprKind::Call:
      if (call_returns_raw_int(e)) return "ov_int(" + emit_call(e) + ")";
      return emit_call(e);
    case ExprKind::Cast: {
      if (!e->type) return ex(e->a);
      const std::string& n = e->type->name;
      std::string a = fresh();
      const std::string& A = a;
      if (n == "Int")
        return "({ OvValue " + a + " = " + ex(e->a) + "; ov_int(ov_tagof(" + A +
               ") == OV_FLOAT ? (int64_t)(" + A + ").f : " + A + ".i); })";
      if (n == "Float")
        return "({ OvValue " + a + " = " + ex(e->a) + "; ov_float(ov_tagof(" + A +
               ") == OV_INT ? (double)(" + A + ").i : " + A + ".f); })";
      if (n == "String" || n == "Str")
        return "({ OvValue " + a + " = " + ex(e->a) + "; ov_str(ov_render(" + A + ")); })";
      return ex(e->a);
    }

    case ExprKind::Error:
    default:
      fail("cannot lower this expression");
      return "ov_nil()";
  }
}

// ----------------------------------------------------------- interpolation
std::string Gen::emit_interp(const Expr* e) {
  std::string acc = fresh();
  std::string s = "({ OvStr* " + acc + " = ov_str_new(\"\", 0); ";
  size_t n = e->items.size(), m = e->parts.size(), li = 0;
  for (size_t i = 0; i < m; i++) {
    if (li < n) {
      const std::string& t = e->items[li++]->sval;
      s += acc + " = ov_str_concat(" + acc + ", ov_str_new(" + quote_c(t) + ", " +
           std::to_string(t.size()) + ")); ";
    }
    s += acc + " = ov_str_concat(" + acc + ", ov_render(" + ex(e->parts[i]) +
         ")); ";
  }
  for (; li < n; li++) {
    const std::string& t = e->items[li]->sval;
    s += acc + " = ov_str_concat(" + acc + ", ov_str_new(" + quote_c(t) + ", " +
         std::to_string(t.size()) + ")); ";
  }
  return s + "ov_str(" + acc + "); })";
}

// ------------------------------------------------------------------ slicing
std::string Gen::emit_slice(const Expr* e) {
  std::string b = fresh(), lo = fresh(), hi = fresh(), n = fresh();
  std::string out = fresh(), outl = fresh(), a = fresh(), z = fresh(), ai = fresh();
  const std::string B = b, N = n, A = a, Z = z, O = out;
  std::string s = "({ OvValue " + b + " = " + ex(e->a) + "; ";
  s += "int64_t " + n + " = (ov_tagof(" + B + ") == OV_STRING) ? " + B +
       ".str->len : (ov_tagof(" + B + ") == OV_LIST) ? " + B + ".list->len : -1; ";
  s += "if (" + N + " < 0) ov_throw_str(ov_str_new(\"cannot slice\", 12)); ";
  s += "OvValue " + lo + " = " + (e->b ? ex(e->b) : std::string("ov_int(0)")) + "; ";
  s += "OvValue " + hi + " = " + (e->c ? ex(e->c) : ("(" + N + ")")) + "; ";
  s += "int64_t " + a + " = " + lo + ".i, " + z + " = " + hi + ".i; ";
  s += "if (" + A + " < 0) " + A + " += " + N + "; ";
  s += "if (" + Z + " < 0) " + Z + " += " + N + "; ";
  s += "if (" + A + " < 0) " + A + " = 0; ";
  s += "if (" + Z + " > " + N + ") " + Z + " = " + N + "; ";
  s += "if (" + Z + " < " + A + ") " + Z + " = " + A + "; ";
  s += "OvValue " + out + "; ";
  s += "if (ov_tagof(" + B + ") == OV_STRING) { " + O + " = ov_str(ov_str_slice(" +
       B + ".str, " + A + ", " + Z + ")); } ";
  s += "else { OvList* " + outl + " = ov_list_new(); for (int64_t " + ai + " = " +
       A + "; " + ai + " < " + Z + "; " + ai + "++) ov_list_push(" + outl +
       ", ov_list_get(" + B + ".list, " + ai + ")); " + O + " = ov_list(" + outl +
       "); } ";
  return s + O + "; })";
}

// ------------------------------------------------------------- comprehension
std::string Gen::emit_listcomp(const Expr* e) {
  // The accumulator is registered as a root for the whole
  // comprehension: element expressions may allocate (and so
  // trigger a collection), and an unrooted half-built list
  // would be swept out from under the push.
  std::string out = fresh(), outv = fresh(), orm = fresh();
  std::string s = "({ size_t " + orm + " = ov_gc_roots_mark(); OvList* " + out +
                  " = ov_list_new(); OvValue " + outv + " = ov_list(" + out + "); " +
                  "ov_gc_register_root(&" + outv + "); ";
  std::vector<std::string> scoped;  // names to restore on the way out

  std::function<void(size_t)> emit_loops = [&](size_t gi) {
    if (gi == e->generators.size()) {
      s += "ov_list_push(" + out + ", " + ex(e->items[0]) + "); ";
      return;
    }
    const Generator& g = e->generators[gi];
    std::string it = fresh(), i = fresh(), held = fresh(), irm = fresh();
    std::string var = ident(g.var, tmp_++);
    push_scope();
    scoped.push_back(g.var);
    scopes_.back()[g.var] = var;

    std::string iterable = ex(g.iterable);
    std::string cond;
    if (g.cond) cond = ex(g.cond);  // sees this and outer loop variables

    const std::string I = it, J = i, V = var, HELD = held, IRM = irm;
    // The iterable and the list derived from it (a string's
    // characters) are rooted for the duration of this
    // generator's loops: element expressions inside may
    // allocate, and both are OvLists the collector can sweep.
    s += "{ OvValue " + it + " = " + iterable + "; ";
    s += "size_t " + irm + " = ov_gc_roots_mark(); ";
    s += "ov_gc_register_root(&" + it + "); ";
    s += "OvValue " + held + " = ov_nil(); ov_gc_register_root(&" + held + "); ";
    // Handle all iterable types: string, list, int64array, float64array, stringarray
    s += "OvList* " + it + "_l = NULL; ";
    s += "OvInt64Array* " + it + "_i64a = NULL; ";
    s += "OvFloat64Array* " + it + "_f64a = NULL; ";
    s += "OvStringArray* " + it + "_stra = NULL; ";
    s += "if (ov_tagof(" + I + ") == OV_STRING) { " + HELD + " = ov_str_chars(" + I + ".str); " + it + "_l = " + HELD + ".list; } "
         "else if (ov_tagof(" + I + ") == OV_LIST) { " + it + "_l = " + I + ".list; } "
         "else if (ov_tagof(" + I + ") == OV_I64A) { " + it + "_i64a = " + I + ".i64a; } "
         "else if (ov_tagof(" + I + ") == OV_F64A) { " + it + "_f64a = " + I + ".f64a; } "
         "else if (ov_tagof(" + I + ") == OV_STRA) { " + it + "_stra = " + I + ".stra; } ";

    // For each iterable type, emit a loop with optional condition
    auto emit_loop = [&](const std::string& suffix, const std::string& accessor) {
      s += "if (" + I + "_" + suffix + ") { for (uint32_t " + i + " = 0; " + i + " < " + I +
           "_" + suffix + "->len; " + i + "++) { OvValue " + var + " = " + accessor + "; ";
      if (!cond.empty()) s += "if (ov_truthy(" + cond + ")) { ";
      emit_loops(gi + 1);
      if (!cond.empty()) s += " } ";
      s += "} } ";
    };

    emit_loop("l", I + "_l->items[" + J + "]");
    emit_loop("i64a", "ov_int(" + I + "_i64a->data[" + J + "])");
    emit_loop("f64a", "ov_float(" + I + "_f64a->data[" + J + "])");
    emit_loop("stra", "ov_str(" + I + "_stra->data[" + J + "])");

    s += "ov_gc_roots_restore(" + irm + "); } ";  // close scope
    pop_scope();
  };
  emit_loops(0);
  return s + "ov_gc_roots_restore(" + orm + "); " + outv + "; })";
}

// --------------------------------------------------------------- assignment
std::string Gen::emit_assign(const Expr* target, const std::string& val) {
  switch (target->kind) {
    case ExprKind::Identifier: {
      // A name shared with a closure through a box: store through the box so
      // both sides observe the write. box_of_[name] is the box OvValue.
      auto bit = box_of_.find(target->name);
      if (bit != box_of_.end() && box_writes_.count(target->name))
        return "(ov_list_set((" + bit->second + ").list, 0, " + val + "), " + val + ")";
      return bind(target->name, val);
    }
    case ExprKind::Index: {
      std::string b = fresh(), i = fresh();
      const std::string B = b, I = i, V = val;
      // An index-assign into a specialized array stores through that array's
      // own setter, which range-checks. Without these arms a specialized
      // literal ([1, 2, 3]) fell through to "cannot index-assign" -- a
      // misleading error for a perfectly good list.
      return "({ OvValue " + b + " = " + ex(target->a) + "; OvValue " + i +
             " = " + ex(target->b) + "; if (ov_tagof(" + B + ") == OV_LIST) "
             "ov_list_set(" + B + ".list, " + I + ".i, " + V + "); else "
             "if (ov_tagof(" + B + ") == OV_I64A) "
             "ov_i64a_set(" + B + ".i64a, (" + I + ").i, (" + V + ").i); else "
             "if (ov_tagof(" + B + ") == OV_F64A) "
             "ov_f64a_set(" + B + ".f64a, (" + I + ").i, (" + V + ").f); else "
             "if (ov_tagof(" + B + ") == OV_STRA) "
             "ov_stra_set(" + B + ".stra, (" + I + ").i, (" + V + ").str); else "
             "if (ov_tagof(" + B + ") == OV_MAP) ov_map_set(" + B + ".map, " + I +
             ", " + V + "); else ov_throw_str(ov_str_new(\"cannot index-assign\", "
             "17)); })";
    }
    case ExprKind::Member: {
      std::string b = fresh();
      const std::string B = b, V = val;
      return "({ OvValue " + b + " = " + ex(target->a) + "; if (ov_tagof(" + B +
             ") != OV_MAP) ov_throw_str(ov_str_new(\"cannot set field\", 15)); "
             "ov_map_set(" + B + ".map, ov_str(ov_str_new(" + quote_c(target->name) +
             ", " + std::to_string(target->name.size()) + ")), " + V + "); })";
    }
    default:
      fail("invalid assignment target");
      return "(void)0";
  }
}

// ---------------------------------------------------------------- closures
//
// Closure capture: a closure that reads an outer local shares it with the
// enclosing scope through a 1-element box list. At the capture point the
// compiler emits: box = [outer_local]; every read/write of that name inside
// the closure body goes through box[0]; every read/write of that name in the
// enclosing scope AFTER the capture point also goes through box[0]. The box
// travels in OvFunc.upvals (ov_h_make_closure), so the closure body reads
// `_fn->upvals[i]` and both sides observe each other's writes -- exactly
// matching the interpreter, which shares the parent Env.
//
// Capture semantics (matches the interpreter):
//   - shared, not snapshot: writes through the closure are visible outside
//     and vice versa (verified by tests/interp/017_closures.ov).
//   - params and locals defined inside the body are NOT captures.
//   - a closure with no free variables emits exactly the old shape
//     (ov_h_make_func, no upvals) -- zero cost for non-capturing code.
//   - capture is per closure-creation: each evaluation of the closure
//     expression boxes the CURRENT value, so loop-created closures each get
//     their own box.
//
// Free-variable analysis: walk the body, collect every Identifier that is
// not a param, not defined in the body, and not a global (user function,
// builtin namespace). Anything left that resolves in an enclosing scope is
// a capture. Assignments to a captured name inside the body are rewritten
// to box writes; assignments in the enclosing scope after capture are
// rewritten at capture time by rebinding the scope entry to the box read.
static void collect_free_vars_expr(const Expr* e,
                                   const std::set<std::string>& bound,
                                   std::set<std::string>& free_vars);
static void collect_free_vars_block(const StmtList& body,
                                    std::set<std::string>& bound,
                                    std::set<std::string>& free_vars);

static void collect_free_vars_expr(const Expr* e,
                                   const std::set<std::string>& bound,
                                   std::set<std::string>& free_vars) {
  if (!e) return;
  switch (e->kind) {
    case ExprKind::Identifier: {
      if (!bound.count(e->name)) free_vars.insert(e->name);
      return;
    }
    case ExprKind::Closure: {
      // Nested closure: its params are bound inside it; recurse with them.
      std::set<std::string> inner = bound;
      for (const auto& p : e->params) inner.insert(p.name);
      for (const Stmt* s : e->body) {
        if (s && (s->kind == StmtKind::VarDecl)) {
          for (const auto& n : s->names) inner.insert(n);
        }
      }
      collect_free_vars_block(e->body, inner, free_vars);
      for (const auto& p : e->params)
        if (p.default_value) collect_free_vars_expr(p.default_value, bound, free_vars);
      return;
    }
    case ExprKind::ListComp: {
      std::set<std::string> inner = bound;
      for (const auto& g : e->generators) {
        collect_free_vars_expr(g.iterable, inner, free_vars);
        inner.insert(g.var);
        if (g.cond) collect_free_vars_expr(g.cond, inner, free_vars);
      }
      if (!e->items.empty()) collect_free_vars_expr(e->items[0], inner, free_vars);
      return;
    }
    default: break;
  }
  collect_free_vars_expr(e->a, bound, free_vars);
  collect_free_vars_expr(e->b, bound, free_vars);
  collect_free_vars_expr(e->c, bound, free_vars);
  for (const Expr* x : e->items) collect_free_vars_expr(x, bound, free_vars);
  for (const Expr* x : e->parts) collect_free_vars_expr(x, bound, free_vars);
  for (const auto& f : e->fields) {
    collect_free_vars_expr(f.first, bound, free_vars);
    collect_free_vars_expr(f.second, bound, free_vars);
  }
  for (const Expr* x : e->args) collect_free_vars_expr(x, bound, free_vars);
  for (const auto& na : e->named_args) collect_free_vars_expr(na.value, bound, free_vars);
}

static void collect_free_vars_stmt(const Stmt* s,
                                   std::set<std::string>& bound,
                                   std::set<std::string>& free_vars) {
  if (!s) return;
  switch (s->kind) {
    case StmtKind::VarDecl:
      for (size_t i = 0; i < s->values.size(); i++)
        collect_free_vars_expr(s->values[i], bound, free_vars);
      for (const auto& n : s->names) {
        // A bare `name = value` has no declaration keyword, so a name that is
        // not already bound locally may be a WRITE to a variable captured from
        // an enclosing scope. The interpreter's VarDecl walks the parent chain
        // (Env::find) and writes through, so such a name must be treated as a
        // free variable here too -- otherwise a closure that only assigns to
        // an outer local (a setter) would not capture it.
        if (!bound.count(n)) free_vars.insert(n);
        bound.insert(n);
      }
      return;
    case StmtKind::FuncDecl:
      bound.insert(s->name);
      return;
    case StmtKind::Return:
      collect_free_vars_expr(s->expr, bound, free_vars);
      return;
    case StmtKind::ExprStmt:
      collect_free_vars_expr(s->expr, bound, free_vars);
      return;
    case StmtKind::Assign:
      collect_free_vars_expr(s->expr, bound, free_vars);
      collect_free_vars_expr(s->expr2, bound, free_vars);
      collect_free_vars_expr(s->expr3, bound, free_vars);
      return;
    case StmtKind::If:
      collect_free_vars_expr(s->expr, bound, free_vars);
      collect_free_vars_block(s->body, bound, free_vars);
      collect_free_vars_block(s->else_body, bound, free_vars);
      return;
    case StmtKind::While:
      collect_free_vars_expr(s->expr, bound, free_vars);
      collect_free_vars_block(s->body, bound, free_vars);
      return;
    case StmtKind::For:
      collect_free_vars_expr(s->expr, bound, free_vars);
      for (const auto& v : s->iter_vars) bound.insert(v);
      collect_free_vars_block(s->body, bound, free_vars);
      return;
    case StmtKind::Block:
      collect_free_vars_block(s->body, bound, free_vars);
      return;
    case StmtKind::Try:
      collect_free_vars_block(s->body, bound, free_vars);
      if (!s->catch_var.empty()) bound.insert(s->catch_var);
      collect_free_vars_block(s->else_body, bound, free_vars);
      return;
    default:
      return;
  }
}

static void collect_free_vars_block(const StmtList& body,
                                    std::set<std::string>& bound,
                                    std::set<std::string>& free_vars) {
  for (const Stmt* s : body) collect_free_vars_stmt(s, bound, free_vars);
}

// --- raw loop-variable pre-scan ----------------------------------------
// A `for v in xs` over a specialized array (OvInt64Array /
// OvFloat64Array) can keep `v` in a raw C numeric local -- no
// per-iteration boxing -- when the body never writes `v`, never
// shadows it, and never captures it in a closure. Reads are fine:
// `ex()` boxes a proven raw local at every OvValue use site, and the
// int/float fast paths (is_int_expr / is_float_expr) keep arithmetic
// in C registers.
static bool expr_keeps_loop_var_raw(const Expr* e, const std::string& name);
static bool stmt_keeps_loop_var_raw(const Stmt* s, const std::string& name);
static bool block_keeps_loop_var_raw(const StmtList& body,
                                     const std::string& name);

static bool expr_keeps_loop_var_raw(const Expr* e, const std::string& name) {
  if (!e) return true;
  switch (e->kind) {
    case ExprKind::Assign:
      if (e->a && e->a->kind == ExprKind::Identifier && e->a->name == name)
        return false;  // v = ... / v += ... writes the loop variable
      break;
    case ExprKind::Closure: {
      // A closure that reads `name` captures it -- a raw C local
      // cannot travel through OvFunc.upvals.
      std::set<std::string> bound;
      for (const auto& p : e->params) bound.insert(p.name);
      std::set<std::string> free_vars;
      collect_free_vars_block(e->body, bound, free_vars);
      if (free_vars.count(name)) return false;
      for (const auto& p : e->params)
        if (!expr_keeps_loop_var_raw(p.default_value, name)) return false;
      return true;
    }
    case ExprKind::ListComp: {
      // Generators bind left to right: one bound to `name` shadows
      // the loop variable from that point on. Earlier iterables and
      // filters still see the loop variable (reads only).
      bool shadowed = false;
      for (const auto& g : e->generators) {
        if (!shadowed) {
          if (!expr_keeps_loop_var_raw(g.iterable, name)) return false;
          if (!expr_keeps_loop_var_raw(g.cond, name)) return false;
          if (g.var == name) shadowed = true;
        }
      }
      if (shadowed) return true;
      return expr_keeps_loop_var_raw(
          e->items.empty() ? nullptr : e->items[0], name);
    }
    case ExprKind::ListLit:
      for (const Expr* it : e->items)
        if (!expr_keeps_loop_var_raw(it, name)) return false;
      return true;
    case ExprKind::MapLit:
      for (const auto& f : e->fields) {
        if (!expr_keeps_loop_var_raw(f.first, name)) return false;
        if (!expr_keeps_loop_var_raw(f.second, name)) return false;
      }
      return true;
    case ExprKind::Interp:
      for (const Expr* p : e->parts)
        if (!expr_keeps_loop_var_raw(p, name)) return false;
      return true;
    case ExprKind::Call:
      if (!expr_keeps_loop_var_raw(e->a, name)) return false;
      for (const Expr* a : e->args)
        if (!expr_keeps_loop_var_raw(a, name)) return false;
      for (const auto& na : e->named_args)
        if (!expr_keeps_loop_var_raw(na.value, name)) return false;
      return true;
    default:
      break;
  }
  return expr_keeps_loop_var_raw(e->a, name) &&
         expr_keeps_loop_var_raw(e->b, name) &&
         expr_keeps_loop_var_raw(e->c, name);
}

static bool block_keeps_loop_var_raw(const StmtList& body,
                                     const std::string& name) {
  for (const Stmt* s : body)
    if (!stmt_keeps_loop_var_raw(s, name)) return false;
  return true;
}

static bool stmt_keeps_loop_var_raw(const Stmt* s, const std::string& name) {
  if (!s) return true;
  switch (s->kind) {
    case StmtKind::Assign:
      if (s->expr && s->expr->kind == ExprKind::Identifier &&
          s->expr->name == name)
        return false;  // v = ... / v += ... writes the loop variable
      return expr_keeps_loop_var_raw(s->expr, name) &&
             expr_keeps_loop_var_raw(s->expr2, name) &&
             expr_keeps_loop_var_raw(s->expr3, name);
    case StmtKind::VarDecl:
      for (const auto& n : s->names)
        if (n == name) return false;  // shadows the loop variable
      for (const Expr* v : s->values)
        if (!expr_keeps_loop_var_raw(v, name)) return false;
      return true;
    case StmtKind::For:
      if (!expr_keeps_loop_var_raw(s->expr, name)) return false;
      for (const auto& v : s->iter_vars)
        if (v == name) return true;  // inner loop shadows: its own variable
      return block_keeps_loop_var_raw(s->body, name);
    case StmtKind::FuncDecl: {
      for (const auto& p : s->params)
        if (p.name == name) return true;  // parameter shadows
      std::set<std::string> bound;
      for (const auto& p : s->params) bound.insert(p.name);
      std::set<std::string> free_vars;
      collect_free_vars_block(s->body, bound, free_vars);
      if (free_vars.count(name)) return false;  // would capture the raw local
      for (const auto& p : s->params)
        if (!expr_keeps_loop_var_raw(p.default_value, name)) return false;
      return true;
    }
    case StmtKind::Try:
      if (!block_keeps_loop_var_raw(s->body, name)) return false;
      if (!s->catch_var.empty() && s->catch_var == name)
        return true;  // catch variable shadows in the handler
      return block_keeps_loop_var_raw(s->else_body, name);
    default:
      // If / While / Block / Return / Throw / ExprStmt / Debug: walk
      // every expression slot and sub-block.
      return expr_keeps_loop_var_raw(s->expr, name) &&
             expr_keeps_loop_var_raw(s->expr2, name) &&
             expr_keeps_loop_var_raw(s->expr3, name) &&
             block_keeps_loop_var_raw(s->body, name) &&
             block_keeps_loop_var_raw(s->else_body, name);
  }
}

// --- cross-function boundary planning -------------------------------------
//
// Which user functions may cross their call boundary as a raw int64_t
// instead of a 16-byte tagged OvValue? BASELINE.md measured the cost of
// boxing every argument and every return: `fib` sits at 9-16x of C while a
// call is a compare, two decrements and an add. Loop locals and specialized
// collections are already unboxed; parameters and return values were not.
//
// A signature is one decision applied to every call site, so each claim is
// proved from the whole program before any body is lowered. The rules are
// deliberately conservative -- each one exists to prevent a specific way
// the claim could be wrong:
//
//   * no anonymous function anywhere in the body. A closure captures its
//     enclosing scope by reference, and a raw int64 has no box to capture
//     through.
//   * no parameter is ever assigned (`n = 2.5` would truncate into the
//     int64 silently -- the exact class of divergence this work must not
//     introduce) and no body binding shadows a parameter (`for n in ...`,
//     `n = ...`, a closure param, a catch var): later uses of the name
//     would refer to the local, not the parameter.
//   * every call site's argument is statically an int: a literal, int
//     arithmetic on literals, the *enclosing* function's own raw
//     parameters, or a global sema already proved int. A map read, a list
//     element, or another function's boxed parameter keeps it boxed.
//   * the return is raw only when the body's last statement is a `return`
//     (falling off the end answers nil on the interpreter and 0 on a raw
//     function -- a silent divergence) and every own-level return is
//     statically an int.
//
// Self-recursion is allowed and is load-bearing: `fib(n-1)` is an argument
// built from `fib`'s own raw parameter. A function planning its own claims
// assumes them; every other context uses the claims computed so far, and
// the whole thing is iterated to a fixed point. Mutually recursive
// functions never bootstrap that way and simply stay boxed.

static void block_has_closure(const StmtList& body, bool& found);

static bool expr_has_closure(const Expr* e) {
  if (!e) return false;
  if (e->kind == ExprKind::Closure) return true;
  for (const Expr* c : {e->a, e->b, e->c})
    if (expr_has_closure(c)) return true;
  for (const Expr* it : e->items)
    if (expr_has_closure(it)) return true;
  for (const Expr* p : e->parts)
    if (expr_has_closure(p)) return true;
  for (const auto& f : e->fields)
    if (expr_has_closure(f.first) || expr_has_closure(f.second)) return true;
  for (const Expr* a : e->args)
    if (expr_has_closure(a)) return true;
  for (const auto& na : e->named_args)
    if (expr_has_closure(na.value)) return true;
  for (const Param& p : e->params)
    if (p.default_value && expr_has_closure(p.default_value)) return true;
  bool nested = false;
  block_has_closure(e->body, nested);
  if (nested) return true;
  for (const Generator& g : e->generators)
    if (expr_has_closure(g.iterable) || expr_has_closure(g.cond)) return true;
  return false;
}

static void block_has_closure(const StmtList& body, bool& found) {
  if (found) return;
  for (const Stmt* s : body) {
    if (!s) continue;
    for (Expr* e : {s->expr, s->expr2, s->expr3})
      if (expr_has_closure(e)) { found = true; return; }
    for (Expr* e : s->values)
      if (expr_has_closure(e)) { found = true; return; }
    for (const Param& p : s->params)
      if (p.default_value && expr_has_closure(p.default_value)) { found = true; return; }
    block_has_closure(s->body, found);
    block_has_closure(s->else_body, found);
    block_has_closure(s->init, found);
    if (found) return;
  }
}

// Names bound by a statement or anything inside it: declarations, loop
// variables, catch variables, closure and (nested) function parameters.
// Function parameters themselves are supplied by the caller, which is
// checking whether the body shadows one of them.
static void collect_expr_bindings(const Expr* e, std::set<std::string>& out);
static void collect_block_bindings(const StmtList& body, std::set<std::string>& out) {
  for (const Stmt* s : body) {
    if (!s) continue;
    for (const auto& n : s->names) out.insert(n);
    for (const auto& v : s->iter_vars) out.insert(v);
    if (!s->catch_var.empty()) out.insert(s->catch_var);
    for (Expr* e : {s->expr, s->expr2, s->expr3}) collect_expr_bindings(e, out);
    for (Expr* e : s->values) collect_expr_bindings(e, out);
    for (const Param& p : s->params) {
      out.insert(p.name);
      collect_expr_bindings(p.default_value, out);
    }
    collect_block_bindings(s->body, out);
    collect_block_bindings(s->else_body, out);
    collect_block_bindings(s->init, out);
  }
}

static void collect_expr_bindings(const Expr* e, std::set<std::string>& out) {
  if (!e) return;
  if (e->kind == ExprKind::Closure) {
    for (const Param& p : e->params) out.insert(p.name);
    collect_block_bindings(e->body, out);  // its locals too: they bind here
    return;
  }
  for (const Expr* c : {e->a, e->b, e->c}) collect_expr_bindings(c, out);
  for (const Expr* it : e->items) collect_expr_bindings(it, out);
  for (const Expr* p : e->parts) collect_expr_bindings(p, out);
  for (const auto& f : e->fields) {
    collect_expr_bindings(f.first, out);
    collect_expr_bindings(f.second, out);
  }
  for (const Expr* a : e->args) collect_expr_bindings(a, out);
  for (const auto& na : e->named_args) collect_expr_bindings(na.value, out);
  for (const Param& p : e->params) {
    out.insert(p.name);
    collect_expr_bindings(p.default_value, out);
  }
  for (const Generator& g : e->generators) {
    collect_expr_bindings(g.iterable, out);
    collect_expr_bindings(g.cond, out);
  }
}

// Identifiers a statement (or anything inside it) assigns to. An assignment
// is a write to an existing binding, not a new one, so it is tracked apart.
static void collect_expr_assignments(const Expr* e, std::set<std::string>& out) {
  if (!e) return;
  if (e->kind == ExprKind::Assign && e->a && e->a->kind == ExprKind::Identifier)
    out.insert(e->a->name);
  if (e->kind == ExprKind::Closure) {
    collect_expr_assignments(e->a, out);
    return;  // the body's assignments bind its own scope; covered by bindings
  }
  for (const Expr* c : {e->a, e->b, e->c}) collect_expr_assignments(c, out);
  for (const Expr* it : e->items) collect_expr_assignments(it, out);
  for (const Expr* p : e->parts) collect_expr_assignments(p, out);
  for (const auto& f : e->fields) {
    collect_expr_assignments(f.first, out);
    collect_expr_assignments(f.second, out);
  }
  for (const Expr* a : e->args) collect_expr_assignments(a, out);
  for (const auto& na : e->named_args) collect_expr_assignments(na.value, out);
  for (const Param& p : e->params) collect_expr_assignments(p.default_value, out);
  for (const Generator& g : e->generators) {
    collect_expr_assignments(g.iterable, out);
    collect_expr_assignments(g.cond, out);
  }
}

static void collect_block_assignments(const StmtList& body, std::set<std::string>& out) {
  for (const Stmt* s : body) {
    if (!s) continue;
    if (s->kind == StmtKind::Assign && s->expr && s->expr->kind == ExprKind::Identifier)
      out.insert(s->expr->name);
    for (Expr* e : {s->expr, s->expr2, s->expr3}) collect_expr_assignments(e, out);
    for (Expr* e : s->values) collect_expr_assignments(e, out);
    for (const Param& p : s->params) collect_expr_assignments(p.default_value, out);
    collect_block_assignments(s->body, out);
    collect_block_assignments(s->else_body, out);
    collect_block_assignments(s->init, out);
  }
}

// `return <expr>` statements at this function's own level. Closure bodies
// are skipped on purpose: their returns belong to the anonymous function.
static void collect_own_returns(const StmtList& body, std::vector<const Expr*>& out) {
  for (const Stmt* s : body) {
    if (!s) continue;
    if (s->kind == StmtKind::Return) out.push_back(s->expr);  // null: bare `return`
    if (s->kind == StmtKind::FuncDecl) continue;              // a nested function's returns
    collect_own_returns(s->body, out);
    collect_own_returns(s->else_body, out);
    collect_own_returns(s->init, out);
  }
}

static bool block_tail_returns(const StmtList& body) {
  for (auto it = body.rbegin(); it != body.rend(); ++it) {
    const Stmt* s = *it;
    if (!s) return false;
    if (s->kind == StmtKind::FuncDecl) continue;
    return s->kind == StmtKind::Return;
  }
  return false;
}

// A raw int64 parameter may only appear where a value is read. A member
// (`a.len`), an index (`a[i]`), a slice, a call through the parameter itself
// (`a(...)`), or iterating it (`for x in a`) all need the 16-byte tagged
// form: the raw claim cannot serve any of them, and emitting one for such a
// body is a clang type error, not a slow program. Value positions -- call
// arguments, arithmetic, returns, interpolations -- are fine, because ex()
// boxes a proven int at every one of them.
static bool ident_is(const Expr* e, const std::string& name) {
  return e && e->kind == ExprKind::Identifier && e->name == name;
}

static bool expr_uses_param_as_value(const Expr* e, const std::string& name);
static bool block_uses_param_as_value(const StmtList& body,
                                       const std::string& name) {
  for (const Stmt* s : body)
    if (s) {
      for (Expr* x : {s->expr, s->expr2, s->expr3})
        if (expr_uses_param_as_value(x, name)) return true;
      for (Expr* x : s->values)
        if (expr_uses_param_as_value(x, name)) return true;
      // `for v in <param>` iterates the parameter itself.
      if (ident_is(s->expr, name) && s->kind == StmtKind::For) return true;
      if (block_uses_param_as_value(s->body, name)) return true;
      if (block_uses_param_as_value(s->else_body, name)) return true;
      if (block_uses_param_as_value(s->init, name)) return true;
    }
  return false;
}

static bool expr_uses_param_as_value(const Expr* e, const std::string& name) {
  if (!e) return false;
  switch (e->kind) {
    case ExprKind::Member:
    case ExprKind::Slice:
      // The object being read from must be the tagged form; the parts in
      // slice bounds are value positions and are checked by recursion below.
      if (ident_is(e->a, name)) return true;
      break;
    case ExprKind::Index:
      if (ident_is(e->a, name)) return true;
      break;
    case ExprKind::Call:
      if (ident_is(e->a, name)) return true;  // calling the parameter itself
      break;
    case ExprKind::Assign:
      // `a[0] = ...`, `a.field = ...`: writing through the parameter.
      if (expr_uses_param_as_value(e->a, name)) return true;
      break;
    default:
      break;
  }
  for (const Expr* c : {e->a, e->b, e->c})
    if (expr_uses_param_as_value(c, name)) return true;
  for (const Expr* it : e->items)
    if (expr_uses_param_as_value(it, name)) return true;
  for (const Expr* p : e->parts)
    if (expr_uses_param_as_value(p, name)) return true;
  for (const auto& f : e->fields) {
    if (expr_uses_param_as_value(f.first, name)) return true;
    if (expr_uses_param_as_value(f.second, name)) return true;
  }
  for (const Expr* a : e->args)
    if (expr_uses_param_as_value(a, name)) return true;
  for (const auto& na : e->named_args)
    if (expr_uses_param_as_value(na.value, name)) return true;
  for (const Param& p : e->params) {
    if (expr_uses_param_as_value(p.default_value, name)) return true;
    if (block_uses_param_as_value(e->body, name)) return true;
  }
  for (const Generator& g : e->generators) {
    if (expr_uses_param_as_value(g.iterable, name)) return true;
    if (expr_uses_param_as_value(g.cond, name)) return true;
  }
  return false;
}

// Every Call node, including inside closure bodies: a closure calls a
// top-level function through the same signature as any other caller.
static void walk_stmt_calls(const Stmt* s, const std::function<void(const Expr*)>& fn);
static void walk_expr_calls(const Expr* e, const std::function<void(const Expr*)>& fn) {
  if (!e) return;
  if (e->kind == ExprKind::Call) fn(e);
  for (const Expr* c : {e->a, e->b, e->c}) walk_expr_calls(c, fn);
  for (const Expr* it : e->items) walk_expr_calls(it, fn);
  for (const Expr* p : e->parts) walk_expr_calls(p, fn);
  for (const auto& f : e->fields) {
    walk_expr_calls(f.first, fn);
    walk_expr_calls(f.second, fn);
  }
  for (const Expr* a : e->args) walk_expr_calls(a, fn);
  for (const auto& na : e->named_args) walk_expr_calls(na.value, fn);
  for (const Param& p : e->params) walk_expr_calls(p.default_value, fn);
  for (const Stmt* s : e->body) walk_stmt_calls(s, fn);
  for (const Generator& g : e->generators) {
    walk_expr_calls(g.iterable, fn);
    walk_expr_calls(g.cond, fn);
  }
}

static void walk_stmt_calls(const Stmt* s, const std::function<void(const Expr*)>& fn) {
  if (!s) return;
  for (Expr* e : {s->expr, s->expr2, s->expr3}) walk_expr_calls(e, fn);
  for (Expr* e : s->values) walk_expr_calls(e, fn);
  for (const Param& p : s->params) walk_expr_calls(p.default_value, fn);
  for (const Stmt* b : s->body) walk_stmt_calls(b, fn);
  for (const Stmt* b : s->else_body) walk_stmt_calls(b, fn);
  for (const Stmt* b : s->init) walk_stmt_calls(b, fn);
}

static void walk_block_calls(const StmtList& body, const std::function<void(const Expr*)>& fn) {
  for (const Stmt* s : body) walk_stmt_calls(s, fn);
}

std::string Gen::emit_closure(const Expr* e) {
  // Free-variable analysis: params + body-defined locals are bound; anything
  // else the body reads that is neither a user function nor a resolvable
  // global is a capture. Globals (userfns_, namespace roots) are NOT
  // captures -- they resolve at module scope in the emitted C.
  std::set<std::string> bound;
  for (const auto& p : e->params) bound.insert(p.name);
  std::set<std::string> free_vars;
  collect_free_vars_block(e->body, bound, free_vars);
  std::vector<std::string> captures;
  for (const auto& n : free_vars) {
    if (userfns_.count(n)) continue;                       // global function
    if (n == "http" || n == "json" || n == "str" || n == "math" || n == "time" ||
        n == "file")
      continue;                                            // namespace root
    std::string* c = lookup(n);
    if (!c) continue;  // unknown name: leave for the body's own error path
    captures.push_back(n);
  }

  std::string cname = "ov_anon_" + std::to_string(tmp_++);
  // Match OvFnPtr exactly: (OvFunc* fn, OvValue* argv, int argc). Parameters are read
  // positionally out of argv, so the emitted body is ABI-compatible with
  // everything else that stores a OvFunc. Captures read from _fn->upvals.
  std::string sig = "static OvValue " + cname + "(struct OvFunc* _fn, OvValue* _argv, int _argc)";

  // Emit the body into a scratch buffer with its own scope chain, then append
  // it to the module's deferred-definition list.
  std::string saved_out;
  saved_out.swap(out);
  int saved_indent = indent_;
  std::string saved_ret = ret_label_;
  indent_ = 0;
  ret_label_ = lbl("Lret");

  push_scope();
  // Save the enclosing box set: the body gets its own. A nested closure that
  // captures a name already boxed by an outer closure must share the SAME
  // box -- handled at ITS capture point via box_of_.
  auto saved_box_of = box_of_;
  auto saved_box_writes = box_writes_;
  box_writes_.clear();
  box_of_.clear();
  for (size_t i = 0; i < captures.size(); i++) {
    scopes_.back()[captures[i]] =
        "ov_list_get(((OvValue*)_fn->upvals)[" + std::to_string(i) + "].list, 0)";
    box_writes_.insert(captures[i]);
    // The box OvValue in this closure's body is the upvals slot.
    box_of_[captures[i]] = "((OvValue*)_fn->upvals)[" + std::to_string(i) + "]";
  }
  for (size_t i = 0; i < e->params.size(); i++)
    scopes_.back()[e->params[i].name] = ident(e->params[i].name, i);

  // Two passes, for the same reason as emit_function: lower the body to learn
  // the locals, then declare and root them at entry.
  std::vector<std::string> outer_roots;
  outer_roots.swap(cur_roots_);
  std::unordered_set<std::string> outer_int_coll;
  outer_int_coll.swap(int_coll_);
  inline_roots_.clear();

  std::string body_buf;
  body_buf.swap(out);
  indent_ = 1;
  fn_has_return_ = false;
  line("(void)_argv; (void)_argc;");
  for (size_t i = 0; i < e->params.size(); i++)
    line("OvValue " + ident(e->params[i].name, i) + " = _argv[" +
         std::to_string(i) + "];");
  // Parameters are heap references like any other local, so they are rooted
  // the same way. They are not roots by siding: a parameter that holds a
  // freshly built array is only reachable from this frame, and the collector
  // fires on any allocation -- so a function that allocates in a loop over a
  // parameter would otherwise read freed memory. The mask the rag index
  // passes down is exactly that shape, and it segfaulted at index 730 of a
  // 1003-slot array whose data pointer had already been swept.
  for (size_t i = 0; i < e->params.size(); i++)
    line("ov_gc_register_root(&" + ident(e->params[i].name, i) + ");");
  block(e->body, false);
  if (fn_has_return_) line(ret_label_ + ": ;");
  line("  ov_gc_roots_restore(_roots_mark);");
  line("  return _ret; }");
  body_buf.swap(out);
  indent_ = 0;

  nl();
  line(sig + " {");
  indent_++;
  line("OvValue _ret = ov_nil();");
  line("size_t _roots_mark = ov_gc_roots_mark();");
  emit_root_prologue();
  cur_roots_.swap(outer_roots);
  int_coll_.swap(outer_int_coll);
  out += body_buf;
  pop_scope();

  std::string body = out;
  out.swap(saved_out);
  indent_ = saved_indent;
  ret_label_ = saved_ret;
  anon_defs_.push_back(body);
  anon_names_.push_back(cname);
  box_of_ = saved_box_of;
  box_writes_ = saved_box_writes;

  // No captures: exactly the old shape -- zero cost for non-capturing code.
  if (captures.empty()) {
    return "ov_func(ov_h_make_func(" + std::to_string(e->params.size()) + ", " + cname + "))";
  }

  // Capturing: share each outer local through a 1-element box list.
  // For each captured variable, if it is not yet boxed in the enclosing scope,
  // allocate a GC-rooted local for the box, initialize it with the variable's
  // current value, and rebind the variable to read/write through the box.
  // Then pass each capture's box OvValue into the upvals array.
  std::string ua = fresh();
  std::string s = "({ ";
  for (size_t i = 0; i < captures.size(); i++) {
    const std::string& n = captures[i];
    if (!box_of_.count(n)) {
      std::string bx = ident("box_" + n, tmp_++);
      cur_roots_.push_back(bx);
      std::string* c = lookup(n);
      std::string init_val = c ? *c : "ov_nil()";
      if (proven_ints_.count(init_val)) init_val = "ov_int(" + init_val + ")";
      else if (proven_floats_.count(init_val)) init_val = "ov_float(" + init_val + ")";
      s += bx + " = ov_list(ov_list_new()); ";
      s += "ov_list_push(" + bx + ".list, " + init_val + "); ";
      box_of_[n] = bx;
      box_writes_.insert(n);
      if (c) *c = "ov_list_get(" + bx + ".list, 0)";
    }
  }
  s += "OvValue* " + ua + " = (OvValue*)calloc(" +
       std::to_string(captures.size()) + ", sizeof(OvValue)); ";
  for (size_t i = 0; i < captures.size(); i++) {
    s += ua + "[" + std::to_string(i) + "] = " + box_of_[captures[i]] + "; ";
  }
  s += "ov_func(ov_h_make_closure(" + std::to_string(e->params.size()) + ", " +
       cname + ", " + ua + ", " + std::to_string(captures.size()) + ")); })";
  return s;
}


// Emit `ov_sb_append_str(builder, <rhs>)` when `name` is the active string
// accumulator being appended to. Returns true when it handled the statement.
bool Gen::try_sb_append(const std::string& name, const Expr* rhs, Tok op) {
  if (!sb_.active || name != sb_.var || !rhs) return false;
  if (op == Tok::PLUS_EQUAL) {
    line("ov_sb_append_value(" + sb_.cvar + ", " + ex(rhs) + ");");
    return true;
  }
  if (op == Tok::ASSIGN && rhs->kind == ExprKind::Binary && rhs->op == Tok::PLUS &&
      rhs->a && rhs->a->kind == ExprKind::Identifier && rhs->a->name == sb_.var) {
    line("ov_sb_append_value(" + sb_.cvar + ", " + ex(rhs->b) + ");");
    return true;
  }
  return false;
}

// ------------------------------------------------------------- call dispatch
// Evaluate arguments left-to-right into temporaries so evaluation order
// matches the interpreter, then dispatch.
std::string Gen::call_value_method(const Expr* e, const std::string& base) {
  const Expr* m = e->a;                      // Member
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  std::string a0 = args.size() > 0 ? args[0] : "ov_nil()";
  std::string a1 = args.size() > 1 ? args[1] : "ov_nil()";
  std::string b = fresh(), o = fresh(), av = fresh();

  // Hot-path for push/append on an int collection: the value goes in as a raw
  // int64, never boxed. sema has already retired any collection with a
  // non-int push, so the coercion here is belt and braces rather than a
  // fallback anyone is expected to reach.
  if ((m->name == "push" || m->name == "append") && args.size() <= 1) {
    if (m->a && m->a->kind == ExprKind::Identifier) {
      if (std::string* c = lookup(m->a->name)) {
        if (int_coll_.count(*c)) {
          std::string v = ex_int_of(e->args.empty() ? nullptr : e->args[0]);
          return "(ov_i64a_push(" + *c + ".i64a, " + v + "), " + base + ")";
        }
      }
    }
    // Fallback to list fast path
    return "({ OvValue " + b + " = " + base + "; OvValue " + av + " = " + a0 +
           "; OvValue " + o + "; if (ov_tagof(" + b + ") == OV_LIST) { ov_list_push(" +
           b + ".list, " + av + "); " + o + " = " + b + "; } else if (ov_h_value_method(" +
           b + ", " + quote_c(m->name) + ", " + av + ", ov_nil(), &" + o +
           ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
  }

  // pop on an int collection: the raw store back, boxed once on the way out.
  if (m->name == "pop" && args.empty()) {
    if (m->a && m->a->kind == ExprKind::Identifier) {
      if (std::string* c = lookup(m->a->name)) {
        // Pop is nil when the collection is empty, exactly as ov_list_pop is.
        // The runtime's array pop returns a raw 0 for an empty array, which
        // would print as 0 where the interpreter prints nil -- a divergence
        // built into a runtime shortcut, not into the language.
        if (int_coll_.count(*c))
          return "((" + *c + ".i64a->len ? ov_int(ov_i64a_pop(" + *c +
                 ".i64a)) : ov_nil()))";
      }
    }
  }

  return "({ OvValue " + b + " = " + base + "; OvValue " + o + "; if (ov_h_value_method(" +
         b + ", " + quote_c(m->name) + ", " + a0 + ", " + a1 + ", &" + o +
         ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
}

std::string Gen::call_namespace(const std::string& ns, const std::string& name,
                                const Expr* e) {
  // Only the namespaces the MVP chatbot/test surface needs.
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  auto named = [&](const char* k) -> std::string {
    for (const auto& na : e->named_args)
      if (na.name == k) return ex(na.value);
    return "ov_nil()";
  };

  if (ns == "json") {
    if (name == "parse") {
      std::string o = fresh(), t = fresh();
      return "({ OvValue " + o + " = " + (args.empty() ? "ov_nil()" : args[0]) +
             "; int " + t + " = 0; OvValue _r = ov_h_json_parse(" + o + ", &" + t +
             "); if (" + t + ") ov_throw_value(ov_h_err_value()); _r; })";
    }
    if (name == "stringify") {
      std::string o = fresh();
      return "({ OvValue " + o + " = " + (args.empty() ? "ov_nil()" : args[0]) +
             "; ov_str(ov_json_stringify(" + o + ")); })";
    }
    if (name == "valid") {
      std::string o = fresh();
      return "({ OvValue " + o + " = " + (args.empty() ? "ov_nil()" : args[0]) +
             "; ov_bool(ov_tagof(" + o + ") == OV_STRING && ov_json_valid(" + o +
             ".str->bytes, " + o + ".str->len)); })";
    }
    if (name == "extract") {
      // Fast JSON field extraction without full AST build
      std::string json_val = args.empty() ? "ov_nil()" : args[0];
      std::string path_val = args.size() < 2 ? "ov_nil()" : args[1];
      
      // Compile-time optimization: if path is a string literal, embed it directly
      // This avoids creating a OvValue for the path at runtime
      if (args.size() >= 2 && e->args[1] && e->args[1]->kind == ExprKind::StringLit) {
        const std::string& path_str = e->args[1]->sval;
        return "({ OvValue _v = " + json_val + "; "
               "OvValue _r = ov_nil(); if (ov_tagof(_v) == OV_STRING) { "
               "_r = ov_json_extract_field(_v.str->bytes, _v.str->len, \"" + 
               path_str + "\"); } "
               "_r; })";
      }
      
      return "({ OvValue _v = " + json_val + "; OvValue _p = " + path_val + "; "
             "OvValue _r = ov_nil(); if (ov_tagof(_v) == OV_STRING && ov_tagof(_p) == OV_STRING) { "
             "_r = ov_json_extract_field(_v.str->bytes, _v.str->len, _p.str->bytes); } "
             "_r; })";
    }
    if (name == "get_float") {
      // Fast numeric field accessor - avoids building full OvValue
      if (args.size() >= 2 && e->args[1] && e->args[1]->kind == ExprKind::StringLit) {
        const std::string& key = e->args[1]->sval;
        return "({ OvValue _v = " + args[0] + "; double _d = 0; "
               "ov_json_get_float(_v, \"" + key + "\", &_d) ? ov_float(_d) : ov_nil(); })";
      }
      return "({ OvValue _v = " + args[0] + "; OvValue _k = " + args[1] + "; "
             "double _d = 0; ov_json_get_float(_v, _k.str->bytes, &_d) ? ov_float(_d) : ov_nil(); })";
    }
    if (name == "get_int") {
      // Fast integer field accessor
      if (args.size() >= 2 && e->args[1] && e->args[1]->kind == ExprKind::StringLit) {
        const std::string& key = e->args[1]->sval;
        return "({ OvValue _v = " + args[0] + "; int64_t _i = 0; "
               "ov_json_get_int(_v, \"" + key + "\", &_i) ? ov_int(_i) : ov_nil(); })";
      }
      return "({ OvValue _v = " + args[0] + "; OvValue _k = " + args[1] + "; "
             "int64_t _i = 0; ov_json_get_int(_v, _k.str->bytes, &_i) ? ov_int(_i) : ov_nil(); })";
    }
  }
  if (ns == "http") {
    std::string url = args.empty() ? "ov_nil()" : args[0];
    std::string hdrs = named("headers");
    // http.post(url, body) / http.post(url, json = {...}): the second
    // positional argument is the body for requests that carry one, and the
    // query map for GET/DELETE/HEAD.
    bool getlike = (name == "get" || name == "delete" || name == "head" ||
                    name == "GET" || name == "DELETE" || name == "HEAD");
    std::string json = named("json");
    std::string params = "ov_nil()";
    if (args.size() >= 2) {
      if (getlike) params = args[1];
      else if (json == "ov_nil()") json = args[1];
    }
    std::string named_params = named("params");
    std::string body = named("body");
    std::string ctype = named("content_type");
    std::string timeout = named("timeout");
    std::string o = fresh();
    std::string meth = name;
    for (auto& c : meth) c = (char)toupper((unsigned char)c);
    return "({ OvValue " + o + "; if (ov_h_http(" + quote_c(meth) + ", " + url +
           ", " + hdrs + ", " + json + ", " + params + ", " + named_params + ", " +
           body + ", " + ctype + ", " + timeout + ", &" + o +
           ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
  }
  if (ns == "time") {
    if (name == "clock") return "ov_float(ov_h_now())";
    if (name == "now")   return "ov_int(ov_h_now_realtime())";
  }
  if (ns == "os") {
    // The agent's hands: a command, and (since the automation pass) the
    // file-descriptor primitives the command layer is built on. Answer for a
    // command is a map with the exit code and both streams kept apart.
    auto A = [&](size_t i) { return i < args.size() ? args[i] : "ov_nil()"; };
    if (name == "tty") return "ov_bool(ov_os_tty())";
    if (name == "run") {
      std::string cmd = args.empty() ? "ov_nil()" : args[0];
      std::string c = fresh(), o = fresh();
      return "({ int " + c + " = 0; OvValue " + o + "; if (ov_os_run(" +
             "ov_tagof(" + cmd + ") == OV_STRING ? (" + cmd + ").str->bytes : \"\", &" +
             c + ", &" + o + ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
    }
    if (name == "pipe") {
      std::string o = fresh();
      return "({ int _t = 0; OvValue " + o + "; if (ov_os_pipe(&" + o +
             ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
    }
    if (name == "read") {
      std::string o = fresh();
      return "({ OvValue " + o + "; int _f = ov_tagof(" + A(0) +
             ") == OV_INT ? (int)(" + A(0) + ").i : -1; int64_t _n = 4096; "
             "if (ov_tagof(" + A(1) + ") == OV_INT) _n = (" + A(1) + ").i; "
             "else if (ov_tagof(" + A(1) + ") == OV_FLOAT) _n = (int64_t)(" + A(1) + ").f; "
             "if (ov_os_read_fd(_f, _n, &" + o + ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
    }
    if (name == "write") {
      std::string o = fresh();
      return "({ int64_t " + o + " = 0; int _f = ov_tagof(" + A(0) +
             ") == OV_INT ? (int)(" + A(0) + ").i : -1; "
             "if (ov_os_write_fd(_f, " + A(1) + ", &" + o +
             ")) ov_throw_value(ov_h_err_value()); ov_int(" + o + "); })";
    }
    if (name == "close") {
      return "({ ov_os_close(ov_tagof(" + A(0) + ") == OV_INT ? (int)(" + A(0) +
             ").i : -1); ov_nil(); })";
    }
    if (name == "stat") {
      std::string o = fresh();
      return "({ OvValue " + o + " = ov_nil(); if (ov_os_stat_path("
             "({ OvValue _p = " + A(0) + "; ov_tagof(_p) == OV_STRING ? _p.str->bytes : \"\"; }), &" + o +
             ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
    }

    // os.exec: the same spawn the interpreter performs, argv and options in
    // the same order, so a program's result map is byte-identical either way.
    if (name == "exec") {
      std::string c = fresh(), o = fresh();
      return "({ int " + c + " = 0; OvValue " + o + "; if (ov_os_exec(" +
             (args.empty() ? "ov_nil()" : args[0]) + ", " +
             named("timeout") + ", " + named("cwd") + ", " + named("env") +
             ", " + named("stdin") + ", " + named("stdout") + ", " +
             named("stderr") + ", " + named("max_output") +
             ", &" + c + ", &" + o + ")) ov_throw_value(ov_h_err_value()); " + o + "; })";
    }
  }
  if (ns == "net") {
    // The other end of the agent: a phone connects and its lines drive the
    // same loop a terminal does. listen/accept/read/write are the four
    // primitives, with IO left to the language so line buffering stays
    // visible (and testable) rather than hidden in C.
    auto A = [&](size_t i) { return i < args.size() ? args[i] : "ov_nil()"; };
    if (name == "listen") {
      // net.listen(port) -> fd. host defaults to 0.0.0.0 so a phone on the
      // same network can reach it; pass "127.0.0.1" to keep it local.
      std::string host = named("host");
      std::string backlog = named("backlog");
      std::string o = fresh(), e = fresh();
      std::string h = host.empty()
                          ? "ov_str(ov_str_new(\"0.0.0.0\", 7))"
                          : "({ OvValue _h = " + host +
                                "; ov_str(ov_str_new(ov_tagof(_h) == OV_STRING "
                                "? _h.str->bytes : \"0.0.0.0\", ov_tagof(_h) == "
                                "OV_STRING ? _h.str->len : 7)); })";
      std::string b = backlog.empty()
                          ? "8"
                          : "({ OvValue _b = " + backlog +
                                "; ov_tagof(_b) == OV_INT ? _b.i : 8; })";
      return "({ int " + o + " = -1; char* " + e + " = NULL; " + o +
             " = ov_net_listen((" + h + ").str->bytes, ({ OvValue _p = " + A(0) +
             "; ov_tagof(_p) == OV_INT ? _p.i : 0; }), " + b + ", &" + e + "); "
             "if (" + o + " < 0) ov_throw_value(ov_str(ov_str_new(" + e + " ? " +
             e + " : \"listen failed\", " + e + " ? strlen(" + e + ") : 13))); "
             "ov_int(" + o + "); })";
    }
    if (name == "accept") {
      std::string fd = A(0);
      std::string o = fresh();
      return "({ int " + o + " = ov_net_accept(({ OvValue _f = " + fd +
             "; ov_tagof(_f) == OV_INT ? _f.i : -1; })); "
             "if (" + o + " < 0) ov_throw_value(ov_str(ov_str_new(\"accept failed\", "
             "13))); ov_int(" + o + "); })";
    }
    if (name == "read") {
      // net.read(fd, max) -> the bytes read, or "" on disconnect.
      std::string fd = A(0), mx = A(1);
      std::string b = fresh(), n = fresh();
      return "({ char " + b + "[4097]; int " + n + " = ov_net_read(({ OvValue _f = " +
             fd + "; ov_tagof(_f) == OV_INT ? _f.i : -1; }), " + b + ", 4096); "
             "ov_str(ov_str_new(" + b + ", " + n + " > 0 ? " + n + " : 0)); })";
    }
    if (name == "write") {
      // net.write(fd, text) -> Bool. All bytes or none.
      std::string fd = A(0), d = A(1);
      std::string s = fresh();
      return "({ OvStr* " + s + " = ov_str_new(({ OvValue _d = " + d +
             "; ov_tagof(_d) == OV_STRING ? _d.str->bytes : \"\"); }, "
             "({ OvValue _d = " + d +
             "; ov_tagof(_d) == OV_STRING ? _d.str->len : 0; })); "
             "ov_bool(ov_net_write(({ OvValue _f = " + fd +
             "; ov_tagof(_f) == OV_INT ? _f.i : -1; }), " + s + "->bytes, " + s +
             "->len) == 0); })";
    }
    if (name == "port") {
      std::string fd = A(0);
      std::string o = fresh();
      // ov_str_lit rather than the interpreter's ov_s: the generated C only
      // declares what ovrt.h exports, and ov_s is a host-side helper.
      return "({ int " + o + " = ov_net_port(({ OvValue _f = " + fd +
             "; ov_tagof(_f) == OV_INT ? _f.i : -1; })); if (" + o +
             " < 0) ov_throw_value(ov_str(ov_str_lit(\"not a listening socket\", "
             "24))); ov_int(" + o + "); })";
    }
    if (name == "close") {
      std::string fd = A(0);
      return "({ int _f = ({ OvValue _g = " + fd +
             "; ov_tagof(_g) == OV_INT ? _g.i : -1; }); ov_net_close(_f); ov_nil(); })";
    }
  }
  if (ns == "file") {
    auto A = [&](size_t i) { return i < args.size() ? args[i] : "ov_nil()"; };
    if (name == "open") {
      std::string o = fresh(), f = fresh();
      return "({ int64_t " + o + " = -1; char* " + f + " = "
             "({ OvValue _m = " + A(1) + "; "
             "ov_tagof(_m) == OV_STRING ? _m.str->bytes : "
             "({ OvValue _n = " + named("mode") + "; "
             "ov_tagof(_n) == OV_STRING ? _n.str->bytes : \"r\"; }); }); "
             "if (ov_file_open_fd(({ OvValue _p = " + A(0) + "; ov_tagof(_p) == "
             "OV_STRING ? _p.str->bytes : \"\"; }), " + f + ", &" + o +
             ")) ov_throw_value(ov_h_err_value()); ov_int(" + o + "); })";
    }
    if (name == "seek") {
      std::string o = fresh();
      return "({ int64_t " + o + " = ov_file_seek_fd("
             "({ OvValue _f0 = " + A(0) + "; ov_tagof(_f0) == OV_INT ? (int)_f0.i : -1; }), "
             "({ OvValue _o1 = " + A(1) + "; ov_tagof(_o1) == OV_INT ? _o1.i : 0; }), "
             "({ OvValue _w2 = " + A(2) + "; ov_tagof(_w2) == OV_STRING && "
             "_w2.str->len == 7 && !memcmp(_w2.str->bytes, \"current\", 7) ? SEEK_CUR : "
             "ov_tagof(_w2) == OV_STRING && _w2.str->len == 3 && !memcmp(_w2.str->bytes, "
             "\"end\", 3) ? SEEK_END : SEEK_SET; })); "
             "if (" + o + " < 0) ov_throw_value(ov_str(ov_str_new(\"file.seek failed\", 16))); "
             "ov_int(" + o + "); })";
    }
    if (name == "read") {
      return "({ OvValue _p = " + A(0) + "; ov_str(ov_file_read(" +
             "ov_tagof(_p) == OV_STRING ? _p.str->bytes : \"\")); })";
    }
    if (name == "write" || name == "append") {
      const char* fn = (name[0] == 'w') ? "ov_file_write" : "ov_file_append";
      std::string r = fresh();
      return "({ OvValue _p = " + A(0) + ", _d = " + A(1) + "; int " + r + " = " +
             std::string(fn) + "(ov_tagof(_p) == OV_STRING ? _p.str->bytes : \"\", "
             "ov_tagof(_d) == OV_STRING ? _d.str->bytes : \"\", "
             "ov_tagof(_d) == OV_STRING ? _d.str->len : 0); ov_bool(" + r + "); })";
    }
    if (name == "exists") {
      return "({ OvValue _p = " + A(0) + "; ov_bool(ov_file_exists(" +
             "ov_tagof(_p) == OV_STRING ? _p.str->bytes : \"\")); })";
    }
    if (name == "delete") {
      return "({ OvValue _p = " + A(0) + "; ov_bool(ov_file_delete(" +
             "ov_tagof(_p) == OV_STRING ? _p.str->bytes : \"\")); })";
    }
    if (name == "size") {
      return "({ OvValue _p = " + A(0) + "; ov_int(ov_file_size(" +
             "ov_tagof(_p) == OV_STRING ? _p.str->bytes : \"\")); })";
    }
  }
  fail("unsupported namespace call '" + ns + "." + name + "'");
  return "ov_nil()";
}

std::string Gen::call_global(const std::string& q, const Expr* e) {
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  auto ARG = [&](size_t i) { return i < args.size() ? args[i] : "ov_nil()"; };
  auto NAMED = [&](const char* k) -> std::string {
    for (const auto& na : e->named_args)
      if (na.name == k) return ex(na.value);
    return "ov_nil()";
  };
  std::string o = fresh(), t = fresh();

  if (q == "print") {
    std::string s = "({ ";
    for (size_t i = 0; i < args.size(); i++) {
      if (i) s += "fputc(' ', stdout); ";
      s += "{ OvStr* _r = ov_render(" + args[i] + "); fwrite(_r->bytes, 1, _r->len, stdout); } ";
    }
    return s + "fputc('\\n', stdout); ov_nil(); })";
  }
  if (q == "sleep") {
    // Wait without holding a core: the monotonic sleep lives in the runtime,
    // so the interpreter and every compiled binary wait identically.
    return "({ ov_os_sleep(" + ARG(0) + "); ov_nil(); })";
  }
  if (q == "eprint") {
    // Same rendering as print, but on stderr, so a program's machine-readable
    // stdout stays clean while progress and diagnostics go to the terminal.
    std::string s = "({ ";
    for (size_t i = 0; i < args.size(); i++) {
      if (i) s += "fputc(' ', stderr); ";
      s += "{ OvStr* _r = ov_render(" + args[i] + "); fwrite(_r->bytes, 1, _r->len, stderr); } ";
    }
    return s + "fputc('\\n', stderr); ov_nil(); })";
  }
  if (q == "input") {
    return "ov_h_input(" + ARG(0) + ")";
  }
  if (q == "env" || q == "getenv") {
    return "({ int " + t + " = 0; OvValue _r = ov_h_env(" + ARG(0) + ", &" + t +
           "); if (" + t + ") ov_throw_value(ov_h_err_value()); _r; })";
  }
  if (q == "env_or" || q == "getenv_or") {
    // ov_h_env sets `t` when the variable is missing (and stores the message in
    // the error slot); `t` is truthy then, so the fallback wins. Evaluated
    // lazily -- the fallback expression may have side effects.
    return "({ int " + t + " = 0; OvValue _r = ov_h_env(" + ARG(0) + ", &" + t +
           "); " + t + " ? " + ARG(1) + " : _r; })";
  }
  if (q == "setenv") {
    // Both arguments render to strings, exactly like the interpreter's setenv.
    return "({ OvStr* _n = ov_render(" + ARG(0) + "); OvStr* _v = ov_render(" +
           ARG(1) + "); ov_env_set(ov_str_data(_n), _v); ov_nil(); })";
  }
  if (q == "pad")
    return "({ int " + t + " = 0; OvValue _r = ov_h_pad(" + ARG(0) + ", " + ARG(1) +
           ", " + ARG(2) + ", " + ARG(3) + ", &" + t + "); if (" + t +
           ") ov_throw_value(ov_h_err_value()); _r; })";
  if (q == "len") {
    // Fast path: if arg is proven list/string, inline the length access
    if (args.size() == 1 && e->args[0] && e->args[0]->kind == ExprKind::Identifier) {
      const std::string& arg_name = e->args[0]->name;
      if (std::string* c = lookup(arg_name)) {
        // Only a specialized int collection has an addressable length here,
      // and its C variable is an OvValue whose array sits in .i64a. The old
      // condition tested proven_ints_/proven_floats_ -- which also holds raw
      // int64_t scalars -- and emitted `->len` on them, which is a clang type
       // error: `len(n)` for a proven int local never compiled. A raw scalar
      // now falls through to ov_h_len, which boxes it and answers correctly.
        if (int_coll_.count(*c)) {
          /* Boxed: len() is a value at every use site, and printing one is
           * the common one. */
          return "ov_int((" + *c + ").i64a->len)";
        }
      }
    }
    return "({ int " + t + " = 0; OvValue _r = ov_h_len(" + ARG(0) + ", &" + t +
           "); if (" + t + ") ov_throw_value(ov_h_err_value()); _r; })";
  }
  if (q == "str") return "ov_str(ov_render(" + ARG(0) + "))";
  if (q == "type") return "({ const char* _n = ov_type_name(" + ARG(0) + "); ov_str(ov_str_new(_n, strlen(_n))); })";
  if (q == "int")
    return "({ int " + t + " = 0; OvValue _r = ov_h_to_int(" + ARG(0) + ", &" + t +
           "); if (" + t + ") ov_throw_value(ov_h_err_value()); _r; })";
  if (q == "float")
    return "({ int " + t + " = 0; OvValue _r = ov_h_to_float(" + ARG(0) + ", &" + t +
           "); if (" + t + ") ov_throw_value(ov_h_err_value()); _r; })";
  if (q == "bool") return "ov_bool(ov_truthy(" + ARG(0) + "))";
  if (q == "exit")
    return "({ ov_request_exit(ov_isnil(" + ARG(0) + ") ? 0 : (int)(" + ARG(0) +
           ").i); ov_nil(); })";
  if (q == "throw")
    return "({ ov_throw_value(" + (args.empty() ? std::string("ov_str(ov_str_new(\"thrown\", 6))") : args[0]) + "); ov_nil(); })";
  if (q == "assert") {
    // assert(cond) / assert(cond, "message")
    std::string msg = args.size() > 1 ? ex(e->args[1])
                                      : std::string("ov_str(ov_str_new(\"assertion failed\", 16))");
    return "({ if (!ov_truthy(" + ARG(0) + ")) ov_throw_value(ov_render(" + msg +
           ")); ov_nil(); })";
  }
  if (q == "range") {
    // Fast path: if all args are int literals or proven int, create a specialized int array
    bool all_int = true;
    for (const Expr* arg : e->args) {
      if (!is_int_expr(arg)) { all_int = false; break; }
    }
    if (all_int) {
      std::string lo, hi, step;
      if (e->args.size() == 1) {
        lo = "0LL";
        hi = ex_int(e->args[0]);
        step = "1LL";
      } else if (e->args.size() == 2) {
        lo = ex_int(e->args[0]);
        hi = ex_int(e->args[1]);
        step = "1LL";
      } else {
        lo = ex_int(e->args[0]);
        hi = ex_int(e->args[1]);
        step = ex_int(e->args[2]);
      }
      std::string arr = fresh(), idx = fresh(), val = fresh();
      return "({ OvInt64Array* " + arr + " = ov_i64a_new_cap((( " + hi + " - " + lo + " + " + step + " - 1 ) / " + step + ") + 1); "
           + "int64_t " + val + " = " + lo + "; "
           + "int64_t " + idx + " = 0; "
           + "for (; " + val + " < " + hi + "; " + val + " += " + step + ") { "
           + arr + "->data[" + idx + "++] = " + val + "; } "
           + arr + "->len = " + idx + "; ov_i64a_val(" + arr + "); })";
    }
    // Original path
    if (args.size() == 1) return "ov_range(0, (" + ex(e->args[0]) + ").i, 1)";
    if (args.size() == 2)
      return "ov_range((" + ex(e->args[0]) + ").i, (" + ex(e->args[1]) + ").i, 1)";
    return "ov_range((" + ex(e->args[0]) + ").i, (" + ex(e->args[1]) + ").i, (" +
           ex(e->args[2]) + ").i)";
  }
  if (q == "abs") {
    if (args.size() == 1 && is_int_expr(e->args[0])) {
      return "ov_int(" + ex_int(e->args[0]) + " < 0 ? -" + ex_int(e->args[0]) + " : " + ex_int(e->args[0]) + ")";
    }
    if (args.size() == 1 && is_float_expr(e->args[0])) {
      return "ov_float(fabs(" + ex_float(e->args[0]) + "))";
    }
    return "({ int _t = 0; ov_h_num1(" + ARG(0) + ", &_t, 'a'); })";
  }
  if (q == "sqrt") {
    if (args.size() == 1 && is_float_expr(e->args[0])) {
      return "ov_float(sqrt(" + ex_float(e->args[0]) + "))";
    }
    return "({ int _t = 0; ov_h_num1(" + ARG(0) + ", &_t, 's'); })";
  }
  if (q == "floor") {
    if (args.size() == 1 && is_float_expr(e->args[0])) {
      return "ov_int((int64_t)floor(" + ex_float(e->args[0]) + "))";
    }
    return "({ int _t = 0; ov_h_num1(" + ARG(0) + ", &_t, 'f'); })";
  }
  if (q == "ceil") {
    if (args.size() == 1 && is_float_expr(e->args[0])) {
      return "ov_int((int64_t)ceil(" + ex_float(e->args[0]) + "))";
    }
    return "({ int _t = 0; ov_h_num1(" + ARG(0) + ", &_t, 'c'); })";
  }
  if (q == "round") {
    if (args.size() == 1 && is_float_expr(e->args[0])) {
      return "ov_int((int64_t)llround(" + ex_float(e->args[0]) + "))";
    }
    return "({ int _t = 0; ov_h_num1(" + ARG(0) + ", &_t, 'r'); })";
  }
  if (q == "min") {
    if (args.size() == 2 && is_int_expr(e->args[0]) && is_int_expr(e->args[1])) {
      return "ov_int((" + ex_int(e->args[0]) + " < " + ex_int(e->args[1]) + " ? " + ex_int(e->args[0]) + " : " + ex_int(e->args[1]) + "))";
    }
    if (args.size() == 2 && is_float_expr(e->args[0]) && is_float_expr(e->args[1])) {
      return "ov_float(fmin(" + ex_float(e->args[0]) + ", " + ex_float(e->args[1]) + "))";
    }
    return "({ OvValue _a = " + ARG(0) + ", _b = " + ARG(1) + "; int _c = ov_cmp(_a,_b); _c == 0 ? _a : (((_c < 0)) == 1 ? _a : _b); })";
  }
  if (q == "max") {
    if (args.size() == 2 && is_int_expr(e->args[0]) && is_int_expr(e->args[1])) {
      return "ov_int((" + ex_int(e->args[0]) + " > " + ex_int(e->args[1]) + " ? " + ex_int(e->args[0]) + " : " + ex_int(e->args[1]) + "))";
    }
    if (args.size() == 2 && is_float_expr(e->args[0]) && is_float_expr(e->args[1])) {
      return "ov_float(fmax(" + ex_float(e->args[0]) + ", " + ex_float(e->args[1]) + "))";
    }
    return "({ OvValue _a = " + ARG(0) + ", _b = " + ARG(1) + "; int _c = ov_cmp(_a,_b); _c == 0 ? _a : (((_c < 0)) == 0 ? _a : _b); })";
  }
  if (q == "sum") {
    // Fast path: if arg is proven int array, inline the sum
    if (args.size() == 1 && e->args[0] && e->args[0]->kind == ExprKind::Identifier) {
      const std::string& arg_name = e->args[0]->name;
      if (std::string* c = lookup(arg_name)) {
        if (proven_ints_.count(*c)) {
          // Raw int64_t array - inline sum loop
          std::string idx = fresh();
          return "({ int64_t " + idx + " = 0; int64_t " + *c + "_sum = 0; for (; " + idx + " < " + *c + "->len; " + idx + "++) " + *c + "_sum += " + *c + "->data[" + idx + "]; ov_int(" + *c + "_sum); })";
        }
        if (proven_floats_.count(*c)) {
          // Raw double array - inline sum loop
          std::string idx = fresh();
          return "({ int64_t " + idx + " = 0; double " + *c + "_sum = 0.0; for (; " + idx + " < " + *c + "->len; " + idx + "++) " + *c + "_sum += " + *c + "->data[" + idx + "]; ov_float(" + *c + "_sum); })";
        }
      }
    }
    return "({ int _t = 0; ov_h_sum(" + ARG(0) + ", &_t); })";
  }
  if (q == "upper") return "ov_str(ov_str_upper(" + ARG(0) + ".str))";
  if (q == "lower") return "ov_str(ov_str_lower(" + ARG(0) + ".str))";
  if (q == "trim")  return "ov_str(ov_str_trim(" + ARG(0) + ".str))";
  if (q == "contains")
    return "({ OvValue _n = " + ARG(0) + ", _h = " + ARG(1) + "; ov_bool(ov_tagof(_h) == OV_STRING && ov_tagof(_n) == OV_STRING ? ov_str_contains(_h.str, _n.str) : ov_in(_n, _h)); })";
  if (q == "join") return "ov_h_join(" + ARG(0) + ", " + ARG(1) + ")";
  if (q == "clock" || q == "time.clock") return "ov_float(ov_h_now())";
  if (q == "now" || q == "time.now") return "ov_int(ov_h_now_realtime())";
  if (q == "gc") return "ov_h_gc(" + ARG(0) + ")";

  fail("unknown function '" + q + "'");
  return "ov_nil()";
}

std::string Gen::emit_call(const Expr* e) {
  const Expr* callee = e->a;

  if (!callee) { fail("malformed call"); return "ov_nil()"; }

  // ns.method(...)  e.g. http.post(...), json.parse(...)
  if (callee->kind == ExprKind::Member) {
    const Expr* base = callee->a;
    if (base && base->kind == ExprKind::Identifier) {
      const std::string& ns = base->name;
      if (ns == "http" || ns == "json" || ns == "math" || ns == "time" || ns == "str" ||
          ns == "file" || ns == "os" || ns == "net")
        return call_namespace(ns, callee->name, e);
    }
    return call_value_method(e, ex(base));
  }

  if (callee->kind == ExprKind::Identifier) {
    const std::string& q = callee->name;
    // A local variable holding a closure shadows the global namespace: if the
    // name is bound in the current scope chain, it is a value call, not a
    // call to a global builtin or top-level function.
    if (lookup(q)) {
      return call_value_method_closure(q, e);
    }
    auto it = userfns_.find(q);
    if (it != userfns_.end()) {
      const Stmt* decl = it->second;
      // Positional then named arguments bound by parameter name. The bound
      // expressions are kept as AST nodes so a parameter the planner proved
      // raw can emit its argument as a raw int64_t instead of a boxed
      // OvValue; everything else boxes exactly as before.
      std::vector<const Expr*> argval(decl->params.size(), nullptr);
      for (size_t i = 0; i < e->args.size() && i < argval.size(); i++)
        argval[i] = e->args[i];
      for (const auto& na : e->named_args) {
        bool found = false;
        for (size_t k = 0; k < decl->params.size(); k++)
          if (decl->params[k].name == na.name) { argval[k] = na.value; found = true; }
        if (!found) { fail("'" + q + "' has no parameter named '" + na.name + "'"); }
      }
      std::string call = funcname(q) + "(";
      for (size_t k = 0; k < decl->params.size(); k++) {
        if (k) call += ", ";
        const Expr* a = argval[k];
        if (!a) call += "ov_nil()";
        else if (fn_param_is_raw(q, k)) call += raw_arg_or_fail(a, q, k);
        else call += ex(a);
      }
      // Empty parameter list at a CALL site is `()`, never `(void)` -- `void`
      // is only legal in a declaration/definition. The function's own
      // signature is emitted with `(void)` elsewhere.
      return call + ")";
    }
    return call_global(q, e);
  }

  // Calling a value: a closure produced by a function expression, or any
  // computed callee (an indexed/mapped closure such as `pair[0](...)`).
  {
    std::vector<std::string> args;
    for (const Expr* a : e->args) args.push_back(ex(a));
    std::string c = fresh(), r = fresh(), av = fresh();
    // Materialise the argument vector into a named array first: a compound
    // literal `{...}` cannot be passed directly as a function argument in C
    // (it is only valid in an initialiser), so `ov_h_call(f, {a,b}, ...)`
    // would not compile. A local `OvValue av[N] = {a,b};` is always valid,
    // including the zero-argument case (`OvValue av[1] = {0};`).
    std::string s = "({ OvValue " + c + " = " + ex(callee) + "; ";
    s += "OvValue " + av + "[" + std::to_string(args.size() ? args.size() : 1) + "] = { ";
    for (size_t i = 0; i < args.size(); i++) { if (i) s += ", "; s += args[i]; }
    if (args.empty()) s += "0";
    s += " }; OvValue* _a; int _n; OvValue " + r + " = ov_h_call(" + c + ", " + av +
         ", " + std::to_string(args.size()) + ", &_a, &_n); " + r + "; })";
    return s;
  }
}


std::string Gen::call_value_method_closure(const std::string& name, const Expr* e) {
  std::string* cv = lookup(name);
  std::string callee = cv ? *cv : "ov_nil()";
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  // Named arguments are not supported on a closure value; positional only.
  std::string c = fresh();
  std::string s = "({ OvValue " + c + " = " + callee + "; ";
  s += "OvValue _argv[";
  s += std::to_string(args.size() ? args.size() : 1);
  s += "]; OvValue* _slot = _argv; int _n = " + std::to_string(args.size()) + "; ";
  for (size_t i = 0; i < args.size(); i++) {
    s += "_argv[" + std::to_string(i) + "] = " + args[i] + "; ";
  }
  s += "ov_h_call(" + c + ", _argv, _n, &_slot, &_n); })";
  return s;
}

// -------------------------------------------------------------- statements
void Gen::block(const StmtList& body, bool own_scope) {
  if (own_scope) push_scope();
  for (const Stmt* s : body) {
    if (!err.empty()) return;
    stmt(s);
  }
  if (own_scope) pop_scope();
}

void Gen::stmt(const Stmt* s) {
  if (!s || !err.empty()) return;
  switch (s->kind) {
    case StmtKind::ExprStmt:
      line("(void)(" + ex(s->expr) + ");");
      return;

case StmtKind::VarDecl: {
      size_t n = s->names.size();
      // `acc = acc + e` reaches us as a VarDecl when acc is not yet bound in
      // this scope, so the builder append has to be recognised here too.
      if (sb_.active && n == 1 && s->values.size() == 1 &&
          try_sb_append(s->names[0], s->values[0], s->op))
        return;
      // Handle each variable individually, generating the appropriate RHS
      for (size_t i = 0; i < n; i++) {
        const std::string& name = s->names[i];
        bool pi = is_proven_int_any_scope(name);
        bool pf = is_proven_float_any_scope(name);
        resolve_unbox(i < s->values.size() ? s->values[i] : nullptr, pi, pf);
        std::string rhs;
        if (i < s->values.size() && s->values[i]) {
          if (pi && is_int_expr(s->values[i])) {
            rhs = ex_int(s->values[i]);
          } else if (pf && is_float_expr(s->values[i])) {
            rhs = ex_float(s->values[i]);
          } else if (pi) {
            std::string tmp_v = fresh();
            rhs = "({ OvValue " + tmp_v + " = " + ex(s->values[i]) + "; (" + tmp_v + ").i; })";
          } else if (pf) {
            std::string tmp_v = fresh();
            rhs = "({ OvValue " + tmp_v + " = " + ex(s->values[i]) + "; (" + tmp_v + ").f; })";
          } else {
            rhs = ex(s->values[i]);
          }
        } else {
          if (pi) rhs = "0LL";
          else if (pf) rhs = "0.0";
          else rhs = "ov_nil()";
        }
        // With an active string builder, `acc = ""` is just the builder's
        // initial (empty) state -- do not rebind the result variable.
        bool is_acc_init =
            sb_.active && name == sb_.var && i < s->values.size() &&
            s->values[i] && s->values[i]->kind == ExprKind::StringLit &&
            s->values[i]->sval.empty();
        if (!is_acc_init) {
          // `xs = []` where sema proved the collection is built only from
          // ints: allocate an Int64Array instead of a boxed list. Everything
          // else about the variable is unchanged -- it is still one OvValue,
          // so len/index/iteration/passing it on all keep working, and only
          // push/pop take the raw-int path.
          const bool is_int_coll =
              i < s->values.size() && s->values[i] &&
              s->values[i]->kind == ExprKind::ListLit &&
              s->values[i]->items.empty() && !pi && !pf &&
              sema_.is_int_collection(cur_fn_, name);
          if (is_int_coll) rhs = "ov_i64a_val(ov_i64a_new())";
          line(bind(name, rhs, pi, pf) + ";");
          // After the bind, so the C name is known: the push and pop paths
          // look these up by C name, never by the Ovyth name.
          if (is_int_coll) {
            if (std::string* c = lookup(name)) int_coll_.insert(*c);
          }
        }
      }
      // Handle tuple unpacking: `a, b = [x, y]` - each name gets the list element at that index.
      // This must happen AFTER the individual bindings so the list value is evaluated first.
      if (n > 1 && s->values.size() == 1) {
        // The list value was already evaluated as the RHS for the first variable
        // We need to re-evaluate it or capture it. For simplicity, re-evaluate.
        std::string list_expr = ex(s->values[0]);
        for (size_t i = 0; i < n; i++) {
          const std::string& name = s->names[i];
          bool pi = is_proven_int_any_scope(name);
          bool pf = is_proven_float_any_scope(name);
          if (pi && pf) { pi = pf = false; }
          std::string elem_rhs;
          if (pi) {
            elem_rhs = "((ov_tagof(" + list_expr + ") == OV_LIST && " + std::to_string(i) +
                       " < " + list_expr + ".list->len) ? (" + list_expr + ".list->items[" +
                       std::to_string(i) + "]).i : "
                       "(ov_tagof(" + list_expr + ") == OV_I64A && " + std::to_string(i) +
                       " < " + list_expr + ".i64a->len) ? " + list_expr + ".i64a->data[" +
                       std::to_string(i) + "] : 0LL)";
          } else if (pf) {
            elem_rhs = "((ov_tagof(" + list_expr + ") == OV_LIST && " + std::to_string(i) +
                       " < " + list_expr + ".list->len) ? (" + list_expr + ".list->items[" +
                       std::to_string(i) + "]).f : "
                       "(ov_tagof(" + list_expr + ") == OV_F64A && " + std::to_string(i) +
                       " < " + list_expr + ".f64a->len) ? " + list_expr + ".f64a->data[" +
                       std::to_string(i) + "] : 0.0)";
          } else {
            elem_rhs = "(ov_tagof(" + list_expr + ") == OV_LIST && " + std::to_string(i) +
                       " < " + list_expr + ".list->len) ? ov_list_get(" + list_expr + ".list, " +
                       std::to_string(i) + ") : "
                       "(ov_tagof(" + list_expr + ") == OV_I64A && " + std::to_string(i) +
                       " < " + list_expr + ".i64a->len) ? ov_int(" + list_expr + ".i64a->data[" +
                       std::to_string(i) + "]) : "
                       "(ov_tagof(" + list_expr + ") == OV_F64A && " + std::to_string(i) +
                       " < " + list_expr + ".f64a->len) ? ov_float(" + list_expr + ".f64a->data[" +
                       std::to_string(i) + "]) : "
                       "(ov_tagof(" + list_expr + ") == OV_STRA && " + std::to_string(i) +
                       " < " + list_expr + ".stra->len) ? ov_str(" + list_expr + ".stra->data[" +
                       std::to_string(i) + "]) : ov_nil()";
          }
          line(bind(name, elem_rhs, pi, pf) + ";");
        }
      }
      // Remember `x = ""` so a following loop can recognise string building.
      if (n == 1 && s->values[0] && s->values[0]->kind == ExprKind::StringLit &&
          s->values[0]->sval.empty())
        last_empty_str_ = s->names[0];
      return;
    }

    case StmtKind::Assign: {
      // If target is a proven int/float variable, generate raw assignment
      if (s->op == Tok::ASSIGN && s->expr && s->expr->kind == ExprKind::Identifier) {
        const std::string& name = s->expr->name;
        bool pi = is_proven_int_any_scope(name);
        bool pf = is_proven_float_any_scope(name);
        if (pi || pf) {
          std::string rhs;
          if (pi && is_int_expr(s->expr2)) {
            rhs = ex_int(s->expr2);
          } else if (pf && is_float_expr(s->expr2)) {
            rhs = ex_float(s->expr2);
          } else if (pi) {
            // RHS is not a pure int expr, but target is proven int:
            // evaluate RHS as OvValue into a temp, then extract .i
            std::string tmp_v = fresh();
            line("OvValue " + tmp_v + " = " + ex(s->expr2) + ";");
            rhs = "((" + tmp_v + ").i)";
          } else if (pf) {
            std::string tmp_v = fresh();
            line("OvValue " + tmp_v + " = " + ex(s->expr2) + ";");
            rhs = "((" + tmp_v + ").f)";
          } else {
            rhs = ex(s->expr2);
          }
          line(bind(name, rhs, pi, pf) + ";");
          return;
        }
      }

      std::string v = fresh();

      // String-builder accumulation (spec 12).
      if (s->kind == StmtKind::Assign && s->expr &&
          s->expr->kind == ExprKind::Identifier) {
        if (try_sb_append(s->expr->name, s->expr2, s->op)) return;
      }

      if (s->op == Tok::ASSIGN) {
        line("OvValue " + v + " = " + ex(s->expr2) + ";");
        line(emit_assign(s->expr, v) + ";");
        return;
      }
      std::string cur = fresh();
      std::string op;
      const std::string V = v, C = cur;
      switch (s->op) {
        case Tok::PLUS_EQUAL:    op = "ov_add(" + C + ", " + V + ")"; break;
        case Tok::MINUS_EQUAL:   op = "ov_sub(" + C + ", " + V + ")"; break;
        case Tok::STAR_EQUAL:    op = "ov_mul(" + C + ", " + V + ")"; break;
        case Tok::SLASH_EQUAL:   op = "ov_div(" + C + ", " + V + ")"; break;
        case Tok::PERCENT_EQUAL: op = "ov_mod(" + C + ", " + V + ")"; break;
        default: op = C; fail("unsupported compound assignment");
      }
      line("OvValue " + v + " = " + ex(s->expr2) + ";");
      line("OvValue " + cur + " = " + ex(s->expr) + ";");
      line(emit_assign(s->expr, op) + ";");
      return;
    }

    case StmtKind::FuncDecl:
      // Hoisted: the definition was emitted at module scope.
      return;

    case StmtKind::Return:
      fn_has_return_ = true;
      // A raw-returning function assigns int64_t. The planner proved every
      // own-level return statically int, so the live check is a guard
      // against an analysis bug: failing here is a compile error, never a
      // silently truncated value.
      // A return from inside a `try` leaves its block without running the
      // cleanup: the jump out of the block skips the push's matching pop and
      // the restore of the roots it registered, and the frame then returns
      // with the runtime's unwind state and root list describing a scope that
      // no longer exists. Clean up on the way out, innermost first.
      emit_try_cleanups();
      if (!cur_fn_.empty() && fn_ret_is_raw(cur_fn_)) {
        if (!s->expr || !is_int_expr(s->expr)) {
          fail("'" + cur_fn_ + "' returns a raw int, but this return is not a "
               "provable int");
          return;
        }
        line("_ret = " + ex_int(s->expr) + "; goto " + ret_label_ + ";");
        return;
      }
      line("_ret = " + (s->expr ? ex(s->expr) : "ov_nil()") + "; goto " + ret_label_ + ";");
      return;

    case StmtKind::If: {
      // A comparison of two provable int expressions can be the C condition
      // itself. The general path builds an OvValue and switches on its tag
      // on every evaluation -- which, in a tight recursive function, is most
      // of the per-call cost.
      if (std::string rc = raw_int_condition(s->expr); !rc.empty()) {
        line("if (" + rc + ") {");
        indent_++;
        block(s->body, true);
        indent_--;
        if (!s->else_body.empty()) {
          line("} else {");
          indent_++;
          block(s->else_body, true);
          indent_--;
        }
        line("}");
        return;
      }
      std::string c = fresh();
      line("{ OvValue " + c + " = " + ex(s->expr) + ";");
      indent_++;
      if (s->else_body.empty()) {
        line("if (ov_truthy(" + c + ")) {");
        indent_++;
        block(s->body, true);
        indent_--;
        line("}");
      } else {
        line("if (ov_truthy(" + c + ")) {");
        indent_++;
        block(s->body, true);
        indent_--;
        line("} else {");
        indent_++;
        block(s->else_body, true);
        indent_--;
        line("}");
      }
      indent_--;
      line("}");
      return;
    }

    case StmtKind::While: {
      LoopLabels L{lbl("Lbrk"), lbl("Lcnt")};
      std::string top = lbl("Ltop");
      loops_.push_back(L);

      // String-builder accumulation (spec 12): `acc = ""` then `acc += x`
      // is quadratic; a builder makes it linear.
      std::string acc;
      bool sb = accumulates_string(last_empty_str_, s->body, &acc);
      // Only the outermost accumulating loop may claim the builder. An inner
      // loop appending to the same variable must keep appending to the builder
      // the outer one opened, otherwise its text is stranded in a builder that
      // is finished (and discarded) inside the outer body.
      if (sb && sb_.active && sb_.var == acc) sb = false;
      BuilderInfo saved_sb = sb_;
      if (sb) {
        sb_.active = true;
        sb_.var = acc;
        sb_.cvar = fresh();
        // Bind the Ovyth name to the *finished* string, materialised after the
        // loop; inside, appends go to the builder.
        std::string res = "__ov_sb_result_" + sb_.cvar;
        scopes_.back()[acc] = res;
        cur_roots_.push_back(res);
        inline_roots_.insert(res);
        line("OvValue " + res + " = ov_nil();");
        line("ov_gc_register_root(&" + res + ");");
        line("OvStrBuilder* " + sb_.cvar + " = ov_sb_new();");
      }

      bool uses_continue = body_uses_continue(s->body);
      line(top + ": ;");
      // A comparison of provable int expressions tests directly in C (see
      // the If case): the boxed test costs an OvValue and a tag switch per
      // iteration, which is what a 20M-iteration loop pays for.
      if (std::string rc = raw_int_condition(s->expr); !rc.empty()) {
        line("if (!(" + rc + ")) goto " + L.brk + ";");
      } else {
        std::string c = fresh();
        line("{ OvValue " + c + " = " + ex(s->expr) + ";");
        indent_++;
        line("if (!ov_truthy(" + c + ")) goto " + L.brk + "; }");
        indent_--;
      }
      loop_depth_++;
      std::string saved_seed = last_empty_str_;
      last_empty_str_.clear();   // only the declaring loop may own the builder
      block(s->body, true);
      last_empty_str_ = saved_seed;
      loop_depth_--;
      // `continue` jumps here, then falls into the next test. The label is
      // only emitted when the body actually uses `continue`, so a loop without
      // one produces no unused-label warning.
      if (uses_continue) line(L.cont + ": ;");
      line("goto " + top + ";");
      line(L.brk + ": ;");

      last_empty_str_.clear();
      if (sb) {
        std::string res = scopes_.back()[acc];
        line(res + " = ov_str(ov_sb_finish(" + sb_.cvar + "));");
        sb_ = saved_sb;
      }
      loops_.pop_back();
      return;
    }

    case StmtKind::Block:
      line("{");
      indent_++;
      block(s->body, true);
      indent_--;
      line("}");
      return;

    case StmtKind::Break:
      if (loops_.empty()) { fail("break outside a loop"); return; }
      emit_try_cleanups();
      line("goto " + loops_.back().brk + ";");
      return;

    case StmtKind::Continue:
      if (loops_.empty()) { fail("continue outside a loop"); return; }
      emit_try_cleanups();
      line("goto " + loops_.back().cont + ";");
      return;

    case StmtKind::Throw:
      line("ov_throw_value(" + (s->expr ? ex(s->expr) : std::string("ov_str(ov_str_new(\"thrown\", 6))")) + ");");
      return;

    case StmtKind::Try: {
      // The exact panic idiom documented in ovrt.h.
      std::string tb = lbl("Ltry");
      try_depth_++;
      line("{ jmp_buf* _jb = ov_try_push();");
      // A panic unwinds past every root registered since
      // this point -- the longjmp bypasses their restores
      // -- so drop them on the catch path: the slots point
      // into dead frames and the next collection would
      // scan them as roots.
      std::string trm = fresh();
      line("size_t " + trm + " = ov_gc_roots_mark();");
      // Every path that leaves the try body must run its cleanup: a `return`,
      // `break` or `continue` out of the body used to jump straight past
      // ov_try_pop() and the roots restore, and left the unwind state of the
      // frame inconsistent with what the runtime believes it is. The exit
      // statements emit the pending cleanups below before they jump.
      try_cleanups_.push_back({tb, "  ov_try_pop(); ov_gc_roots_restore(" + trm + ");"});
      indent_++;
      line("if (setjmp(*_jb) == 0) {");
      indent_++;
      block(s->body, true);
      indent_--;
      line("  ov_try_pop(); goto " + tb + "; }");
      line("  ov_try_pop();");
      line("  ov_gc_roots_restore(" + trm + ");");
      if (!s->catch_var.empty()) line("  " + bind(s->catch_var, "ov_caught") + ";");
      else line("  (void)ov_caught;");
      block(s->else_body, true);
      line(tb + ": ;");
      indent_--;
      line("}");
      try_depth_--;
      try_cleanups_.pop_back();
      return;
    }

    case StmtKind::Debug:
      line("ov_debug_note(" + quote_c(s->pos.file) + ", " +
           std::to_string(s->pos.line) + ");");
      return;

    case StmtKind::For:
      emit_for(s);
      return;

    case StmtKind::Import:
      return;  // consumed by the loader before codegen
  }
}

void Gen::emit_for(const Stmt* s) {
  // `for v in xs` over a list, a string's characters, a map's keys, or
  // specialized arrays. Over OvInt64Array / OvFloat64Array the loop
  // variable can live in a raw C numeric local -- no per-iteration
  // boxing -- when the body keeps it raw (pre-scan above): reads box
  // on demand through ex(), arithmetic stays in C registers.
  std::string it = fresh(), idx = fresh(), seq = fresh(), n = fresh();
  std::string var = s->iter_vars.empty() ? std::string("ov_unused") : ident(s->iter_vars[0], tmp_++);
  LoopLabels L{lbl("Lbrk"), lbl("Lcont")};
  std::string top = lbl("Ltop");

  const std::string I = it, S = seq, N = n, X = idx;
  line("{ OvValue " + it + " = " + ex(s->expr) + ";");
  indent_++;
  // The iterable, and the list derived from it (a string's
  // characters, a map's keys), are rooted for the whole loop:
  // the body may allocate, and both are OvLists the collector
  // can sweep. (Specialized arrays are not GC-managed, so an
  // i64a/f64a/stra iterable is safe without this.)
  std::string held = fresh(), rm = fresh();
  const std::string HELD = held, RM = rm;
  line("size_t " + rm + " = ov_gc_roots_mark();");
  line("ov_gc_register_root(&" + it + ");");
  line("OvValue " + held + " = ov_nil();");
  line("ov_gc_register_root(&" + held + ");");
  // Handle all iterable types: string, list, map, int64array, float64array, stringarray
  line("OvList* " + seq + " = NULL;");
  line("OvInt64Array* " + seq + "_i64a = NULL;");
  line("OvFloat64Array* " + seq + "_f64a = NULL;");
  line("OvStringArray* " + seq + "_stra = NULL;");
  line("if (ov_tagof(" + I + ") == OV_STRING) { " + HELD + " = ov_str_chars(" + I + ".str); " + seq + " = " + HELD + ".list; } "
       "else if (ov_tagof(" + I + ") == OV_MAP) { " + HELD + " = ov_map_keys(" + I + ".map); " + seq + " = " + HELD + ".list; } "
       "else if (ov_tagof(" + I + ") == OV_LIST) { " + seq + " = " + I + ".list; } "
       "else if (ov_tagof(" + I + ") == OV_I64A) { " + seq + "_i64a = " + I + ".i64a; } "
       "else if (ov_tagof(" + I + ") == OV_F64A) { " + seq + "_f64a = " + I + ".f64a; } "
       "else if (ov_tagof(" + I + ") == OV_STRA) { " + seq + "_stra = " + I + ".stra; } ");
  line("int64_t " + n + " = 0;");
  line("if (" + seq + ") " + n + " = " + seq + "->len;");
  line("else if (" + seq + "_i64a) " + n + " = " + seq + "_i64a->len;");
  line("else if (" + seq + "_f64a) " + n + " = " + seq + "_f64a->len;");
  line("else if (" + seq + "_stra) " + n + " = " + seq + "_stra->len;");
  bool uses_continue = body_uses_continue(s->body);
  bool uses_break = body_uses_break(s->body);

  // The boxed loop shape: one OvValue slot, the element extracted per
  // iterable type. Used by the generic path and by the fallback branch
  // of the raw path (list / string / map / string array).
  auto emit_boxed_loop = [&]() {
    line("for (int64_t " + X + " = 0; " + X + " < " + N + "; " + X + "++) {");
    indent_++;
    push_scope();
    scopes_.back()[s->iter_vars.empty() ? std::string("__unused") : s->iter_vars[0]] = var;
    cur_roots_.push_back(var);
    // Extract element based on iterable type
    // The final else is unreachable -- one of the four shapes always matches,
    // because the loop was entered through it -- but without it the variable
    // is formally uninitialized on some path, and clang says so
    // (-Wsometimes-uninitialized) on every program whose iterable type it
    // cannot narrow.
    line("OvValue " + var + ";");
    line("if (" + seq + ") " + var + " = " + seq + "->items[" + X + "];");
    line("else if (" + seq + "_i64a) " + var + " = ov_int(" + seq + "_i64a->data[" + X + "]);");
    line("else if (" + seq + "_f64a) " + var + " = ov_float(" + seq + "_f64a->data[" + X + "]);");
    line("else if (" + seq + "_stra) " + var + " = ov_str(" + seq + "_stra->data[" + X + "]);");
    loops_.push_back(L);
    block(s->body, false);
    loops_.pop_back();
    if (uses_continue) line(L.cont + ": ;");
    pop_scope();
    indent_--;
    line("}");
    // Only emitted when the body actually jumps to it -- see the note on
    // the continue label in the While case.
    if (uses_break) line(L.brk + ": ;");
  };

  // Raw path: specialized arrays, a single loop variable, a body that
  // keeps it raw (no writes, no shadowing, no closure capture), and a
  // name that is not already a closure-shared box (bind() would store
  // through the box instead of declaring the local).
  //
  // The loop variable always gets a FRESH C local, never an outer slot:
  // `for n in ...` inside a function with a boxed parameter `n` must not
  // assign an int64 into the parameter's OvValue (that shape is a clang
  // type error), and a loop variable that shadows a local must be its own
  // local anyway -- the interpreter scopes the loop variable to the body.
  if (s->iter_vars.size() == 1 && !box_writes_.count(s->iter_vars[0]) &&
      block_keeps_loop_var_raw(s->body, s->iter_vars[0])) {
    line("if (" + seq + "_i64a) {");
    indent_++;
    LoopLabels BL{lbl("Lbrk"), lbl("Lcont")};
    line("for (int64_t " + X + " = 0; " + X + " < " + seq + "_i64a->len; " + X + "++) {");
    indent_++;
    push_scope();
    {
      std::string rv = ident(s->iter_vars[0], tmp_++);
      scopes_.back()[s->iter_vars[0]] = rv;
      proven_ints_.insert(rv);
      cur_roots_.push_back("@int:" + rv);
      line("int64_t " + rv + " = " + seq + "_i64a->data[" + X + "];");
    }
    loops_.push_back(BL);
    block(s->body, false);
    loops_.pop_back();
    if (uses_continue) line(BL.cont + ": ;");
    pop_scope();
    indent_--;
    line("}");
    if (uses_break) line(BL.brk + ": ;");
    indent_--;
    line("} else if (" + seq + "_f64a) {");
    indent_++;
    LoopLabels FL{lbl("Lbrk"), lbl("Lcont")};
    line("for (int64_t " + X + " = 0; " + X + " < " + seq + "_f64a->len; " + X + "++) {");
    indent_++;
    push_scope();
    {
      std::string rv = ident(s->iter_vars[0], tmp_++);
      scopes_.back()[s->iter_vars[0]] = rv;
      proven_floats_.insert(rv);
      cur_roots_.push_back("@float:" + rv);
      line("double " + rv + " = " + seq + "_f64a->data[" + X + "];");
    }
    loops_.push_back(FL);
    block(s->body, false);
    loops_.pop_back();
    if (uses_continue) line(FL.cont + ": ;");
    pop_scope();
    indent_--;
    line("}");
    if (uses_break) line(FL.brk + ": ;");
    indent_--;
    line("} else {");
    indent_++;
    emit_boxed_loop();
    indent_--;
    line("}");
    indent_--;
  } else {
    emit_boxed_loop();
    indent_--;
  }
  line("ov_gc_roots_restore(" + RM + ");");
  line("}");
  (void)top;
}

// ---------------------------------------------------------------- functions
void Gen::collect_functions(const StmtList& body) {
  for (const Stmt* s : body)
    if (s && s->kind == StmtKind::FuncDecl && !userfns_.count(s->name))
      userfns_[s->name] = s;
}

// Statically-int test used only by the boundary planner. Unlike
// is_int_expr() it cannot consult emission scopes -- no body has been
// lowered yet -- so an identifier is an int only when it is the scanned
// function's own parameter (assumed while that function plans itself) or a
// global sema proved int.
static const Expr* arg_for_param(const Expr* call, const Stmt* decl, size_t idx) {
  const Expr* got = nullptr;
  if (idx < call->args.size()) got = call->args[idx];
  for (const auto& na : call->named_args)
    if (decl->params[idx].name == na.name) got = na.value;
  return got;
}

bool Gen::pre_int(const Expr* e, const ScanCtx& ctx) const {
  if (!e) return false;
  switch (e->kind) {
    case ExprKind::IntLit:
      return true;
    case ExprKind::Unary:
      return e->op == Tok::MINUS && pre_int(e->a, ctx);
    case ExprKind::Binary:
      switch (e->op) {
        case Tok::PLUS: case Tok::MINUS: case Tok::STAR: case Tok::PERCENT:
        case Tok::AMP: case Tok::PIPE: case Tok::CARET: case Tok::SHL: case Tok::SHR:
          return pre_int(e->a, ctx) && pre_int(e->b, ctx);
        default:
          return false;
      }
    case ExprKind::Identifier: {
      if (ctx.enclosing) {
        auto it = userfns_.find(*ctx.enclosing);
        if (it != userfns_.end()) {
          const Stmt* fn = it->second;
          for (size_t i = 0; i < fn->params.size(); i++)
            if (fn->params[i].name == e->name) {
              // A self-call's argument may assume the parameter under test;
              // everything else must trust the claim computed so far.
              if (ctx.planning && *ctx.planning == *ctx.enclosing && ctx.for_call_arg)
                return true;
              auto pit = fn_plans_.find(*ctx.enclosing);
              return pit != fn_plans_.end() && i < pit->second.raw_param.size() &&
                     pit->second.raw_param[i];
            }
        }
      }
      auto git = sema_.proven_types.find(e->name);
      return git != sema_.proven_types.end() && git->second.is_proven_int;
    }
    case ExprKind::Call: {
      const Expr* callee = e->a;
      if (!callee || callee->kind != ExprKind::Identifier) return false;
      const std::string& n = callee->name;
      if (!userfns_.count(n) || bound_names_.count(n)) return false;
      if (ctx.planning && *ctx.planning == n) return true;  // self-recursion
      auto pit = fn_plans_.find(n);
      return pit != fn_plans_.end() && pit->second.raw_ret;
    }
    default:
      return false;
  }
}

void Gen::plan_function_boundaries() {
  struct Static {
    bool has_closure = false;
    bool variadic = false;
    bool tail_return = false;
    std::set<std::string> assigned;
    std::set<std::string> bound;
    std::vector<const Expr*> returns;
  };
  std::unordered_map<std::string, Static> info;

  for (const Stmt* s : prog_.statements) {
    if (!s || s->kind != StmtKind::FuncDecl) continue;
    Static st;
    st.variadic = s->is_variadic;
    bool cl = false;
    block_has_closure(s->body, cl);
    st.has_closure = cl;
    collect_block_assignments(s->body, st.assigned);
    collect_block_bindings(s->body, st.bound);
    collect_own_returns(s->body, st.returns);
    st.tail_return = block_tail_returns(s->body);
    if (!userfns_.count(s->name)) userfns_[s->name] = s;
    info[s->name] = std::move(st);
  }

  // A callee name bound anywhere as a local is a call through a value, not
  // a direct call, so it never constrains a signature.
  bound_names_ = {};
  {
    std::set<std::string> b;
    collect_block_bindings(prog_.statements, b);
    for (const auto& kv : userfns_) b.erase(kv.first);
    bound_names_ = std::move(b);
  }

  struct CallSite {
    const std::string* enclosing;
    const Expr* call;
  };
  std::unordered_map<std::string, std::vector<CallSite>> sites;
  auto record = [&](const Expr* c, const std::string* enclosing) {
    const Expr* callee = c->a;
    if (!callee || callee->kind != ExprKind::Identifier) return;
    if (!userfns_.count(callee->name) || bound_names_.count(callee->name)) return;
    sites[callee->name].push_back({enclosing, c});
  };
  for (const Stmt* s : prog_.statements) {
    if (s && s->kind == StmtKind::FuncDecl) {
      walk_block_calls(s->body, [&](const Expr* c) { record(c, &s->name); });
    } else {
      walk_stmt_calls(s, [&](const Expr* c) { record(c, nullptr); });
    }
  }

  for (const auto& kv : info) {
    FnPlan plan;
    plan.raw_param.assign(userfns_[kv.first]->params.size(), false);
    fn_plans_[kv.first] = std::move(plan);
  }

  // Claims only ever grow: a call site's argument becomes provable when a
  // callee's return is claimed, and a parameter claim enables the caller's
  // argument. Start from nothing and iterate to the fixed point.
  bool changed = true;
  int rounds = 0;
  const int cap = (int)info.size() + 2;
  while (changed && rounds++ <= cap) {
    changed = false;
    for (const auto& kv : info) {
      const std::string& name = kv.first;
      const Static& st = kv.second;
      const Stmt* decl = userfns_[name];
      FnPlan& plan = fn_plans_[name];
      std::vector<bool> cand(decl->params.size(), true);
      for (size_t i = 0; i < decl->params.size(); i++) {
        const std::string& p = decl->params[i].name;
        if (st.assigned.count(p) || st.bound.count(p)) { cand[i] = false; continue; }
        // The body must also only *read* the parameter as a value; a member,
        // index, slice, self-call or iteration needs the tagged form.
        if (block_uses_param_as_value(decl->body, p)) { cand[i] = false; continue; }
        for (const CallSite& cs : sites[name]) {
          ScanCtx ctx;
          ctx.enclosing = cs.enclosing;
          // Only a self-call's argument may assume the parameter under test;
          // an argument inside another function must trust that function's
          // computed claim, or a boxed caller would silently force a raw
          // signature on its callee.
          if (cs.enclosing && *cs.enclosing == name) {
            ctx.planning = &name;
            ctx.for_call_arg = true;
          }
          const Expr* arg = arg_for_param(cs.call, decl, i);
          if (!arg || !pre_int(arg, ctx)) { cand[i] = false; break; }
        }
      }
      bool ret = false;
      if (!st.has_closure && !st.variadic && st.tail_return && !st.returns.empty()) {
        ScanCtx ctx;
        ctx.enclosing = &name;
        ctx.planning = &name;
        ret = true;
        for (const Expr* r : st.returns)
          if (!r || !pre_int(r, ctx)) { ret = false; break; }
      }
      if (st.has_closure || st.variadic) {
        for (size_t i = 0; i < cand.size(); i++) cand[i] = false;
        ret = false;
      }
      if (ret != plan.raw_ret) { plan.raw_ret = ret; changed = true; }
      for (size_t i = 0; i < cand.size(); i++)
        if (cand[i] != plan.raw_param[i]) { plan.raw_param[i] = cand[i]; changed = true; }
    }
  }

  // Non-convergence would mean oscillating claims, which the rules above
  // cannot produce; if it ever happened, boxed is the safe answer.
  if (changed) {
    for (auto& kv : fn_plans_) {
      kv.second.raw_ret = false;
      for (size_t i = 0; i < kv.second.raw_param.size(); i++)
        kv.second.raw_param[i] = false;
    }
  }
}

bool Gen::fn_ret_is_raw(const std::string& fn) const {
  auto it = fn_plans_.find(fn);
  return it != fn_plans_.end() && it->second.raw_ret;
}

bool Gen::fn_param_is_raw(const std::string& fn, size_t idx) const {
  auto it = fn_plans_.find(fn);
  return it != fn_plans_.end() && idx < it->second.raw_param.size() &&
         it->second.raw_param[idx];
}

bool Gen::call_returns_raw_int(const Expr* e) const {
  if (!e || e->kind != ExprKind::Call) return false;
  const Expr* callee = e->a;
  if (!callee || callee->kind != ExprKind::Identifier) return false;
  if (lookup(callee->name)) return false;  // a local shadows the function: value call
  return fn_ret_is_raw(callee->name);
}

std::string Gen::raw_arg_or_fail(const Expr* a, const std::string& fn, size_t idx) {
  if (is_int_expr(a)) return ex_int(a);
  fail("'" + fn + "' parameter " + std::to_string(idx + 1) +
       " crosses the call boundary as a raw int, but this argument is not a "
       "provable int");
  return "0LL";
}

// Leaving a try body by any path other than its own end -- `return`,
// `break`, `continue` -- must still run the try's cleanup: the runtime tracks
// the unwind state by push/pop, and skipping the pop leaves it describing a
// frame that has already gone.
void Gen::emit_try_cleanups() {
  for (auto it = try_cleanups_.rbegin(); it != try_cleanups_.rend(); ++it)
    line(it->second);
}

void Gen::emit_function(const Stmt* s) {
  auto saved_scopes = scopes_;
  auto saved_ret = ret_label_;
  auto saved_fn = cur_fn_;
  auto saved_box_of = box_of_;
  auto saved_box_writes = box_writes_;
  auto saved_ints = proven_ints_;
  auto saved_floats = proven_floats_;
  box_of_.clear();
  box_writes_.clear();
  // Sema keys a local's proven type by the enclosing function, and the
  // planner keys a parameter's raw claim by function too. Both are reset
  // here for the same reason: a parameter's C name is unqualified
  // (`v_n_0` in every function), so one function's claim must never be
  // visible while another's body is lowered.
  proven_ints_.clear();
  proven_floats_.clear();
  std::string saved_out;
  saved_out.swap(out);
  int saved_indent = indent_;
  indent_ = 0;
  cur_fn_ = s->name;
  ret_label_ = lbl("Lret");

  push_scope();
  for (const auto& p : s->params) scopes_.back()[p.name] = ident(p.name, 0);
  // Parameters the planner proved raw cross the boundary as int64_t. Reads
  // box at their use sites through the same proven-local machinery loop
  // locals use, so no expression site changes.
  for (size_t i = 0; i < s->params.size(); i++)
    if (fn_param_is_raw(s->name, i)) proven_ints_.insert(ident(s->params[i].name, 0));

  // Two passes. The body is lowered first into a scratch buffer so every local
  // it introduces is known; only then is the entry prologue written, declaring
  // those locals and registering each as a GC root. Doing it in this order is
  // what makes the result safe: the collector can fire on any allocation, so a
  // live local must be rooted before the first instruction that can allocate.
  std::vector<std::string> outer_roots;
  outer_roots.swap(cur_roots_);
  std::unordered_set<std::string> outer_int_coll;
  outer_int_coll.swap(int_coll_);
  inline_roots_.clear();

  std::string body_buf;
  body_buf.swap(out);
  indent_ = 1;
  fn_has_return_ = false;
  block(s->body, false);
  // The root mark/restore pair exists to pop this frame's registrations on
  // return -- without it every call would leave its locals in the root
  // array forever. A function that registers nothing owns no root: raw
  // int64 parameters are not heap references, and no local was rooted.
  // Skipping the pair there removes two calls and a global store/load per
  // invocation, which measurement showed is the entire remaining gap
  // between `fib` and the same function in hand-written C.
  bool fn_needs_roots = !cur_roots_.empty();
  for (size_t i = 0; i < s->params.size(); i++)
    if (!fn_param_is_raw(s->name, i)) fn_needs_roots = true;
  if (fn_has_return_) line(ret_label_ + ": ;");
  if (fn_needs_roots) line("  ov_gc_roots_restore(_roots_mark);");
  line("  return _ret; }");
  body_buf.swap(out);
  indent_ = 0;

  nl();
  line(fn_signature(s) + " {");
  indent_++;
  line(fn_ret_is_raw(s->name) ? "int64_t _ret = 0;" : "OvValue _ret = ov_nil();");
  if (fn_needs_roots) line("size_t _roots_mark = ov_gc_roots_mark();");
  emit_root_prologue();
  // Parameters live in the C signature, so the prologue has no declaration to
  // hang them from and they are registered here instead. A parameter is a heap
  // reference like any other local: the collector fires on any allocation, so a
  // function that allocates in a loop over a parameter -- or holds one a caller
  // built in the same expression -- was reading freed memory otherwise. Raw
  // int64 parameters are not heap references and are deliberately absent.
  {
    std::string regs;
    for (size_t i = 0; i < s->params.size(); i++)
      if (!fn_param_is_raw(s->name, i))
        regs += "ov_gc_register_root(&" + ident(s->params[i].name, 0) + "); ";
    if (!regs.empty()) line(regs);
  }
  cur_roots_.swap(outer_roots);
  int_coll_.swap(outer_int_coll);   // done: locals are rooted by the prologue
  out += body_buf;
  pop_scope();

  std::string body = out;
  out.swap(saved_out);
  indent_ = saved_indent;
  scopes_ = saved_scopes;
  ret_label_ = saved_ret;
  cur_fn_ = saved_fn;
  box_of_ = saved_box_of;
  box_writes_ = saved_box_writes;
  proven_ints_ = saved_ints;
  proven_floats_ = saved_floats;
  pending_fns_.push_back(body);
}

// The emitted signature. Parameters the planner proved raw are int64_t and
// the return is int64_t when every own-level return is provably int; both
// the forward declarations and the definition come from here so they cannot
// drift.
std::string Gen::fn_signature(const Stmt* s) const {
  std::string sig = fn_ret_is_raw(s->name) ? "static int64_t " : "static OvValue ";
  sig += funcname(s->name) + "(";
  if (s->params.empty()) {
    sig += "void";
  } else {
    for (size_t i = 0; i < s->params.size(); i++) {
      if (i) sig += ", ";
      sig += fn_param_is_raw(s->name, i) ? "int64_t " : "OvValue ";
      sig += ident(s->params[i].name, 0);
    }
  }
  sig += ")";
  return sig;
}

// ------------------------------------------------------------------- module
std::string Gen::run() {
  collect_functions(prog_.statements);
  init_proven_types();
  plan_function_boundaries();

  // ---- header ----
  out +=
      "/* Generated by ovc. Do not edit. */\n"
      "#include \"ovrt.h\"\n"
      "#include \"ovrt_helpers.h\"\n"
      "#include <setjmp.h>\n"
      "#include <math.h>\n"
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "#include <string.h>\n"
      "#include <time.h>\n\n";

  // ---- user functions ----
  {
    // Forward declarations first, so a function may call any other.
    for (const Stmt* s : prog_.statements) {
      if (!s || s->kind != StmtKind::FuncDecl) continue;
      line(fn_signature(s) + ";");
    }
    nl();

    // Lower each user-function body into a scratch buffer. This DISCOVERS the
    // anonymous closure functions they create -- their C names land in
    // anon_names_, their bodies in anon_defs_ -- but writes nothing to `out`
    // yet, so the forward declarations below can precede every reference.
    for (const Stmt* s : prog_.statements)
      if (s && s->kind == StmtKind::FuncDecl) emit_function(s);

    // Forward-declare every anon function BEFORE any body that references it.
    // A closure created inside a function body references its ov_anon_N symbol
    // from within that body, so the declaration must come first -- otherwise
    // clang rejects the generated C with `use of undeclared identifier
    // 'ov_anon_N'`. A nested closure's creation site lives inside its enclosing
    // closure's body, so one declaration shape covers all of them. Signatures
    // are ABI-uniform.
    {
      std::set<std::string> seen;
      for (const auto& nm : anon_names_) {
        if (seen.insert(nm).second)
          line("static OvValue " + nm +
               "(struct OvFunc* _fn, OvValue* _argv, int _argc);");
      }
    }

    // Now emit the user-function bodies, then the closure bodies they created.
    for (const auto& f : pending_fns_) out += f;
    pending_fns_.clear();
    while (!anon_defs_.empty()) {
      out += anon_defs_.back();
      anon_defs_.pop_back();
    }
    anon_names_.clear();
  }

  // ---- program body ----
  {
    std::vector<std::string> outer_roots;
    outer_roots.swap(cur_roots_);
    inline_roots_.clear();
    std::string saved;
    saved.swap(out);
    push_scope();
    ret_label_ = lbl("Lmain");
    indent_ = 1;
    fn_has_return_ = false;
    block(prog_.statements, false);
    if (fn_has_return_) line(ret_label_ + ": ;");
    line("  ov_gc_roots_restore(_roots_mark);");
    line("  return _ret; }");
    std::string body = out;
    out.swap(saved);
    pop_scope();
    // Same forward-declaration discipline as the functions phase: nested
    // closures reference inner bodies defined later in this same drain.
    {
      std::set<std::string> seen;
      for (const auto& nm : anon_names_) {
        if (seen.insert(nm).second)
          line("static OvValue " + nm +
               "(struct OvFunc* _fn, OvValue* _argv, int _argc);");
      }
    }
    while (!anon_defs_.empty()) { out += anon_defs_.back(); anon_defs_.pop_back(); }
    anon_names_.clear();

    line("static OvValue ov_main(void) {");
    indent_++;
  line("OvValue _ret = ov_nil();");
  line("size_t _roots_mark = ov_gc_roots_mark();");
  emit_root_prologue();

    cur_roots_.swap(outer_roots);
    out += body;
    indent_ = 0;
  }

  // ---- entry point ----
  line("");
  line("int main(int argc, char** argv) {");
  line("  ov_runtime_init(argc, argv);");
  line("  int _code = 0;");
  line("  jmp_buf* _jb = ov_try_push();");
  // A panic or `exit(code)` longjmps out of ov_main, which skips ov_main's
  // own epilogue -- so every root it registered still points into its frame,
  // and that frame is dead. ov_runtime_shutdown() collects, and a collector
  // that walks a root into a dead frame dereferences whatever now occupies
  // it: a segfault whose location has nothing to do with the cause. Restore
  // the root list to this frame's entry mark before anything walks it, the
  // same idiom an in-language `try` uses on its catch path.
  line("  size_t _rm = ov_gc_roots_mark();");
  line("  if (setjmp(*_jb) == 0) {");
  line("    ov_main();");
  line("    _code = 0;");
  line("  } else {");
  line("    ov_gc_roots_restore(_rm);");
  line("    _code = ov_uncaught_code();");
  line("  }");
  line("  ov_try_pop();");
  line("  ov_runtime_shutdown();");
  // `--stats`: what the heap actually did. Emitted only when asked for, so a
  // benchmark never prints its own instrumentation into its own timing.
  if (stats_) line("  ov_heap_print_stats();");
  line("  return _code;");
  line("}");

  return out;
}

}  // namespace

std::string emit_c_source(const ast::Program& program, Sema& sema,
                          const std::string& source, std::string& err,
                          bool stats) {
  (void)source;
  Gen g(program, sema);
  g.stats_ = stats;
  std::string out = g.run();
  if (!g.err.empty()) { err = g.err; return ""; }
  err.clear();
  return out;
}

}  // namespace ov

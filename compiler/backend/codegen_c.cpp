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

class Gen {
 public:
  std::string out;
  std::string err;

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
  bool is_proven_int_any_scope(const std::string& name) const {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) {
        return proven_ints_.count(it->second) > 0;
      }
    }
    // Not in current codegen scopes, check sema
    auto it = sema_.proven_types.find(name);
    return it != sema_.proven_types.end() && it->second.is_proven_int;
  }
  bool is_proven_float_any_scope(const std::string& name) const {
    for (int i = (int)scopes_.size() - 1; i >= 0; i--) {
      auto it = scopes_[i].find(name);
      if (it != scopes_[i].end()) {
        return proven_floats_.count(it->second) > 0;
      }
    }
    auto it = sema_.proven_types.find(name);
    return it != sema_.proven_types.end() && it->second.is_proven_float;
  }

  // Initialize proven types from sema at codegen start
  void init_proven_types() {
    for (const auto& kv : sema_.proven_types) {
      const std::string& name = kv.first;
      const TyInfo& info = kv.second;
      // Use the C variable name that will be generated for this Ovyth name
      std::string cname = ident(name, 0);  // base name, uniq=0 for globals/params
      if (info.is_proven_int) proven_ints_.insert(cname);
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

  // A hoisted user function, plus whether it is variadic.
  std::unordered_map<std::string, const Stmt*> userfns_;
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
// Returns true when `e` is provably a pure int64_t expression:
//   - IntLit
//   - Identifier whose C name is in proven_ints_
//   - Binary(+,-,*,%,&,|,^,<<,>>) of two such expressions
// Does NOT include SLASH (might be int/int but we still want ov_div's
// zero-check semantics in the general case; fold_const_binop handles literals).
bool Gen::is_int_expr(const Expr* e) const {
  if (!e) return false;
  if (e->kind == ExprKind::IntLit) return true;
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
  switch (e->op) {
    case Tok::PLUS: case Tok::MINUS: case Tok::STAR: case Tok::PERCENT:
    case Tok::AMP: case Tok::PIPE: case Tok::CARET:
    case Tok::SHL: case Tok::SHR:
      return is_int_expr(e->a) && is_int_expr(e->b);
    default: return false;
  }
}

// Returns a raw int64_t C expression. Only call when is_int_expr() is true.
std::string Gen::ex_int(const Expr* e) {
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
      // Fallback to generic OvList
      std::string l = fresh(), v = fresh();
      std::string s = "({ OvList* " + l + " = ov_list_new(); OvValue " + v +
                      " = ov_list(" + l + "); ";
      for (const Expr* it : e->items)
        s += "ov_list_push(" + l + ", " + ex(it) + "); ";
      return s + v + "; })";
    }

    case ExprKind::MapLit: {
      std::string m = fresh(), v = fresh();
      std::string s = "({ OvMap* " + m + " = ov_map_new(); OvValue " + v +
                      " = ov_map(" + m + "); ";
      for (const auto& f : e->fields)
        s += "ov_map_set(" + m + ", " + ex(f.first) + ", " + ex(f.second) + "); ";
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
    case ExprKind::Call:     return emit_call(e);

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
  std::string out = fresh();
  std::string s = "({ OvList* " + out + " = ov_list_new(); ";
  std::vector<std::string> scoped;  // names to restore on the way out

  std::function<void(size_t)> emit_loops = [&](size_t gi) {
    if (gi == e->generators.size()) {
      s += "ov_list_push(" + out + ", " + ex(e->items[0]) + "); ";
      return;
    }
    const Generator& g = e->generators[gi];
    std::string it = fresh(), i = fresh();
    std::string var = ident(g.var, tmp_++);
    push_scope();
    scoped.push_back(g.var);
    scopes_.back()[g.var] = var;

    std::string iterable = ex(g.iterable);
    std::string cond;
    if (g.cond) cond = ex(g.cond);  // sees this and outer loop variables

    const std::string I = it, J = i, V = var;
    s += "{ OvValue " + it + " = " + iterable + "; ";
    // Handle all iterable types: string, list, int64array, float64array, stringarray
    s += "OvList* " + it + "_l = NULL; ";
    s += "OvInt64Array* " + it + "_i64a = NULL; ";
    s += "OvFloat64Array* " + it + "_f64a = NULL; ";
    s += "OvStringArray* " + it + "_stra = NULL; ";
    s += "if (ov_tagof(" + I + ") == OV_STRING) { " + it + "_l = ov_str_chars(" + I + ".str).list; } "
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

    s += "} ";  // close scope
    pop_scope();
  };
  emit_loops(0);
  return s + "ov_list(" + out + "); })";
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
      return "({ OvValue " + b + " = " + ex(target->a) + "; OvValue " + i +
             " = " + ex(target->b) + "; if (ov_tagof(" + B + ") == OV_LIST) "
             "ov_list_set(" + B + ".list, " + I + ".i, " + V + "); else "
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
    if (n == "http" || n == "json" || n == "str" || n == "math" || n == "time")
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
  inline_roots_.clear();

  std::string body_buf;
  body_buf.swap(out);
  indent_ = 1;
  fn_has_return_ = false;
  line("(void)_argv; (void)_argc;");
  for (size_t i = 0; i < e->params.size(); i++)
    line("OvValue " + ident(e->params[i].name, i) + " = _argv[" +
         std::to_string(i) + "];");
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
  std::string b = fresh(), o = fresh();

  // Hot-path for push/append on known specialized arrays
  if ((m->name == "push" || m->name == "append") && args.size() <= 1) {
    std::string av = fresh();
    // Check if base is a proven int/float array
    if (e->a && e->a->kind == ExprKind::Identifier) {
      const std::string& base_name = e->a->name;
      if (std::string* c = lookup(base_name)) {
        if (proven_ints_.count(*c)) {
          // Fast path for int64_t array push
          return "({ OvInt64Array* " + *c + "_arr = (" + *c + "); " + *c + "_arr->data[" + *c + "_arr->len++] = " + a0 + "; " + base + "; })";
        }
        if (proven_floats_.count(*c)) {
          // Fast path for double array push
          return "({ OvFloat64Array* " + *c + "_arr = (" + *c + "); " + *c + "_arr->data[" + *c + "_arr->len++] = " + a0 + "; " + base + "; })";
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

  // Hot-path for pop on known specialized arrays
  if (m->name == "pop" && args.empty()) {
    if (e->a && e->a->kind == ExprKind::Identifier) {
      const std::string& base_name = e->a->name;
      if (std::string* c = lookup(base_name)) {
        if (proven_ints_.count(*c)) {
          return "ov_int(" + *c + "->data[--" + *c + "->len])";
        }
        if (proven_floats_.count(*c)) {
          return "ov_float(" + *c + "->data[--" + *c + "->len])";
        }
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
    if (name == "now")   return "ov_int((int64_t)ov_h_now())";
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
        // Check if it's a proven int/float array
        if (proven_ints_.count(*c) || proven_floats_.count(*c)) {
          // It's a raw array, not a OvList
          return "(" + *c + "->len)";
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
  if (q == "now" || q == "time.now") return "ov_int((int64_t)ov_h_now())";
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
      if (ns == "http" || ns == "json" || ns == "math" || ns == "time" || ns == "str")
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
      // Positional then named arguments bound by parameter name.
      std::vector<std::string> pos;
      for (const Expr* a : e->args) pos.push_back(ex(a));
      std::vector<std::string> byname(decl->params.size());
      for (size_t i = 0; i < byname.size(); i++) byname[i] = "ov_nil()";
      for (size_t i = 0; i < pos.size() && i < byname.size(); i++) byname[i] = pos[i];
      for (const auto& na : e->named_args) {
        bool found = false;
        for (size_t k = 0; k < decl->params.size(); k++)
          if (decl->params[k].name == na.name) { byname[k] = ex(na.value); found = true; }
        if (!found) { fail("'" + q + "' has no parameter named '" + na.name + "'"); }
      }
      std::string call = funcname(q) + "(";
      for (size_t k = 0; k < byname.size(); k++) {
        if (k) call += ", ";
        call += byname[k];
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
          line(bind(name, rhs, pi, pf) + ";");
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
      line("_ret = " + (s->expr ? ex(s->expr) : "ov_nil()") + "; goto " + ret_label_ + ";");
      return;

    case StmtKind::If: {
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
      std::string c = fresh();
      line("{ OvValue " + c + " = " + ex(s->expr) + ";");
      indent_++;
      line("if (!ov_truthy(" + c + ")) goto " + L.brk + "; }");
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
      line("goto " + loops_.back().brk + ";");
      return;

    case StmtKind::Continue:
      if (loops_.empty()) { fail("continue outside a loop"); return; }
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
      indent_++;
      line("if (setjmp(*_jb) == 0) {");
      indent_++;
      block(s->body, true);
      indent_--;
      line("  ov_try_pop(); goto " + tb + "; }");
      line("  ov_try_pop();");
      if (!s->catch_var.empty()) line("  " + bind(s->catch_var, "ov_caught") + ";");
      else line("  (void)ov_caught;");
      block(s->else_body, true);
      line(tb + ": ;");
      indent_--;
      line("}");
      try_depth_--;
      return;
    }

    case StmtKind::Debug:
      line("fprintf(stderr, \"[ovyth] %d\\n\", " + std::to_string(s->pos.line) + ");");
      return;

    case StmtKind::For:
      emit_for(s);
      return;
  }
}

void Gen::emit_for(const Stmt* s) {
  // `for v in xs` over a list, a string's characters, a map's keys, or specialized arrays.
  std::string it = fresh(), idx = fresh(), seq = fresh(), n = fresh();
  std::string var = s->iter_vars.empty() ? std::string("ov_unused") : ident(s->iter_vars[0], tmp_++);
  LoopLabels L{lbl("Lbrk"), lbl("Lcont")};
  std::string top = lbl("Ltop");

  const std::string I = it, S = seq, N = n, X = idx;
  line("{ OvValue " + it + " = " + ex(s->expr) + ";");
  indent_++;
  // Handle all iterable types: string, list, map, int64array, float64array, stringarray
  line("OvList* " + seq + " = NULL;");
  line("OvInt64Array* " + seq + "_i64a = NULL;");
  line("OvFloat64Array* " + seq + "_f64a = NULL;");
  line("OvStringArray* " + seq + "_stra = NULL;");
  line("if (ov_tagof(" + I + ") == OV_STRING) { " + seq + " = ov_str_chars(" + I + ".str).list; } "
       "else if (ov_tagof(" + I + ") == OV_MAP) { " + seq + " = ov_map_keys(" + I + ".map).list; } "
       "else if (ov_tagof(" + I + ") == OV_LIST) { " + seq + " = " + I + ".list; } "
       "else if (ov_tagof(" + I + ") == OV_I64A) { " + seq + "_i64a = " + I + ".i64a; } "
       "else if (ov_tagof(" + I + ") == OV_F64A) { " + seq + "_f64a = " + I + ".f64a; } "
       "else if (ov_tagof(" + I + ") == OV_STRA) { " + seq + "_stra = " + I + ".stra; } ");
  line("int64_t " + n + " = 0;");
  line("if (" + seq + ") " + n + " = " + seq + "->len;");
  line("else if (" + seq + "_i64a) " + n + " = " + seq + "_i64a->len;");
  line("else if (" + seq + "_f64a) " + n + " = " + seq + "_f64a->len;");
  line("else if (" + seq + "_stra) " + n + " = " + seq + "_stra->len;");
  line("for (int64_t " + X + " = 0; " + X + " < " + N + "; " + X + "++) {");
  indent_++;
  push_scope();
  scopes_.back()[s->iter_vars.empty() ? std::string("__unused") : s->iter_vars[0]] = var;
  cur_roots_.push_back(var);
  // Extract element based on iterable type
  line("OvValue " + var + ";");
  line("if (" + seq + ") " + var + " = " + seq + "->items[" + X + "];");
  line("else if (" + seq + "_i64a) " + var + " = ov_int(" + seq + "_i64a->data[" + X + "]);");
  line("else if (" + seq + "_f64a) " + var + " = ov_float(" + seq + "_f64a->data[" + X + "]);");
  line("else if (" + seq + "_stra) " + var + " = ov_str(" + seq + "_stra->data[" + X + "]);");
  bool uses_continue = body_uses_continue(s->body);
  bool uses_break = body_uses_break(s->body);
  loops_.push_back(L);
  block(s->body, false);
  loops_.pop_back();
  if (uses_continue) line(L.cont + ": ;");
  pop_scope();
  indent_--;
  line("}");
  // Only emitted when the body actually jumps to it -- see the note on the
  // continue label in the While case.
  if (uses_break) line(L.brk + ": ;");
  indent_--;
  line("}");
  (void)top;
}

// ---------------------------------------------------------------- functions
void Gen::collect_functions(const StmtList& body) {
  for (const Stmt* s : body)
    if (s && s->kind == StmtKind::FuncDecl && !userfns_.count(s->name))
      userfns_[s->name] = s;
}

void Gen::emit_function(const Stmt* s) {
  std::ostringstream sig;
  sig << "static OvValue " << funcname(s->name) << "(";
  if (s->params.empty()) {
    sig << "void";
  } else {
    for (size_t i = 0; i < s->params.size(); i++) {
      if (i) sig << ", ";
      sig << "OvValue " << ident(s->params[i].name, 0);
    }
  }
  sig << ")";

  auto saved_scopes = scopes_;
  auto saved_ret = ret_label_;
  auto saved_fn = cur_fn_;
  auto saved_box_of = box_of_;
  auto saved_box_writes = box_writes_;
  box_of_.clear();
  box_writes_.clear();
  std::string saved_out;
  saved_out.swap(out);
  int saved_indent = indent_;
  indent_ = 0;
  cur_fn_ = s->name;
  ret_label_ = lbl("Lret");

  push_scope();
  for (const auto& p : s->params) scopes_.back()[p.name] = ident(p.name, 0);

  // Two passes. The body is lowered first into a scratch buffer so every local
  // it introduces is known; only then is the entry prologue written, declaring
  // those locals and registering each as a GC root. Doing it in this order is
  // what makes the result safe: the collector can fire on any allocation, so a
  // live local must be rooted before the first instruction that can allocate.
  std::vector<std::string> outer_roots;
  outer_roots.swap(cur_roots_);
  inline_roots_.clear();

  std::string body_buf;
  body_buf.swap(out);
  indent_ = 1;
  fn_has_return_ = false;
  block(s->body, false);
  if (fn_has_return_) line(ret_label_ + ": ;");
  line("  ov_gc_roots_restore(_roots_mark);");
  line("  return _ret; }");
  body_buf.swap(out);
  indent_ = 0;

  nl();
  line(sig.str() + " {");
  indent_++;
  line("OvValue _ret = ov_nil();");
  line("size_t _roots_mark = ov_gc_roots_mark();");
  emit_root_prologue();
  cur_roots_.swap(outer_roots);   // done: locals are rooted by the prologue
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
  pending_fns_.push_back(body);
}

// ------------------------------------------------------------------- module
std::string Gen::run() {
  collect_functions(prog_.statements);
  init_proven_types();

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
      std::ostringstream sig;
      sig << "static OvValue " << funcname(s->name) << "(";
      if (s->params.empty()) {
        sig << "void";
      } else {
        for (size_t i = 0; i < s->params.size(); i++) {
          if (i) sig << ", ";
          sig << "OvValue " << ident(s->params[i].name, 0);
        }
      }
      sig << ");";
      line(sig.str());
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
  line("  if (setjmp(*_jb) == 0) {");
  line("    ov_main();");
  line("    _code = 0;");
  line("  } else {");
  // An uncaught panic, or `exit(code)`, arrives here via longjmp.
  line("    _code = ov_exit_code();");
  line("  }");
  line("  ov_try_pop();");
  line("  ov_runtime_shutdown();");
  line("  return _code;");
  line("}");

  return out;
}

}  // namespace

std::string emit_c_source(const ast::Program& program, Sema& sema,
                          const std::string& source, std::string& err) {
  (void)source;
  Gen g(program, sema);
  std::string out = g.run();
  if (!g.err.empty()) { err = g.err; return ""; }
  err.clear();
  return out;
}

}  // namespace ov

//
// Lowering the AST to C.
//
// The driver compiles the emitted C with clang and links it against
// libvyrt.a, so the result is an ordinary native executable: no interpreter,
// no VM, no runtime dependency beyond libcurl/OpenSSL/zlib. The C compiler on
// the box *is* the LLVM toolchain, so `-O3 -flto` and
// `-ffunction-sections -fdata-sections` in the perf phase apply to generated
// code exactly as they would to hand-written C.
//
// Every Vayu value is one `VyValue` (tag + unboxed payload), so an expression
// lowers to a single C expression of type VyValue. Constructs needing
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

namespace vy {
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

// C identifiers for Vayu names, and C function names for Vayu functions.
std::string ident(const std::string& n, int uniq) {
  return "v_" + n + "_" + std::to_string(uniq);
}
std::string funcname(const std::string& n) { return "vy_fn_" + n; }

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
  std::string var;    // Vayu name of the accumulator
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
  // box_of_[name] is the box VyValue expression in the enclosing scope;
  // box_writes_ marks names whose assignments must store through the box.
  // Saved/restored around closure bodies (which get their own box set).
  std::unordered_map<std::string, std::string> box_of_;
  std::set<std::string> box_writes_;

  // Proven numeric types: names that are proven Int/Float and can use raw registers
  std::unordered_set<std::string> proven_ints_;
  std::unordered_set<std::string> proven_floats_;

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
  // For proven Int/Float, we use raw C types instead of VyValue and skip GC registration.
  std::string bind(const std::string& name, const std::string& rhs, bool is_proven_int = false, bool is_proven_float = false) {
    if (box_writes_.count(name) && box_of_.count(name)) {
      return "vy_list_set((" + box_of_[name] + ").list, 0, " + rhs + ")";
    }
    if (is_proven_int || is_proven_float) {
      // Use raw C type, no GC registration needed
      if (std::string* c = lookup(name)) return *c + " = " + rhs;
      std::string c = ident(name, tmp_++);
      scopes_.back()[name] = c;
      if (is_proven_int) proven_ints_.insert(c);   // track by C name
      if (is_proven_float) proven_floats_.insert(c);
      // Emit the C declaration inline and push to prologue list
      cur_roots_.push_back("@int:" + c);  // sentinel: raw int64_t
      return "int64_t " + c + " = " + rhs;
    }
    // Regular VyValue - needs GC registration
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
      decls += "VyValue " + slot + " = vy_nil();";
      regs += "vy_gc_register_root(&" + slot + ");";
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
// the vyrt.h fast paths, clang folds most of this again at -O3, so this is
// what makes -O0/debug builds and constant subexpressions free too.
// Deliberately NOT folded: division/modulo by literal zero and int edges
// (the runtime must still raise its usual error), and `**` (vy_pow always
// returns Float -- folding it to an Int would change observable types).
static std::string fold_int(int64_t v) {
  return "vy_int(" + std::to_string(v) + "LL)";
}
static std::string fold_float(double v) {
  std::ostringstream o;
  o.precision(17);
  o << v;
  std::string s = o.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
    s += ".0";
  return "vy_float(" + s + ")";
}

static bool fold_const_binop(const Expr* e, std::string& out) {
  const Expr* x = e->a;
  const Expr* y = e->b;
  if (!x || !y) return false;
  const bool xi = x->kind == ExprKind::IntLit, xf = x->kind == ExprKind::FloatLit;
  const bool yi = y->kind == ExprKind::IntLit, yf = y->kind == ExprKind::FloatLit;
  if (!(xi || xf) || !(yi || yf)) return false;
  const bool allint = xi && yi;
  auto emit_bool = [&](bool v) { out = v ? "vy_bool(1)" : "vy_bool(0)"; return true; };

  // Comparisons: same semantics as vy_eq / vy_cmp for every literal numeric
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
      if (x->kind == ExprKind::BoolLit) { out = x->bval ? "vy_bool(0)" : "vy_bool(1)"; return true; }
      return false;
    default: return false;
  }
}

// ---------------------------------------------------------------- proven-int helpers
// Returns true when `e` is provably a pure int64_t expression:
//   - IntLit
//   - Identifier whose C name is in proven_ints_
//   - Binary(+,-,*,%,&,|,^,<<,>>) of two such expressions
// Does NOT include SLASH (might be int/int but we still want vy_div's
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
    case Tok::PERCENT: return "(" + bi + ")?(" + ai + ")%(" + bi + "):(vy_zero_error(),0LL)";
    case Tok::AMP:     return "(" + ai + ")&(" + bi + ")";
    case Tok::PIPE:    return "(" + ai + ")|(" + bi + ")";
    case Tok::CARET:   return "(" + ai + ")^(" + bi + ")";
    case Tok::SHL:     return "(" + ai + ")<<(" + bi + ")";
    case Tok::SHR:     return "(" + ai + ")>>(" + bi + ")";
    default: return "0LL";
  }
}

// ---------------------------------------------------------------- literals
std::string Gen::ex(const Expr* e) {
  if (!e) return "vy_nil()";

  switch (e->kind) {
    case ExprKind::IntLit:
      return "vy_int(" + std::to_string(e->ival) + "LL)";
    case ExprKind::FloatLit: {
      std::ostringstream o;
      o.precision(17);
      o << e->fval;
      std::string s = o.str();
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
      return "vy_float(" + s + ")";
    }
    case ExprKind::BoolLit:  return e->bval ? "vy_bool(1)" : "vy_bool(0)";
    case ExprKind::NullLit:  return "vy_nil()";
    case ExprKind::StringLit:
      // One pinned allocation per literal site, cached in a block-local
      // static. The old shape calloc'd a fresh VyStr on EVERY evaluation --
      // that was most of strconcat's cost (40k literal allocs per run) and
      // half of mapops' (200k "key" allocs).
      return "({ static VyStr* vy_lit_; if (!vy_lit_) vy_lit_ = vy_str_lit(" +
             quote_c(e->sval) + ", " + std::to_string(e->sval.size()) +
             "); vy_str(vy_lit_); })";

    case ExprKind::Identifier: {
      if (std::string* c = lookup(e->name)) return *c;
      if (userfns_.count(e->name)) return "vy_nil()";
      fail("unknown name '" + e->name + "'");
      return "vy_nil()";
    }

    case ExprKind::ListLit: {
      std::string l = fresh(), v = fresh();
      std::string s = "({ VyList* " + l + " = vy_list_new(); VyValue " + v +
                      " = vy_list(" + l + "); ";
      for (const Expr* it : e->items)
        s += "vy_list_push(" + l + ", " + ex(it) + "); ";
      return s + v + "; })";
    }

    case ExprKind::MapLit: {
      std::string m = fresh(), v = fresh();
      std::string s = "({ VyMap* " + m + " = vy_map_new(); VyValue " + v +
                      " = vy_map(" + m + "); ";
      for (const auto& f : e->fields)
        s += "vy_map_set(" + m + ", " + ex(f.first) + ", " + ex(f.second) + "); ";
      return s + v + "; })";
    }

    case ExprKind::Interp: return emit_interp(e);

    case ExprKind::Unary: {
      if (std::string cf; fold_const_unary(e, cf)) return cf;
      std::string a = fresh();
      std::string s = "({ VyValue " + a + " = " + ex(e->a) + "; ";
      switch (e->op) {
        case Tok::KW_NOT: s += "vy_bool(!vy_truthy(" + a + "))"; break;
        case Tok::MINUS:   s += "vy_neg(" + a + ")"; break;
        case Tok::PLUS:    s += a; break;
        case Tok::TILDE:   s += "vy_int(~((" + a + ").i))"; break;
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
        return "({ VyValue " + n + " = " + ex(e->a) + "; VyValue " + h + " = " +
               ex(e->b) + "; vy_bool(vy_in(" + n + ", " + h + ")); })";
      }
      // Proven-int fast path: both operands are statically Int.
      // Emit raw int64_t arithmetic; no VyValue temporaries, no tag checks.
      if (is_int_expr(e->a) && is_int_expr(e->b)) {
        std::string ai = ex_int(e->a);
        std::string bi = ex_int(e->b);
        switch (e->op) {
          case Tok::PLUS:          return "vy_int((" + ai + ") + (" + bi + "))";
          case Tok::MINUS:         return "vy_int((" + ai + ") - (" + bi + "))";
          case Tok::STAR:          return "vy_int((" + ai + ") * (" + bi + "))";
          case Tok::PERCENT:       return "vy_int((" + bi + ") ? (" + ai + ") % (" + bi + ") : (vy_zero_error(),0LL))";
          case Tok::AMP:           return "vy_int((" + ai + ") & (" + bi + "))";
          case Tok::PIPE:          return "vy_int((" + ai + ") | (" + bi + "))";
          case Tok::CARET:         return "vy_int((" + ai + ") ^ (" + bi + "))";
          case Tok::SHL:           return "vy_int((" + ai + ") << (" + bi + "))";
          case Tok::SHR:           return "vy_int((" + ai + ") >> (" + bi + "))";
          case Tok::EQUAL:         return "vy_bool((" + ai + ") == (" + bi + "))";
          case Tok::BANG_EQUAL:    return "vy_bool((" + ai + ") != (" + bi + "))";
          case Tok::LESS:          return "vy_bool((" + ai + ") < (" + bi + "))";
          case Tok::GREATER:       return "vy_bool((" + ai + ") > (" + bi + "))";
          case Tok::LESS_EQUAL:    return "vy_bool((" + ai + ") <= (" + bi + "))";
          case Tok::GREATER_EQUAL: return "vy_bool((" + ai + ") >= (" + bi + "))";
          // SLASH falls through to VyValue path (div-by-zero handled by vy_div)
          default: break;
        }
      }
      std::string a = fresh(), b = fresh();
      const std::string pre = "({ VyValue " + a + " = " + ex(e->a) + "; VyValue " +
                              b + " = " + ex(e->b) + "; ";
      const std::string& A = a;
      const std::string& B = b;
      switch (e->op) {
        case Tok::PLUS:          return pre + "vy_add(" + A + ", " + B + "); })";
        case Tok::MINUS:         return pre + "vy_sub(" + A + ", " + B + "); })";
        case Tok::STAR:          return pre + "vy_mul(" + A + ", " + B + "); })";
        case Tok::SLASH:         return pre + "vy_div(" + A + ", " + B + "); })";
        case Tok::PERCENT:       return pre + "vy_mod(" + A + ", " + B + "); })";
        case Tok::DOUBLE_STAR:   return pre + "vy_pow(" + A + ", " + B + "); })";
        case Tok::AMP:           return pre + "vy_bitand(" + A + ", " + B + "); })";
        case Tok::PIPE:          return pre + "vy_bitor(" + A + ", " + B + "); })";
        case Tok::CARET:         return pre + "vy_bitxor(" + A + ", " + B + "); })";
        case Tok::SHL:           return pre + "vy_lshift(" + A + ", " + B + "); })";
        case Tok::SHR:           return pre + "vy_rshift(" + A + ", " + B + "); })";
        case Tok::EQUAL:         return pre + "vy_bool(vy_eq(" + A + ", " + B + ")); })";
        case Tok::BANG_EQUAL:    return pre + "vy_bool(!vy_eq(" + A + ", " + B + ")); })";
        case Tok::LESS:          return pre + "vy_bool(vy_cmp(" + A + ", " + B + ") < 0); })";
        case Tok::GREATER:       return pre + "vy_bool(vy_cmp(" + A + ", " + B + ") > 0); })";
        case Tok::LESS_EQUAL:    return pre + "vy_bool(vy_cmp(" + A + ", " + B + ") <= 0); })";
        case Tok::GREATER_EQUAL: return pre + "vy_bool(vy_cmp(" + A + ", " + B + ") >= 0); })";
        default:
          fail("unsupported binary operator");
          return pre + A + "; })";
      }
    }

    case ExprKind::Logical: {
      std::string a = fresh();
      const std::string& A = a;
      if (e->op == Tok::KW_AND)
        return "({ VyValue " + a + " = " + ex(e->a) + "; vy_bool(vy_truthy(" + A +
               ") && vy_truthy(" + ex(e->b) + ")); })";
      return "({ VyValue " + a + " = " + ex(e->a) + "; vy_bool(vy_truthy(" + A +
             ") || vy_truthy(" + ex(e->b) + ")); })";
    }

    case ExprKind::Ternary: {
      std::string c = fresh();
      return "({ VyValue " + c + " = " + ex(e->a) + "; vy_truthy(" + c + ") ? " +
             ex(e->b) + " : " + ex(e->c) + "; })";
    }

    case ExprKind::Assign: {
      std::string v = fresh();
      if (e->op == Tok::ASSIGN) {
        return "({ VyValue " + v + " = " + ex(e->b) + "; " +
               emit_assign(e->a, v) + "; " + v + "; })";
      }
      // Compound: the interpreter evaluates the value, then the current
      // target, then applies the operator and rebinds.
      std::string cur = fresh();
      std::string rhs;
      const std::string& V = v;
      const std::string& C = cur;
      switch (e->op) {
        case Tok::PLUS_EQUAL:    rhs = "vy_add(" + C + ", " + V + ")"; break;
        case Tok::MINUS_EQUAL:   rhs = "vy_sub(" + C + ", " + V + ")"; break;
        case Tok::STAR_EQUAL:    rhs = "vy_mul(" + C + ", " + V + ")"; break;
        case Tok::SLASH_EQUAL:   rhs = "vy_div(" + C + ", " + V + ")"; break;
        case Tok::PERCENT_EQUAL: rhs = "vy_mod(" + C + ", " + V + ")"; break;
        default:
          fail("unsupported compound assignment");
          rhs = C;
      }
      return "({ VyValue " + v + " = " + ex(e->b) + "; VyValue " + cur + " = " +
             ex(e->a) + "; " + emit_assign(e->a, rhs) + "; " + rhs + "; })";
    }

    case ExprKind::Index: {
      std::string b = fresh(), i = fresh(), o = fresh();
      // Pin base and index via mutation guard: intermediate objects on the C
      // stack are not GC roots, so a collection triggered by vy_str_lit (or
      // any other allocation inside ex(e->b)) would free them. The mutation
      // counter is nestable so chained calls are safe.
      return "({ vy_gc_begin_mutation(); VyValue " + b + " = " + ex(e->a) +
             "; VyValue " + i + " = " + ex(e->b) + "; VyValue " + o +
             "; if (vy_h_index(" + b + ", " + i + ", &" + o + ")) { vy_gc_end_mutation(); vy_throw_value(vy_h_err_value()); } " +
             "vy_gc_end_mutation(); " + o + "; })";
    }

    case ExprKind::Member: {
      std::string b = fresh(), o = fresh();
      return "({ vy_gc_begin_mutation(); VyValue " + b + " = " + ex(e->a) +
             "; VyValue " + o + "; if (vy_h_member(" + b + ", " + quote_c(e->name) +
             ", &" + o + ")) { vy_gc_end_mutation(); vy_throw_value(vy_h_err_value()); } " +
             "vy_gc_end_mutation(); " + o + "; })";
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
        return "({ VyValue " + a + " = " + ex(e->a) + "; vy_int(vy_tagof(" + A +
               ") == VY_FLOAT ? (int64_t)(" + A + ").f : " + A + ".i); })";
      if (n == "Float")
        return "({ VyValue " + a + " = " + ex(e->a) + "; vy_float(vy_tagof(" + A +
               ") == VY_INT ? (double)(" + A + ").i : " + A + ".f); })";
      if (n == "String" || n == "Str")
        return "({ VyValue " + a + " = " + ex(e->a) + "; vy_str(vy_render(" + A + ")); })";
      return ex(e->a);
    }

    case ExprKind::Error:
    default:
      fail("cannot lower this expression");
      return "vy_nil()";
  }
}

// ----------------------------------------------------------- interpolation
std::string Gen::emit_interp(const Expr* e) {
  std::string acc = fresh();
  std::string s = "({ VyStr* " + acc + " = vy_str_new(\"\", 0); ";
  size_t n = e->items.size(), m = e->parts.size(), li = 0;
  for (size_t i = 0; i < m; i++) {
    if (li < n) {
      const std::string& t = e->items[li++]->sval;
      s += acc + " = vy_str_concat(" + acc + ", vy_str_new(" + quote_c(t) + ", " +
           std::to_string(t.size()) + ")); ";
    }
    s += acc + " = vy_str_concat(" + acc + ", vy_render(" + ex(e->parts[i]) +
         ")); ";
  }
  for (; li < n; li++) {
    const std::string& t = e->items[li]->sval;
    s += acc + " = vy_str_concat(" + acc + ", vy_str_new(" + quote_c(t) + ", " +
         std::to_string(t.size()) + ")); ";
  }
  return s + "vy_str(" + acc + "); })";
}

// ------------------------------------------------------------------ slicing
std::string Gen::emit_slice(const Expr* e) {
  std::string b = fresh(), lo = fresh(), hi = fresh(), n = fresh();
  std::string out = fresh(), outl = fresh(), a = fresh(), z = fresh(), ai = fresh();
  const std::string B = b, N = n, A = a, Z = z, O = out;
  std::string s = "({ VyValue " + b + " = " + ex(e->a) + "; ";
  s += "int64_t " + n + " = (vy_tagof(" + B + ") == VY_STRING) ? " + B +
       ".str->len : (vy_tagof(" + B + ") == VY_LIST) ? " + B + ".list->len : -1; ";
  s += "if (" + N + " < 0) vy_throw_str(vy_str_new(\"cannot slice\", 12)); ";
  s += "VyValue " + lo + " = " + (e->b ? ex(e->b) : std::string("vy_int(0)")) + "; ";
  s += "VyValue " + hi + " = " + (e->c ? ex(e->c) : ("(" + N + ")")) + "; ";
  s += "int64_t " + a + " = " + lo + ".i, " + z + " = " + hi + ".i; ";
  s += "if (" + A + " < 0) " + A + " += " + N + "; ";
  s += "if (" + Z + " < 0) " + Z + " += " + N + "; ";
  s += "if (" + A + " < 0) " + A + " = 0; ";
  s += "if (" + Z + " > " + N + ") " + Z + " = " + N + "; ";
  s += "if (" + Z + " < " + A + ") " + Z + " = " + A + "; ";
  s += "VyValue " + out + "; ";
  s += "if (vy_tagof(" + B + ") == VY_STRING) { " + O + " = vy_str(vy_str_slice(" +
       B + ".str, " + A + ", " + Z + ")); } ";
  s += "else { VyList* " + outl + " = vy_list_new(); for (int64_t " + ai + " = " +
       A + "; " + ai + " < " + Z + "; " + ai + "++) vy_list_push(" + outl +
       ", vy_list_get(" + B + ".list, " + ai + ")); " + O + " = vy_list(" + outl +
       "); } ";
  return s + O + "; })";
}

// ------------------------------------------------------------- comprehension
std::string Gen::emit_listcomp(const Expr* e) {
  std::string out = fresh();
  std::string s = "({ VyList* " + out + " = vy_list_new(); ";
  std::vector<std::string> scoped;  // names to restore on the way out

  std::function<void(size_t)> emit_loops = [&](size_t gi) {
    if (gi == e->generators.size()) {
      s += "vy_list_push(" + out + ", " + ex(e->items[0]) + "); ";
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
    s += "{ VyValue " + it + " = " + iterable + "; ";
    s += "VyList* " + it + "_l = (vy_tagof(" + I + ") == VY_STRING) ? "
         "vy_str_chars(" + I + ".str).list : " + I + ".list; ";
    s += "if (" + I + "_l) for (uint32_t " + i + " = 0; " + i + " < " + I +
         "_l->len; " + i + "++) { VyValue " + var + " = " + I + "_l->items[" + J +
         "]; ";
    if (!cond.empty()) s += "if (vy_truthy(" + cond + ")) { ";
    emit_loops(gi + 1);
    if (!cond.empty()) s += " } ";
    s += "} } ";

    pop_scope();
  };
  emit_loops(0);
  return s + "vy_list(" + out + "); })";
}

// --------------------------------------------------------------- assignment
std::string Gen::emit_assign(const Expr* target, const std::string& val) {
  switch (target->kind) {
    case ExprKind::Identifier: {
      // A name shared with a closure through a box: store through the box so
      // both sides observe the write. box_of_[name] is the box VyValue.
      std::cerr << "[DBG emit_assign] target=" << target->name
                << " box_of_ size=" << box_of_.size()
                << " box_writes_ size=" << box_writes_.size()
                << " box_of_ has=" << (box_of_.count(target->name) ? "yes" : "no")
                << " box_writes_ has=" << (box_writes_.count(target->name) ? "yes" : "no") << "\n";
      auto bit = box_of_.find(target->name);
      if (bit != box_of_.end() && box_writes_.count(target->name)) {
        std::cerr << "[DBG emit_assign] -> USING vy_list_set for " << target->name << "\n";
        return "(vy_list_set((" + bit->second + ").list, 0, " + val + "), " + val + ")";
      }
      std::cerr << "[DBG emit_assign] -> USING bind for " << target->name << "\n";
      return bind(target->name, val);
    }
    case ExprKind::Index: {
      std::string b = fresh(), i = fresh();
      const std::string B = b, I = i, V = val;
      return "({ VyValue " + b + " = " + ex(target->a) + "; VyValue " + i +
             " = " + ex(target->b) + "; if (vy_tagof(" + B + ") == VY_LIST) "
             "vy_list_set(" + B + ".list, " + I + ".i, " + V + "); else "
             "if (vy_tagof(" + B + ") == VY_MAP) vy_map_set(" + B + ".map, " + I +
             ", " + V + "); else vy_throw_str(vy_str_new(\"cannot index-assign\", "
             "17)); })";
    }
    case ExprKind::Member: {
      std::string b = fresh();
      const std::string B = b, V = val;
      return "({ VyValue " + b + " = " + ex(target->a) + "; if (vy_tagof(" + B +
             ") != VY_MAP) vy_throw_str(vy_str_new(\"cannot set field\", 15)); "
             "vy_map_set(" + B + ".map, vy_str(vy_str_new(" + quote_c(target->name) +
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
// travels in VyFunc.upvals (vy_h_make_closure), so the closure body reads
// `_fn->upvals[i]` and both sides observe each other's writes -- exactly
// matching the interpreter, which shares the parent Env.
//
// Capture semantics (matches the interpreter):
//   - shared, not snapshot: writes through the closure are visible outside
//     and vice versa (verified by tests/interp/017_closures.vy).
//   - params and locals defined inside the body are NOT captures.
//   - a closure with no free variables emits exactly the old shape
//     (vy_h_make_func, no upvals) -- zero cost for non-capturing code.
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
      for (const auto& n : s->names) bound.insert(n);
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

  std::string cname = "vy_anon_" + std::to_string(tmp_++);
  // Match VyFnPtr exactly: (VyFunc* fn, VyValue* argv, int argc). Parameters are read
  // positionally out of argv, so the emitted body is ABI-compatible with
  // everything else that stores a VyFunc. Captures read from _fn->upvals.
  std::string sig = "static VyValue " + cname + "(struct VyFunc* _fn, VyValue* _argv, int _argc)";

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
  std::cerr << "[DBG closure] captures.size=" << captures.size() << "\n";
  for (size_t i = 0; i < captures.size(); i++) {
    std::cerr << "[DBG closure] capture[" << i << "]=" << captures[i] << "\n";
    scopes_.back()[captures[i]] =
        "vy_list_get(((VyValue*)_fn->upvals)[" + std::to_string(i) + "].list, 0)";
    box_writes_.insert(captures[i]);
    // The box VyValue in this closure's body is the upvals slot.
    box_of_[captures[i]] = "((VyValue*)_fn->upvals)[" + std::to_string(i) + "]";
  }
  std::cerr << "[DBG closure] after setup: box_of_ size=" << box_of_.size()
            << " box_writes_ size=" << box_writes_.size() << "\n";
  for (const auto& kv : box_of_) {
    std::cerr << "[DBG closure]   box_of_[" << kv.first << "]=" << kv.second << "\n";
  }
  for (const auto& n : box_writes_) {
    std::cerr << "[DBG closure]   box_writes_ has " << n << "\n";
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
    line("VyValue " + ident(e->params[i].name, i) + " = _argv[" +
         std::to_string(i) + "];");
  block(e->body, false);
  if (fn_has_return_) line(ret_label_ + ": ;");
  line("  vy_gc_roots_restore(_roots_mark);");
  line("  return _ret; }");
  body_buf.swap(out);
  indent_ = 0;

  nl();
  line(sig + " {");
  indent_++;
  line("VyValue _ret = vy_nil();");
  line("size_t _roots_mark = vy_gc_roots_mark();");
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
    return "vy_func(vy_h_make_func(" + std::to_string(e->params.size()) + ", " + cname + "))";
  }

  // Capturing: share each outer local through a 1-element box list.
  // For each captured variable, if it is not yet boxed in the enclosing scope,
  // allocate a GC-rooted local for the box, initialize it with the variable's
  // current value, and rebind the variable to read/write through the box.
  // Then pass each capture's box VyValue into the upvals array.
  std::string ua = fresh();
  std::string s = "({ ";
  for (size_t i = 0; i < captures.size(); i++) {
    const std::string& n = captures[i];
    if (!box_of_.count(n)) {
      std::string bx = ident("box_" + n, tmp_++);
      cur_roots_.push_back(bx);
      std::string* c = lookup(n);
      std::string init_val = c ? *c : "vy_nil()";
      if (proven_ints_.count(init_val)) init_val = "vy_int(" + init_val + ")";
      else if (proven_floats_.count(init_val)) init_val = "vy_float(" + init_val + ")";
      s += bx + " = vy_list(vy_list_new()); ";
      s += "vy_list_push(" + bx + ".list, " + init_val + "); ";
      box_of_[n] = bx;
      box_writes_.insert(n);
      if (c) *c = "vy_list_get(" + bx + ".list, 0)";
    }
  }
  s += "VyValue* " + ua + " = (VyValue*)calloc(" +
       std::to_string(captures.size()) + ", sizeof(VyValue)); ";
  for (size_t i = 0; i < captures.size(); i++) {
    s += ua + "[" + std::to_string(i) + "] = " + box_of_[captures[i]] + "; ";
  }
  s += "vy_func(vy_h_make_closure(" + std::to_string(e->params.size()) + ", " +
       cname + ", " + ua + ", " + std::to_string(captures.size()) + ")); })";
  return s;
}


// Emit `vy_sb_append_str(builder, <rhs>)` when `name` is the active string
// accumulator being appended to. Returns true when it handled the statement.
bool Gen::try_sb_append(const std::string& name, const Expr* rhs, Tok op) {
  if (!sb_.active || name != sb_.var || !rhs) return false;
  if (op == Tok::PLUS_EQUAL) {
    line("vy_sb_append_value(" + sb_.cvar + ", " + ex(rhs) + ");");
    return true;
  }
  if (op == Tok::ASSIGN && rhs->kind == ExprKind::Binary && rhs->op == Tok::PLUS &&
      rhs->a && rhs->a->kind == ExprKind::Identifier && rhs->a->name == sb_.var) {
    line("vy_sb_append_value(" + sb_.cvar + ", " + ex(rhs->b) + ");");
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
  std::string a0 = args.size() > 0 ? args[0] : "vy_nil()";
  std::string a1 = args.size() > 1 ? args[1] : "vy_nil()";
  std::string b = fresh(), o = fresh();

  // Hot-path guard for the builder pattern: xs.push(x) used to pay an
  // out-of-line dispatch + strcmp chain per call (500k times in
  // benchmarks/cases/listappend.vy). A list base -- by far the common case --
  // now takes the inline vy_list_push directly; anything else falls through
  // to the exact same dispatch as before, errors included. Only emitted for
  // arity <= 1 so every argument expression still gets evaluated.
  if ((m->name == "push" || m->name == "append") && args.size() <= 1) {
    std::string av = fresh();
    return "({ VyValue " + b + " = " + base + "; VyValue " + av + " = " + a0 +
           "; VyValue " + o + "; if (vy_tagof(" + b + ") == VY_LIST) { vy_list_push(" +
           b + ".list, " + av + "); " + o + " = " + b + "; } else if (vy_h_value_method(" +
           b + ", " + quote_c(m->name) + ", " + av + ", vy_nil(), &" + o +
           ")) vy_throw_value(vy_h_err_value()); " + o + "; })";
  }

  return "({ VyValue " + b + " = " + base + "; VyValue " + o + "; if (vy_h_value_method(" +
         b + ", " + quote_c(m->name) + ", " + a0 + ", " + a1 + ", &" + o +
         ")) vy_throw_value(vy_h_err_value()); " + o + "; })";
}

std::string Gen::call_namespace(const std::string& ns, const std::string& name,
                                const Expr* e) {
  // Only the namespaces the MVP chatbot/test surface needs.
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  auto named = [&](const char* k) -> std::string {
    for (const auto& na : e->named_args)
      if (na.name == k) return ex(na.value);
    return "vy_nil()";
  };

  if (ns == "json") {
    if (name == "parse") {
      std::string o = fresh(), t = fresh();
      return "({ VyValue " + o + " = " + (args.empty() ? "vy_nil()" : args[0]) +
             "; int " + t + " = 0; VyValue _r = vy_h_json_parse(" + o + ", &" + t +
             "); if (" + t + ") vy_throw_value(vy_h_err_value()); _r; })";
    }
    if (name == "stringify") {
      std::string o = fresh();
      return "({ VyValue " + o + " = " + (args.empty() ? "vy_nil()" : args[0]) +
             "; vy_str(vy_json_stringify(" + o + ")); })";
    }
    if (name == "valid") {
      std::string o = fresh();
      return "({ VyValue " + o + " = " + (args.empty() ? "vy_nil()" : args[0]) +
             "; vy_bool(vy_tagof(" + o + ") == VY_STRING && vy_json_valid(" + o +
             ".str->bytes, " + o + ".str->len)); })";
    }
    if (name == "extract") {
      // Fast JSON field extraction without full AST build
      std::string json_val = args.empty() ? "vy_nil()" : args[0];
      std::string path_val = args.size() < 2 ? "vy_nil()" : args[1];
      
      // Compile-time optimization: if path is a string literal, embed it directly
      // This avoids creating a VyValue for the path at runtime
      if (args.size() >= 2 && e->args[1] && e->args[1]->kind == ExprKind::StringLit) {
        const std::string& path_str = e->args[1]->sval;
        return "({ VyValue _v = " + json_val + "; "
               "VyValue _r = vy_nil(); if (vy_tagof(_v) == VY_STRING) { "
               "_r = vy_json_extract_field(_v.str->bytes, _v.str->len, \"" + 
               path_str + "\"); } "
               "_r; })";
      }
      
      return "({ VyValue _v = " + json_val + "; VyValue _p = " + path_val + "; "
             "VyValue _r = vy_nil(); if (vy_tagof(_v) == VY_STRING && vy_tagof(_p) == VY_STRING) { "
             "_r = vy_json_extract_field(_v.str->bytes, _v.str->len, _p.str->bytes); } "
             "_r; })";
    }
    if (name == "get_float") {
      // Fast numeric field accessor - avoids building full VyValue
      if (args.size() >= 2 && e->args[1] && e->args[1]->kind == ExprKind::StringLit) {
        const std::string& key = e->args[1]->sval;
        return "({ VyValue _v = " + args[0] + "; double _d = 0; "
               "vy_json_get_float(_v, \"" + key + "\", &_d) ? vy_float(_d) : vy_nil(); })";
      }
      return "({ VyValue _v = " + args[0] + "; VyValue _k = " + args[1] + "; "
             "double _d = 0; vy_json_get_float(_v, _k.str->bytes, &_d) ? vy_float(_d) : vy_nil(); })";
    }
    if (name == "get_int") {
      // Fast integer field accessor
      if (args.size() >= 2 && e->args[1] && e->args[1]->kind == ExprKind::StringLit) {
        const std::string& key = e->args[1]->sval;
        return "({ VyValue _v = " + args[0] + "; int64_t _i = 0; "
               "vy_json_get_int(_v, \"" + key + "\", &_i) ? vy_int(_i) : vy_nil(); })";
      }
      return "({ VyValue _v = " + args[0] + "; VyValue _k = " + args[1] + "; "
             "int64_t _i = 0; vy_json_get_int(_v, _k.str->bytes, &_i) ? vy_int(_i) : vy_nil(); })";
    }
  }
  if (ns == "http") {
    std::string url = args.empty() ? "vy_nil()" : args[0];
    std::string hdrs = named("headers");
    // http.post(url, body) / http.post(url, json = {...}): the second
    // positional argument is the body for requests that carry one, and the
    // query map for GET/DELETE/HEAD.
    bool getlike = (name == "get" || name == "delete" || name == "head" ||
                    name == "GET" || name == "DELETE" || name == "HEAD");
    std::string json = named("json");
    std::string params = "vy_nil()";
    if (args.size() >= 2) {
      if (getlike) params = args[1];
      else if (json == "vy_nil()") json = args[1];
    }
    std::string named_params = named("params");
    std::string body = named("body");
    std::string ctype = named("content_type");
    std::string timeout = named("timeout");
    std::string o = fresh();
    std::string meth = name;
    for (auto& c : meth) c = (char)toupper((unsigned char)c);
    return "({ VyValue " + o + "; if (vy_h_http(" + quote_c(meth) + ", " + url +
           ", " + hdrs + ", " + json + ", " + params + ", " + named_params + ", " +
           body + ", " + ctype + ", " + timeout + ", &" + o +
           ")) vy_throw_value(vy_h_err_value()); " + o + "; })";
  }
  if (ns == "time") {
    if (name == "clock") return "vy_float(vy_h_now())";
    if (name == "now")   return "vy_int((int64_t)vy_h_now())";
  }
  fail("unsupported namespace call '" + ns + "." + name + "'");
  return "vy_nil()";
}

std::string Gen::call_global(const std::string& q, const Expr* e) {
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  auto ARG = [&](size_t i) { return i < args.size() ? args[i] : "vy_nil()"; };
  auto NAMED = [&](const char* k) -> std::string {
    for (const auto& na : e->named_args)
      if (na.name == k) return ex(na.value);
    return "vy_nil()";
  };
  std::string o = fresh(), t = fresh();

  if (q == "print") {
    std::string s = "({ ";
    for (size_t i = 0; i < args.size(); i++) {
      if (i) s += "fputc(' ', stdout); ";
      s += "{ VyStr* _r = vy_render(" + args[i] + "); fwrite(_r->bytes, 1, _r->len, stdout); } ";
    }
    return s + "fputc('\\n', stdout); vy_nil(); })";
  }
  if (q == "eprint") {
    // Same rendering as print, but on stderr, so a program's machine-readable
    // stdout stays clean while progress and diagnostics go to the terminal.
    std::string s = "({ ";
    for (size_t i = 0; i < args.size(); i++) {
      if (i) s += "fputc(' ', stderr); ";
      s += "{ VyStr* _r = vy_render(" + args[i] + "); fwrite(_r->bytes, 1, _r->len, stderr); } ";
    }
    return s + "fputc('\\n', stderr); vy_nil(); })";
  }
  if (q == "input") {
    return "vy_h_input(" + ARG(0) + ")";
  }
  if (q == "env" || q == "getenv") {
    return "({ int " + t + " = 0; VyValue _r = vy_h_env(" + ARG(0) + ", &" + t +
           "); if (" + t + ") vy_throw_value(vy_h_err_value()); _r; })";
  }
  if (q == "env_or" || q == "getenv_or") {
    // vy_h_env sets `t` when the variable is missing (and stores the message in
    // the error slot); `t` is truthy then, so the fallback wins. Evaluated
    // lazily -- the fallback expression may have side effects.
    return "({ int " + t + " = 0; VyValue _r = vy_h_env(" + ARG(0) + ", &" + t +
           "); " + t + " ? " + ARG(1) + " : _r; })";
  }
  if (q == "setenv") {
    // Both arguments render to strings, exactly like the interpreter's setenv.
    return "({ VyStr* _n = vy_render(" + ARG(0) + "); VyStr* _v = vy_render(" +
           ARG(1) + "); vy_env_set(vy_str_data(_n), _v); vy_nil(); })";
  }
  if (q == "pad")
    return "({ int " + t + " = 0; VyValue _r = vy_h_pad(" + ARG(0) + ", " + ARG(1) +
           ", " + ARG(2) + ", " + ARG(3) + ", &" + t + "); if (" + t +
           ") vy_throw_value(vy_h_err_value()); _r; })";
  if (q == "len") {
    return "({ int " + t + " = 0; VyValue _r = vy_h_len(" + ARG(0) + ", &" + t +
           "); if (" + t + ") vy_throw_value(vy_h_err_value()); _r; })";
  }
  if (q == "str") return "vy_str(vy_render(" + ARG(0) + "))";
  if (q == "type") return "({ const char* _n = vy_type_name(" + ARG(0) + "); vy_str(vy_str_new(_n, strlen(_n))); })";
  if (q == "int")
    return "({ int " + t + " = 0; VyValue _r = vy_h_to_int(" + ARG(0) + ", &" + t +
           "); if (" + t + ") vy_throw_value(vy_h_err_value()); _r; })";
  if (q == "float")
    return "({ int " + t + " = 0; VyValue _r = vy_h_to_float(" + ARG(0) + ", &" + t +
           "); if (" + t + ") vy_throw_value(vy_h_err_value()); _r; })";
  if (q == "bool") return "vy_bool(vy_truthy(" + ARG(0) + "))";
  if (q == "exit")
    return "({ vy_request_exit(vy_isnil(" + ARG(0) + ") ? 0 : (int)(" + ARG(0) +
           ").i); vy_nil(); })";
  if (q == "throw")
    return "({ vy_throw_value(" + (args.empty() ? std::string("vy_str(vy_str_new(\"thrown\", 6))") : args[0]) + "); vy_nil(); })";
  if (q == "assert") {
    // assert(cond) / assert(cond, "message")
    std::string msg = args.size() > 1 ? ex(e->args[1])
                                      : std::string("vy_str(vy_str_new(\"assertion failed\", 16))");
    return "({ if (!vy_truthy(" + ARG(0) + ")) vy_throw_value(vy_render(" + msg +
           ")); vy_nil(); })";
  }
  if (q == "range") {
    if (args.size() == 1) return "vy_range(0, (" + ex(e->args[0]) + ").i, 1)";
    if (args.size() == 2)
      return "vy_range((" + ex(e->args[0]) + ").i, (" + ex(e->args[1]) + ").i, 1)";
    return "vy_range((" + ex(e->args[0]) + ").i, (" + ex(e->args[1]) + ").i, (" +
           ex(e->args[2]) + ").i)";
  }
  if (q == "abs") return "({ int _t = 0; vy_h_num1(" + ARG(0) + ", &_t, 'a'); })";
  if (q == "sqrt") return "({ int _t = 0; vy_h_num1(" + ARG(0) + ", &_t, 's'); })";
  if (q == "floor") return "({ int _t = 0; vy_h_num1(" + ARG(0) + ", &_t, 'f'); })";
  if (q == "ceil") return "({ int _t = 0; vy_h_num1(" + ARG(0) + ", &_t, 'c'); })";
  if (q == "round") return "({ int _t = 0; vy_h_num1(" + ARG(0) + ", &_t, 'r'); })";
  if (q == "min") return "({ VyValue _a = " + ARG(0) + ", _b = " + ARG(1) + "; int _c = vy_cmp(_a,_b); _c == 0 ? _a : (((_c < 0)) == 1 ? _a : _b); })";
  if (q == "max") return "({ VyValue _a = " + ARG(0) + ", _b = " + ARG(1) + "; int _c = vy_cmp(_a,_b); _c == 0 ? _a : (((_c < 0)) == 0 ? _a : _b); })";
  if (q == "sum") return "({ int _t = 0; vy_h_sum(" + ARG(0) + ", &_t); })";
  if (q == "upper") return "vy_str(vy_str_upper(" + ARG(0) + ".str))";
  if (q == "lower") return "vy_str(vy_str_lower(" + ARG(0) + ".str))";
  if (q == "trim")  return "vy_str(vy_str_trim(" + ARG(0) + ".str))";
  if (q == "contains")
    return "({ VyValue _n = " + ARG(0) + ", _h = " + ARG(1) + "; vy_bool(vy_tagof(_h) == VY_STRING && vy_tagof(_n) == VY_STRING ? vy_str_contains(_h.str, _n.str) : vy_in(_n, _h)); })";
  if (q == "join") return "vy_h_join(" + ARG(0) + ", " + ARG(1) + ")";
  if (q == "clock" || q == "time.clock") return "vy_float(vy_h_now())";
  if (q == "now" || q == "time.now") return "vy_int((int64_t)vy_h_now())";
  if (q == "gc") return "vy_h_gc(" + ARG(0) + ")";

  fail("unknown function '" + q + "'");
  return "vy_nil()";
}

std::string Gen::emit_call(const Expr* e) {
  const Expr* callee = e->a;

  if (!callee) { fail("malformed call"); return "vy_nil()"; }

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
      for (size_t i = 0; i < byname.size(); i++) byname[i] = "vy_nil()";
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
      if (decl->params.empty()) call += "void";
      return call + ")";
    }
    return call_global(q, e);
  }

  // Calling a value: a closure produced by a function expression.
  {
    std::vector<std::string> args;
    for (const Expr* a : e->args) args.push_back(ex(a));
    std::string c = fresh(), r = fresh();
    std::string call = "({ VyValue " + c + " = " + ex(callee) +
                       "; VyValue* _a; int _n; VyValue " + r + " = vy_h_call(" + c + ", ";
    call += "{ ";
    for (size_t i = 0; i < args.size(); i++) { if (i) call += ", "; call += args[i]; }
    call += "}, " + std::to_string(args.size()) + ", &_a, &_n); " + r + "; })";
    return call;
  }
}


std::string Gen::call_value_method_closure(const std::string& name, const Expr* e) {
  std::string* cv = lookup(name);
  std::string callee = cv ? *cv : "vy_nil()";
  std::vector<std::string> args;
  for (const Expr* a : e->args) args.push_back(ex(a));
  // Named arguments are not supported on a closure value; positional only.
  std::string c = fresh();
  std::string s = "({ VyValue " + c + " = " + callee + "; ";
  s += "VyValue _argv[";
  s += std::to_string(args.size() ? args.size() : 1);
  s += "]; VyValue* _slot = _argv; int _n = " + std::to_string(args.size()) + "; ";
  for (size_t i = 0; i < args.size(); i++) {
    s += "_argv[" + std::to_string(i) + "] = " + args[i] + "; ";
  }
  s += "vy_h_call(" + c + ", _argv, _n, &_slot, &_n); })";
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
      // Evaluate every value into a temp first, so `a, b = b, a` is correct
      // and each intermediate stays reachable across allocations.
      std::vector<std::string> tmp;
      for (size_t i = 0; i < n; i++) {
        std::string t = fresh();
        std::string rhs = (i < s->values.size() && s->values[i]) ? ex(s->values[i])
                                                                : "vy_nil()";
        line("VyValue " + t + " = " + rhs + ";");
        tmp.push_back(t);
      }
      // `a, b = [x, y]` unpacks a list on the right: every name, starting at
      // index 0, takes the element at that position.
      if (n > 1 && s->values.size() == 1) {
        std::string src = tmp[0];
        for (size_t i = 0; i < n; i++) {
          std::string t = fresh();
          line("VyValue " + t + " = (vy_tagof(" + src + ") == VY_LIST && " +
               std::to_string(i) + " < " + src + ".list->len) ? vy_list_get(" +
               src + ".list, " + std::to_string(i) + ") : vy_nil();");
          tmp[i] = t;
        }
      }
      for (size_t i = 0; i < n; i++) {
        // With an active string builder, `acc = ""` is just the builder's
        // initial (empty) state -- do not rebind the result variable.
        // The accumulator's own storage is the builder, so `acc = ""` inside a
        // rewritten loop must not rebind the result variable.
        bool is_acc_init =
            sb_.active && s->names[i] == sb_.var && i < s->values.size() &&
            s->values[i] && s->values[i]->kind == ExprKind::StringLit &&
            s->values[i]->sval.empty();
        if (!is_acc_init) line(bind(s->names[i], tmp[i]) + ";");
      }
      // Remember `x = ""` so a following loop can recognise string building.
      if (n == 1 && s->values[0] && s->values[0]->kind == ExprKind::StringLit &&
          s->values[0]->sval.empty())
        last_empty_str_ = s->names[0];
      return;
    }

    case StmtKind::Assign: {
      std::string v = fresh();

      // String-builder accumulation (spec 12).
      if (s->kind == StmtKind::Assign && s->expr &&
          s->expr->kind == ExprKind::Identifier) {
        if (try_sb_append(s->expr->name, s->expr2, s->op)) return;
      }

      if (s->op == Tok::ASSIGN) {
        line("VyValue " + v + " = " + ex(s->expr2) + ";");
        line(emit_assign(s->expr, v) + ";");
        return;
      }
      std::string cur = fresh();
      std::string op;
      const std::string V = v, C = cur;
      switch (s->op) {
        case Tok::PLUS_EQUAL:    op = "vy_add(" + C + ", " + V + ")"; break;
        case Tok::MINUS_EQUAL:   op = "vy_sub(" + C + ", " + V + ")"; break;
        case Tok::STAR_EQUAL:    op = "vy_mul(" + C + ", " + V + ")"; break;
        case Tok::SLASH_EQUAL:   op = "vy_div(" + C + ", " + V + ")"; break;
        case Tok::PERCENT_EQUAL: op = "vy_mod(" + C + ", " + V + ")"; break;
        default: op = C; fail("unsupported compound assignment");
      }
      line("VyValue " + v + " = " + ex(s->expr2) + ";");
      line("VyValue " + cur + " = " + ex(s->expr) + ";");
      line(emit_assign(s->expr, op) + ";");
      return;
    }

    case StmtKind::FuncDecl:
      // Hoisted: the definition was emitted at module scope.
      return;

    case StmtKind::Return:
      fn_has_return_ = true;
      line("_ret = " + (s->expr ? ex(s->expr) : "vy_nil()") + "; goto " + ret_label_ + ";");
      return;

    case StmtKind::If: {
      std::string c = fresh();
      line("{ VyValue " + c + " = " + ex(s->expr) + ";");
      indent_++;
      if (s->else_body.empty()) {
        line("if (vy_truthy(" + c + ")) {");
        indent_++;
        block(s->body, true);
        indent_--;
        line("}");
      } else {
        line("if (vy_truthy(" + c + ")) {");
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
        // Bind the Vayu name to the *finished* string, materialised after the
        // loop; inside, appends go to the builder.
        std::string res = "__vy_sb_result_" + sb_.cvar;
        scopes_.back()[acc] = res;
        cur_roots_.push_back(res);
        inline_roots_.insert(res);
        line("VyValue " + res + " = vy_nil();");
        line("vy_gc_register_root(&" + res + ");");
        line("VyStrBuilder* " + sb_.cvar + " = vy_sb_new();");
      }

      bool uses_continue = body_uses_continue(s->body);
      line(top + ": ;");
      std::string c = fresh();
      line("{ VyValue " + c + " = " + ex(s->expr) + ";");
      indent_++;
      line("if (!vy_truthy(" + c + ")) goto " + L.brk + "; }");
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
        line(res + " = vy_str(vy_sb_finish(" + sb_.cvar + "));");
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
      line("vy_throw_value(" + (s->expr ? ex(s->expr) : std::string("vy_str(vy_str_new(\"thrown\", 6))")) + ");");
      return;

    case StmtKind::Try: {
      // The exact panic idiom documented in vyrt.h.
      std::string tb = lbl("Ltry");
      try_depth_++;
      line("{ jmp_buf* _jb = vy_try_push();");
      indent_++;
      line("if (setjmp(*_jb) == 0) {");
      indent_++;
      block(s->body, true);
      indent_--;
      line("  vy_try_pop(); goto " + tb + "; }");
      line("  vy_try_pop();");
      if (!s->catch_var.empty()) line("  " + bind(s->catch_var, "vy_caught") + ";");
      else line("  (void)vy_caught;");
      block(s->else_body, true);
      line(tb + ": ;");
      indent_--;
      line("}");
      try_depth_--;
      return;
    }

    case StmtKind::Debug:
      line("fprintf(stderr, \"[vayu] %d\\n\", " + std::to_string(s->pos.line) + ");");
      return;

    case StmtKind::For:
      emit_for(s);
      return;
  }
}

void Gen::emit_for(const Stmt* s) {
  // `for v in xs` over a list, a string's characters, or a map's keys.
  std::string it = fresh(), idx = fresh(), seq = fresh(), n = fresh();
  std::string var = s->iter_vars.empty() ? std::string("vy_unused") : ident(s->iter_vars[0], tmp_++);
  LoopLabels L{lbl("Lbrk"), lbl("Lcont")};
  std::string top = lbl("Ltop");

  const std::string I = it, S = seq, N = n, X = idx;
  line("{ VyValue " + it + " = " + ex(s->expr) + ";");
  indent_++;
  line("VyList* " + seq + " = (vy_tagof(" + I + ") == VY_STRING) ? "
       "vy_str_chars(" + I + ".str).list : (vy_tagof(" + I + ") == VY_MAP) ? "
       "vy_map_keys(" + I + ".map).list : " + I + ".list;");
  line("int64_t " + n + " = " + S + " ? (int64_t)" + S + "->len : 0;");
  line("for (int64_t " + X + " = 0; " + X + " < " + N + "; " + X + "++) {");
  indent_++;
  push_scope();
  scopes_.back()[s->iter_vars.empty() ? std::string("__unused") : s->iter_vars[0]] = var;
  cur_roots_.push_back(var);
  line("VyValue " + var + " = " + S + "->items[" + X + "];");
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
  sig << "static VyValue " << funcname(s->name) << "(";
  if (s->params.empty()) {
    sig << "void";
  } else {
    for (size_t i = 0; i < s->params.size(); i++) {
      if (i) sig << ", ";
      sig << "VyValue " << ident(s->params[i].name, 0);
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
  line("  vy_gc_roots_restore(_roots_mark);");
  line("  return _ret; }");
  body_buf.swap(out);
  indent_ = 0;

  nl();
  line(sig.str() + " {");
  indent_++;
  line("VyValue _ret = vy_nil();");
  line("size_t _roots_mark = vy_gc_roots_mark();");
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

  // ---- header ----
  out +=
      "/* Generated by vyc. Do not edit. */\n"
      "#include \"vyrt.h\"\n"
      "#include \"vyrt_helpers.h\"\n"
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
      sig << "static VyValue " << funcname(s->name) << "(";
      if (s->params.empty()) {
        sig << "void";
      } else {
        for (size_t i = 0; i < s->params.size(); i++) {
          if (i) sig << ", ";
          sig << "VyValue " << ident(s->params[i].name, 0);
        }
      }
      sig << ");";
      line(sig.str());
    }
    nl();

    // Emit each body into a scratch buffer, then append it to the module.
    for (const Stmt* s : prog_.statements)
      if (s && s->kind == StmtKind::FuncDecl) emit_function(s);
    for (const auto& f : pending_fns_) out += f;
    pending_fns_.clear();

    // Closure bodies discovered while lowering anything above.
    // Forward-declare every anon function first: a nested closure's creation
    // site lives INSIDE its enclosing closure's body, which is emitted before
    // the inner body -- without a declaration that is a C compile error
    // (`use of undeclared identifier 'vy_anon_N'`). Signatures are
    // ABI-uniform, so one declaration shape covers all anon functions.
    {
      std::set<std::string> seen;
      for (const auto& nm : anon_names_) {
        if (seen.insert(nm).second)
          line("static VyValue " + nm +
               "(struct VyFunc* _fn, VyValue* _argv, int _argc);");
      }
    }
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
    line("  vy_gc_roots_restore(_roots_mark);");
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
          line("static VyValue " + nm +
               "(struct VyFunc* _fn, VyValue* _argv, int _argc);");
      }
    }
    while (!anon_defs_.empty()) { out += anon_defs_.back(); anon_defs_.pop_back(); }
    anon_names_.clear();

    line("static VyValue vy_main(void) {");
    indent_++;
    line("VyValue _ret = vy_nil();");
    line("size_t _roots_mark = vy_gc_roots_mark();");
    emit_root_prologue();
    cur_roots_.swap(outer_roots);
    out += body;
    indent_ = 0;
  }

  // ---- entry point ----
  line("");
  line("int main(int argc, char** argv) {");
  line("  vy_runtime_init(argc, argv);");
  line("  int _code = 0;");
  line("  jmp_buf* _jb = vy_try_push();");
  line("  if (setjmp(*_jb) == 0) {");
  line("    vy_main();");
  line("    _code = 0;");
  line("  } else {");
  // An uncaught panic, or `exit(code)`, arrives here via longjmp.
  line("    _code = vy_exit_code();");
  line("  }");
  line("  vy_try_pop();");
  line("  vy_runtime_shutdown();");
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

}  // namespace vy

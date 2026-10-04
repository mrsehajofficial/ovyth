/* Vayu runtime :: src/value.c  --  operators, comparison, truthiness. */
#include "vyrt.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- numbers */

static double as_f(VyValue v) {
  switch (vy_tagof(v)) {
    case VY_INT:   return (double)v.i;
    case VY_FLOAT: return v.f;
    case VY_BOOL:  return v.b ? 1.0 : 0.0;
    default:       vy_type_error("a number", v);
  }
  return 0;
}

static int64_t as_i(VyValue v) {
  switch (vy_tagof(v)) {
    case VY_INT:   return v.i;
    case VY_FLOAT: return (int64_t)v.f;
    case VY_BOOL:  return v.b ? 1 : 0;
    default:       vy_type_error("an integer", v);
  }
  return 0;
}

static int is_num(VyValue v) {
  VyTag t = vy_tagof(v);
  return t == VY_INT || t == VY_FLOAT;
}

static int both_int(VyValue a, VyValue b) {
  return vy_tagof(a) == VY_INT && vy_tagof(b) == VY_INT;
}

/* ------------------------------------------------------------- strings */

static VyStr* as_s(VyValue v) {
  if (vy_tagof(v) != VY_STRING) vy_type_error("a string", v);
  return v.str;
}

/* Repeated `s + s` in a loop is the hottest string path in AI code (building
 * request bodies, joining log lines). Grow geometrically when the receiver is
 * the last allocation and is uniquely owned, otherwise copy. */
static VyStr* str_build(VyStr* a, const char* b, size_t blen) {
  size_t n = a->len + blen;
  VyStr* s = vy_str_new(a->bytes, n);
  memcpy(s->bytes + a->len, b, blen);
  return s;
}

VyValue vy_add_slow(VyValue a, VyValue b) {
  if (vy_tagof(a) == VY_STRING && vy_tagof(b) == VY_STRING)
    return vy_str(vy_str_concat(a.str, b.str));
  /* String coercion:  "n=" + 42  ==  "n=42" (render the non-string side). */
  if (vy_tagof(a) == VY_STRING)
    return vy_str(vy_str_concat(a.str, vy_render(b)));
  if (vy_tagof(b) == VY_STRING)
    return vy_str(vy_str_concat(vy_render(a), b.str));
  if (vy_tagof(a) == VY_LIST && vy_tagof(b) == VY_LIST) {
    VyList* out = vy_list_new_cap(a.list->len + b.list->len);
    memcpy(out->items, a.list->items, a.list->len * sizeof(VyValue));
    memcpy(out->items + a.list->len, b.list->items, b.list->len * sizeof(VyValue));
    out->len = a.list->len + b.list->len;
    return vy_list(out);
  }
  if (is_num(a) && is_num(b)) {
    if (both_int(a, b)) return vy_int(a.i + b.i);
    return vy_float(as_f(a) + as_f(b));
  }
  if (vy_tagof(a) == VY_NIL) return b;
  if (vy_tagof(b) == VY_NIL) return a;
  vy_type_error("two numbers, strings or lists", a);
  return vy_nil();
}

VyValue vy_sub_slow(VyValue a, VyValue b) {
  if (is_num(a) && is_num(b)) {
    if (both_int(a, b)) return vy_int(a.i - b.i);
    return vy_float(as_f(a) - as_f(b));
  }
  vy_type_error("two numbers", a);
  return vy_nil();
}

VyValue vy_mul_slow(VyValue a, VyValue b) {
  if (is_num(a) && is_num(b)) {
    if (both_int(a, b)) return vy_int(a.i * b.i);
    return vy_float(as_f(a) * as_f(b));
  }
  /* string repeat:  "-" * 3  ==  "---" */
  if (vy_tagof(a) == VY_STRING && vy_tagof(b) == VY_INT)
    return vy_str(vy_str_repeat(a.str, b.i));
  if (vy_tagof(a) == VY_INT && vy_tagof(b) == VY_STRING)
    return vy_str(vy_str_repeat(b.str, a.i));
  if (vy_tagof(a) == VY_LIST && vy_tagof(b) == VY_INT) {
    VyList* l = a.list;
    VyList* out = vy_list_new_cap(l->len * (size_t)(b.i > 0 ? b.i : 0) + 4);
    for (int64_t i = 0; i < b.i; i++)
      for (uint32_t j = 0; j < l->len; j++) vy_list_push(out, l->items[j]);
    return vy_list(out);
  }
  vy_type_error("two numbers", a);
  return vy_nil();
}

VyValue vy_div_slow(VyValue a, VyValue b) {
  if (!is_num(a) || !is_num(b)) vy_type_error("two numbers", a);
  if (both_int(a, b)) {
    if (b.i == 0) vy_zero_error();
    return vy_int(a.i / b.i);
  }
  double d = as_f(b);
  if (d == 0.0) vy_zero_error();
  return vy_float(as_f(a) / d);
}

VyValue vy_mod_slow(VyValue a, VyValue b) {
  if (!is_num(a) || !is_num(b)) vy_type_error("two numbers", a);
  if (both_int(a, b)) {
    if (b.i == 0) vy_zero_error();
    return vy_int(a.i % b.i);
  }
  double d = as_f(b);
  if (d == 0.0) vy_zero_error();
  return vy_float(fmod(as_f(a), d));
}

VyValue vy_pow(VyValue a, VyValue b) {
  if (!is_num(a) || !is_num(b)) vy_type_error("two numbers", a);
  return vy_float(pow(as_f(a), as_f(b)));
}

/* vy_neg / vy_pos / vy_not / vy_truthy / vy_is are now static inline in
 * vyrt.h (P0 fast paths): the scalar case has to be visible to the optimiser
 * or every loop pays a call + tag dispatch per operator. */

VyValue vy_bitand_slow(VyValue a, VyValue b) { return vy_int(as_i(a) & as_i(b)); }
VyValue vy_bitxor_slow(VyValue a, VyValue b) { return vy_int(as_i(a) ^ as_i(b)); }
VyValue vy_bitor_slow(VyValue a, VyValue b)  { return vy_int(as_i(a) | as_i(b)); }
VyValue vy_lshift_slow(VyValue a, VyValue b) { return vy_int((int64_t)((uint64_t)as_i(a) << as_i(b))); }
VyValue vy_rshift_slow(VyValue a, VyValue b) { return vy_int(as_i(a) >> as_i(b)); }

/* ------------------------------------------------------------ equality */

int vy_eq_slow(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_FLOAT) return (double)a.i == b.f;
  if (ta == VY_FLOAT && tb == VY_INT) return a.f == (double)b.i;
  if (ta != tb) return 0;
  switch (ta) {
    case VY_NIL:   return 1;
    case VY_BOOL:  return (a.b != 0) == (b.b != 0);
    case VY_INT:   return a.i == b.i;
    case VY_FLOAT: return a.f == b.f;
    case VY_STRING:return vy_str_eq(a.str, b.str);
    case VY_LIST: {
      VyList* x = a.list;
      VyList* y = b.list;
      if (x == y) return 1;
      if (x->len != y->len) return 0;
      for (uint32_t i = 0; i < x->len; i++)
        if (!vy_eq(x->items[i], y->items[i])) return 0;
      return 1;
    }
    case VY_MAP:   return a.map == b.map;
    default:       return a.tag == b.tag;
  }
}

int vy_cmp_slow(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) return a.i < b.i ? -1 : (a.i > b.i);
  if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT)) {
    double x = as_f(a), y = as_f(b);
    return x < y ? -1 : (x > y);
  }
  if (ta == VY_STRING && tb == VY_STRING) return vy_str_cmp(a.str, b.str);
  if (ta == VY_LIST && tb == VY_LIST) {
    uint32_t n = a.list->len < b.list->len ? a.list->len : b.list->len;
    for (uint32_t i = 0; i < n; i++) {
      int c = vy_cmp(a.list->items[i], b.list->items[i]);
      if (c) return c;
    }
    return a.list->len < b.list->len ? -1 : (a.list->len > b.list->len);
  }
  /* nil sorts before everything, then by type tag for a stable order */
  if (ta == VY_NIL || tb == VY_NIL)
    return ta == tb ? 0 : (ta == VY_NIL ? -1 : 1);
  if (ta != tb) return ta < tb ? -1 : 1;
  return 0;
}

int vy_in(VyValue needle, VyValue haystack) {
  if (vy_tagof(haystack) == VY_LIST) return vy_list_contains(haystack.list, needle);
  if (vy_tagof(haystack) == VY_MAP)   return vy_map_has(haystack.map, needle);
  if (vy_tagof(haystack) == VY_STRING) {
    if (vy_tagof(needle) != VY_STRING) return 0;
    return vy_str_contains(haystack.str, needle.str);
  }
  return 0;
}

VyValue vy_range(int64_t lo, int64_t hi, int64_t step) {
  if (step == 0) vy_error("range step cannot be zero");
  int64_t n = step > 0 ? ((hi - lo + step - 1) / step) : ((hi - lo + step + 1) / step);
  if (n < 0) n = 0;
  VyList* l = vy_list_new_cap((uint32_t)n);
  for (int64_t i = 0; i < n; i++) vy_list_push(l, vy_int(lo + i * step));
  return vy_list(l);
}

/* --------------------------------------------------------------- types */

const char* vy_tag_name(VyTag t) {
  switch (t) {
    case VY_NIL:    return "nil";
    case VY_BOOL:   return "Bool";
    case VY_INT:    return "Int";
    case VY_FLOAT:  return "Float";
    case VY_STRING: return "String";
    case VY_LIST:   return "List";
    case VY_MAP:    return "Map";
    case VY_FUNC:   return "Function";
  }
  return "?";
}

const char* vy_type_name(VyValue v) { return vy_tag_name(vy_tagof(v)); }

VyValue vy_type_of(VyValue v) { return vy_str_val(vy_tag_name(vy_tagof(v))); }

/* keep the helpers referenced so -Wall stays quiet in release builds */
void vy__unused_helpers(void) {
  (void)as_s;
  (void)str_build;
}
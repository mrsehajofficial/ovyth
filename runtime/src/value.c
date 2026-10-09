/* Ovyth runtime :: src/value.c  --  operators, comparison, truthiness. */
#include "ovrt.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- numbers */

static double as_f(OvValue v) {
  switch (ov_tagof(v)) {
    case OV_INT:   return (double)v.i;
    case OV_FLOAT: return v.f;
    case OV_BOOL:  return v.b ? 1.0 : 0.0;
    default:       ov_type_error("a number", v);
  }
  return 0;
}

static int64_t as_i(OvValue v) {
  switch (ov_tagof(v)) {
    case OV_INT:   return v.i;
    case OV_FLOAT: return (int64_t)v.f;
    case OV_BOOL:  return v.b ? 1 : 0;
    default:       ov_type_error("an integer", v);
  }
  return 0;
}

static int is_num(OvValue v) {
  OvTag t = ov_tagof(v);
  return t == OV_INT || t == OV_FLOAT;
}

static int both_int(OvValue a, OvValue b) {
  return ov_tagof(a) == OV_INT && ov_tagof(b) == OV_INT;
}

/* ------------------------------------------------------------- strings */

static OvStr* as_s(OvValue v) {
  if (ov_tagof(v) != OV_STRING) ov_type_error("a string", v);
  return v.str;
}

/* Repeated `s + s` in a loop is the hottest string path in AI code (building
 * request bodies, joining log lines). Grow geometrically when the receiver is
 * the last allocation and is uniquely owned, otherwise copy. */
static OvStr* str_build(OvStr* a, const char* b, size_t blen) {
  size_t n = a->len + blen;
  OvStr* s = ov_str_new(a->bytes, n);
  memcpy(s->bytes + a->len, b, blen);
  return s;
}

OvValue ov_add_slow(OvValue a, OvValue b) {
  if (ov_tagof(a) == OV_STRING && ov_tagof(b) == OV_STRING)
    return ov_str(ov_str_concat(a.str, b.str));
  /* String coercion:  "n=" + 42  ==  "n=42" (render the non-string side). */
  if (ov_tagof(a) == OV_STRING)
    return ov_str(ov_str_concat(a.str, ov_render(b)));
  if (ov_tagof(b) == OV_STRING)
    return ov_str(ov_str_concat(ov_render(a), b.str));
  if (ov_tagof(a) == OV_LIST && ov_tagof(b) == OV_LIST) {
    OvList* out = ov_list_new_cap(a.list->len + b.list->len);
    memcpy(out->items, a.list->items, a.list->len * sizeof(OvValue));
    memcpy(out->items + a.list->len, b.list->items, b.list->len * sizeof(OvValue));
    out->len = a.list->len + b.list->len;
    return ov_list(out);
  }
  if (is_num(a) && is_num(b)) {
    if (both_int(a, b)) return ov_int(a.i + b.i);
    return ov_float(as_f(a) + as_f(b));
  }
  if (ov_tagof(a) == OV_NIL) return b;
  if (ov_tagof(b) == OV_NIL) return a;
  ov_type_error("two numbers, strings or lists", a);
  return ov_nil();
}

OvValue ov_sub_slow(OvValue a, OvValue b) {
  if (is_num(a) && is_num(b)) {
    if (both_int(a, b)) return ov_int(a.i - b.i);
    return ov_float(as_f(a) - as_f(b));
  }
  ov_type_error("two numbers", a);
  return ov_nil();
}

OvValue ov_mul_slow(OvValue a, OvValue b) {
  if (is_num(a) && is_num(b)) {
    if (both_int(a, b)) return ov_int(a.i * b.i);
    return ov_float(as_f(a) * as_f(b));
  }
  /* string repeat:  "-" * 3  ==  "---" */
  if (ov_tagof(a) == OV_STRING && ov_tagof(b) == OV_INT)
    return ov_str(ov_str_repeat(a.str, b.i));
  if (ov_tagof(a) == OV_INT && ov_tagof(b) == OV_STRING)
    return ov_str(ov_str_repeat(b.str, a.i));
  if (ov_tagof(a) == OV_LIST && ov_tagof(b) == OV_INT) {
    OvList* l = a.list;
    ov_gc_begin_mutation();
    OvList* out = ov_list_new_cap(l->len * (size_t)(b.i > 0 ? b.i : 0) + 4);
    for (int64_t i = 0; i < b.i; i++)
      for (uint32_t j = 0; j < l->len; j++) ov_list_push(out, l->items[j]);
    ov_gc_end_mutation();
    return ov_list(out);
  }

  ov_type_error("two numbers", a);
  return ov_nil();
}

OvValue ov_div_slow(OvValue a, OvValue b) {
  if (!is_num(a) || !is_num(b)) ov_type_error("two numbers", a);
  if (both_int(a, b)) {
    if (b.i == 0) ov_zero_error();
    return ov_int(a.i / b.i);
  }
  double d = as_f(b);
  if (d == 0.0) ov_zero_error();
  return ov_float(as_f(a) / d);
}

OvValue ov_mod_slow(OvValue a, OvValue b) {
  if (!is_num(a) || !is_num(b)) ov_type_error("two numbers", a);
  if (both_int(a, b)) {
    if (b.i == 0) ov_zero_error();
    return ov_int(a.i % b.i);
  }
  double d = as_f(b);
  if (d == 0.0) ov_zero_error();
  return ov_float(fmod(as_f(a), d));
}

OvValue ov_pow(OvValue a, OvValue b) {
  if (!is_num(a) || !is_num(b)) ov_type_error("two numbers", a);
  return ov_float(pow(as_f(a), as_f(b)));
}

/* ov_neg / ov_pos / ov_not / ov_truthy / ov_is are now static inline in
 * ovrt.h (P0 fast paths): the scalar case has to be visible to the optimiser
 * or every loop pays a call + tag dispatch per operator. */

OvValue ov_bitand_slow(OvValue a, OvValue b) { return ov_int(as_i(a) & as_i(b)); }
OvValue ov_bitxor_slow(OvValue a, OvValue b) { return ov_int(as_i(a) ^ as_i(b)); }
OvValue ov_bitor_slow(OvValue a, OvValue b)  { return ov_int(as_i(a) | as_i(b)); }
OvValue ov_lshift_slow(OvValue a, OvValue b) { return ov_int((int64_t)((uint64_t)as_i(a) << as_i(b))); }
OvValue ov_rshift_slow(OvValue a, OvValue b) { return ov_int(as_i(a) >> as_i(b)); }

/* ------------------------------------------------------------ equality */

int ov_eq_slow(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_FLOAT) return (double)a.i == b.f;
  if (ta == OV_FLOAT && tb == OV_INT) return a.f == (double)b.i;
  if (ta != tb) return 0;
  switch (ta) {
    case OV_NIL:   return 1;
    case OV_BOOL:  return (a.b != 0) == (b.b != 0);
    case OV_INT:   return a.i == b.i;
    case OV_FLOAT: return a.f == b.f;
    case OV_STRING:return ov_str_eq(a.str, b.str);
    case OV_LIST: {
      OvList* x = a.list;
      OvList* y = b.list;
      if (x == y) return 1;
      if (x->len != y->len) return 0;
      for (uint32_t i = 0; i < x->len; i++)
        if (!ov_eq(x->items[i], y->items[i])) return 0;
      return 1;
    }
    case OV_MAP:   return a.map == b.map;
    default:       return a.tag == b.tag;
  }
}

int ov_cmp_slow(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) return a.i < b.i ? -1 : (a.i > b.i);
  if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT)) {
    double x = as_f(a), y = as_f(b);
    return x < y ? -1 : (x > y);
  }
  if (ta == OV_STRING && tb == OV_STRING) return ov_str_cmp(a.str, b.str);
  if (ta == OV_LIST && tb == OV_LIST) {
    uint32_t n = a.list->len < b.list->len ? a.list->len : b.list->len;
    for (uint32_t i = 0; i < n; i++) {
      int c = ov_cmp(a.list->items[i], b.list->items[i]);
      if (c) return c;
    }
    return a.list->len < b.list->len ? -1 : (a.list->len > b.list->len);
  }
  /* nil sorts before everything, then by type tag for a stable order */
  if (ta == OV_NIL || tb == OV_NIL)
    return ta == tb ? 0 : (ta == OV_NIL ? -1 : 1);
  if (ta != tb) return ta < tb ? -1 : 1;
  return 0;
}

int ov_in(OvValue needle, OvValue haystack) {
  if (ov_tagof(haystack) == OV_LIST) return ov_list_contains(haystack.list, needle);
  if (ov_tagof(haystack) == OV_MAP)   return ov_map_has(haystack.map, needle);
  if (ov_tagof(haystack) == OV_STRING) {
    if (ov_tagof(needle) != OV_STRING) return 0;
    return ov_str_contains(haystack.str, needle.str);
  }
  return 0;
}

OvValue ov_range(int64_t lo, int64_t hi, int64_t step) {
  if (step == 0) ov_error("range step cannot be zero");
  int64_t n = step > 0 ? ((hi - lo + step - 1) / step) : ((hi - lo + step + 1) / step);
  if (n < 0) n = 0;
  OvList* l = ov_list_new_cap((uint32_t)n);
  for (int64_t i = 0; i < n; i++) ov_list_push(l, ov_int(lo + i * step));
  return ov_list(l);
}

/* --------------------------------------------------------------- types */

const char* ov_tag_name(OvTag t) {
  switch (t) {
    case OV_NIL:    return "nil";
    case OV_BOOL:   return "Bool";
    case OV_INT:    return "Int";
    case OV_FLOAT:  return "Float";
    case OV_STRING: return "String";
    case OV_LIST:   return "List";
    case OV_MAP:    return "Map";
    case OV_FUNC:   return "Function";
  }
  return "?";
}

const char* ov_type_name(OvValue v) { return ov_tag_name(ov_tagof(v)); }

OvValue ov_type_of(OvValue v) { return ov_str_val(ov_tag_name(ov_tagof(v))); }

/* keep the helpers referenced so -Wall stays quiet in release builds */
void ov__unused_helpers(void) {
  (void)as_s;
  (void)str_build;
}
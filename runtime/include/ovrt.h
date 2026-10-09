/* Ovyth runtime :: include/ovrt.h
 *
 * The C ABI every compiled Ovyth program links against.
 *
 * Value representation (ABI_V1)
 * -----------------------------
 *   nil      bool(false)      int      float     string    list    map
 *   TAG_NIL  TAG_BOOL         TAG_INT  TAG_FLOAT TAG_STRING TAG_LIST TAG_MAP
 *
 * Scalars are stored *by value* inside OvValue (never boxed), so a program
 * with no heap traffic has no allocations at all.
 *
 *   typedef struct OvValue {
 *     uint64_t tag;    // type tag ^ (heap pointer for tagged kinds)
 *     union {
 *       int64_t  i;
 *       double   f;
 *       OvStr*   str;
 *       OvList*  list;
 *       OvMap*   map;
 *       uint8_t  b;
 *     };
 *   } OvValue;
 *
 * Strings are immutable {len, bytes, hash}; lists and maps are GC-managed
 * ref-counted arrays. `tag` packs a 3-bit kind with 61 bits of payload, so a
 * tagged reference fits in one machine word -- the shape the compiler emits.
 */
#ifndef OVRT_H
#define OVRT_H

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ values */

typedef struct OvStr  OvStr;
typedef struct OvList OvList;
typedef struct OvMap  OvMap;
typedef struct OvFunc OvFunc;
typedef struct OvHeader OvHeader;
typedef struct OvInt64Array OvInt64Array;
typedef struct OvFloat64Array OvFloat64Array;
typedef struct OvStringArray OvStringArray;

typedef struct OvValue {
  uint64_t tag;
  union {
    int64_t i;
    double  f;
    OvStr*  str;
    OvList* list;
    OvMap*  map;
    OvFunc* fn;
    OvInt64Array*  i64a;
    OvFloat64Array* f64a;
    OvStringArray*  stra;
    uint8_t b;
  };
} OvValue;

/* Internal object header: every heap object starts with one, so the collector
 * can reach a OvStr (which lives inside its own allocation, header first) and
 * a OvList/OvMap (header inline) through one code path. Generated code never
 * reads these fields. */
typedef struct OvHeader {
  struct OvHeader* next;
  uint32_t len;     /* strings: payload length; lists/maps: spare        */
  uint32_t hash;    /* strings: cached hash                               */
  uint8_t  kind;    /* 0 string, 1 list, 2 map                            */
  uint8_t  marked;
  uint16_t pad;     /* bit 0 = OV_HDR_PIN, see below                      */
} OvHeader;

/* pad bit set on strings created by ov_str_lit(): generated code caches one
 * allocation per string-literal site and reuses it for every evaluation.
 * Their only root is a C static the collector cannot see, so sweep() keeps
 * them instead of freeing an object that is still very much in use. */
#define OV_HDR_PIN 1u

typedef enum OvTag {
  OV_NIL    = 0,
  OV_BOOL   = 1,
  OV_INT    = 2,
  OV_FLOAT  = 3,
  OV_STRING = 4,
  OV_LIST   = 5,
  OV_MAP    = 6,
  OV_FUNC   = 7,
  OV_I64A   = 8,   // Int64Array
  OV_F64A   = 9,   // Float64Array
  OV_STRA   = 10,  // StringArray
  OV_MASK   = 15ULL
} OvTag;

/* Reference tags pack a 3-bit kind with the 61-bit heap address, so a tagged
 * reference is a single machine word; the union below is the fast unboxed
 * access path used by generated code. */
#define OV_TAG_OF(p, k)  (((uint64_t)(uintptr_t)(p) << 3) | (uint64_t)(k))
#define OV_PTR_OF(v)     ((void*)(uintptr_t)((v).tag >> 3))
#define OV_MAX_DEPTH 512

static inline OvValue ov_nil(void)      { OvValue v; v.tag = OV_NIL;   v.i = 0; return v; }
static inline OvValue ov_bool(int b)     { OvValue v; v.tag = OV_BOOL;  v.i = 0; v.b = (uint8_t)(b != 0); return v; }
static inline OvValue ov_int(int64_t i)  { OvValue v; v.tag = OV_INT;   v.i = i; return v; }
static inline OvValue ov_float(double f) { OvValue v; v.tag = OV_FLOAT; v.i = 0; v.f = f; return v; }
static inline OvValue ov_str(OvStr* s)      { OvValue v; v.tag = OV_TAG_OF(s, OV_STRING); v.str = s; return v; }
static inline OvValue ov_list(OvList* l)    { OvValue v; v.tag = OV_TAG_OF(l, OV_LIST);   v.list = l; return v; }
static inline OvValue ov_map(OvMap* m)      { OvValue v; v.tag = OV_TAG_OF(m, OV_MAP);     v.map = m; return v; }
static inline OvValue ov_func(OvFunc* f)    { OvValue v; v.tag = OV_TAG_OF(f, OV_FUNC);   v.fn = f; return v; }

#define ov_tagof(v)  ((OvTag)((v).tag & OV_MASK))
#define ov_ishas(v, T) (ov_tagof(v) == (T))
#define ov_isnil(v)   (ov_tagof(v) == OV_NIL)

/* ------------------------------------------------------------- strings */

struct OvStr {
  uint32_t len;
  uint32_t hash;      /* FNV-1a over the bytes, cached                    */
  char    bytes[1];   /* NUL terminated for C interop                    */
};

/* Constructors. Returned OvStr* has refcount 1; ov_str() wraps it. */
OvStr* ov_str_new(const char* p, size_t n);
OvStr* ov_str_lit(const char* p, size_t n);   /* pinned immortal literal     */
OvStr* ov_str_cstr(const char* p);
OvValue ov_str_val(const char* p);    /* convenience: new + wrap              */
OvValue ov_str_val_n(const char* p, size_t n);
OvValue ov_str_of(const char* p, size_t n);   /* same, but takes (ptr,len) */

int64_t ov_str_len(OvStr* s);
const char* ov_str_data(OvStr* s);
uint32_t ov_str_hash(OvStr* s);
int     ov_str_eq(OvStr* a, OvStr* b);
int32_t ov_str_cmp(OvStr* a, OvStr* b);
OvStr*  ov_str_concat(OvStr* a, OvStr* b);
int     ov_str_contains(OvStr* hay, OvStr* needle);
int64_t ov_str_find(OvStr* hay, OvStr* needle, int64_t from);
OvStr*  ov_str_slice(OvStr* s, int64_t lo, int64_t hi);
OvStr*  ov_str_upper(OvStr* s);
OvStr*  ov_str_lower(OvStr* s);
OvStr*  ov_str_trim(OvStr* s);
OvStr*  ov_str_repeat(OvStr* s, int64_t n);
/* pad(s, width[, fill]): grow s to `width` by repeating `fill` (a space when
 * 0) on the left when `left`, on the right otherwise. Never truncates. */
OvStr*  ov_str_pad(OvStr* s, int64_t width, int64_t fill, int left);
OvStr*  ov_str_replace(OvStr* s, OvStr* from, OvStr* to);
OvValue ov_str_split(OvStr* s, OvStr* sep);
/* --- string builder ---
 * `acc = acc + "x"` in a loop copies the whole accumulator on every step, which
 * is quadratic in the total output length. The builder amortises that to a
 * single allocation per doubling: appends copy only the new bytes. The compiler
 * recognises the accumulate-in-a-loop pattern and lowers it to these calls;
 * using it directly is also fine. */
typedef struct OvStrBuilder OvStrBuilder;
OvStrBuilder* ov_sb_new(void);
void  ov_sb_free(OvStrBuilder* sb);
void  ov_sb_append(OvStrBuilder* sb, const char* p, size_t n);
void  ov_sb_append_str(OvStrBuilder* sb, OvStr* s);
void  ov_sb_append_value(OvStrBuilder* sb, OvValue v);  /* renders v */
size_t ov_sb_len(OvStrBuilder* sb);
OvStr* ov_sb_finish(OvStrBuilder* sb);   /* consumes the builder */

OvValue ov_str_chars(OvStr* s);   /* split into UTF-8 code point list     */
OvValue ov_str_bytes(OvStr* s);   /* split into UTF-8 byte list           */

/* ---------------------------------------------------------------- lists */

/* `hdr` must stay the FIRST field: the GC tracks &l->hdr as the allocation
 * base, and sweep() casts it back to OvList* to free the block. Moving it
 * anywhere else offsets the free() target into the middle of the malloc and
 * aborts. (Strings use the other layout -- header PREFIXED in the same block.
 * Both work; what is not allowed is the header living at a nonzero offset
 * inside a standalone calloc of the struct.) */
struct OvList {
  OvHeader hdr;
  uint32_t len;
  uint32_t cap;
  OvValue* items;
};

OvList* ov_list_new(void);
OvList* ov_list_new_cap(uint32_t cap);

/* P0 fast path: push runs once per element in every builder loop. The old
 * shape was two calls per push (ov_list_push -> list_grow even when there was
 * room). No allocation happens here and the collector only runs at allocation
 * points, so writing the slot before bumping len cannot be observed. */
void    ov_list_push_slow(OvList* l, OvValue v);   /* NULL or growth needed   */
static inline void ov_list_push(OvList* l, OvValue v) {
  if (l && l->len < l->cap) { l->items[l->len++] = v; return; }
  ov_list_push_slow(l, v);
}

OvValue ov_list_get_slow(OvList* l, int64_t i);    /* negative idx + errors   */
static inline OvValue ov_list_get(OvList* l, int64_t i) {
  if (l && i >= 0 && (uint64_t)i < (uint64_t)l->len) return l->items[i];
  return ov_list_get_slow(l, i);
}

void    ov_list_set(OvList* l, int64_t i, OvValue v);
int64_t ov_list_len(OvList* l);
OvValue ov_list_pop(OvList* l);
void    ov_list_insert(OvList* l, int64_t i, OvValue v);
int     ov_list_remove(OvList* l, int64_t i);
int64_t ov_list_index(OvList* l, OvValue v);
OvList* ov_list_copy(OvList* l);
void    ov_list_sort(OvList* l);
void    ov_list_reverse(OvList* l);
int     ov_list_contains(OvList* l, OvValue v);

/* ----------------------------------------------------------------- maps */

typedef struct OvPair {
  OvValue key;
  OvValue val;
} OvPair;

/* ---------------------------------------------------------- specialized arrays */

/* Specialized homogeneous arrays - contiguous native buffers for
 * known element types. These avoid boxing and provide cache-friendly
 * access patterns. NOT GC-tracked; must be freed explicitly. */
typedef struct OvInt64Array {
  int64_t* data;
  uint32_t len;
  uint32_t cap;
} OvInt64Array;

typedef struct OvFloat64Array {
  double* data;
  uint32_t len;
  uint32_t cap;
} OvFloat64Array;

typedef struct OvStringArray {
  OvStr** data;
  uint32_t len;
  uint32_t cap;
} OvStringArray;

/* Specialized array value wrappers - used by generated code */
extern OvValue ov_i64a_val(OvInt64Array* a);
extern OvValue ov_f64a_val(OvFloat64Array* a);
extern OvValue ov_stra_val(OvStringArray* a);

/* Forward declarations for slow paths */
void ov_i64a_push_slow(OvInt64Array* a, int64_t v);
int64_t ov_i64a_get_slow(OvInt64Array* a, int64_t i);
void ov_i64a_free(OvInt64Array* a);

void ov_f64a_push_slow(OvFloat64Array* a, double v);
double ov_f64a_get_slow(OvFloat64Array* a, int64_t i);
void ov_f64a_free(OvFloat64Array* a);

void ov_stra_push_slow(OvStringArray* a, OvStr* v);
OvStr* ov_stra_get_slow(OvStringArray* a, int64_t i);
void ov_stra_free(OvStringArray* a);

/* Int64Array operations */
OvInt64Array* ov_i64a_new_cap(uint32_t cap);
static inline OvInt64Array* ov_i64a_new(void) { return ov_i64a_new_cap(4); }
static inline void ov_i64a_push(OvInt64Array* a, int64_t v) {
  if (a && a->len < a->cap) { a->data[a->len++] = v; return; }
  ov_i64a_push_slow(a, v);
}
static inline int64_t ov_i64a_get(OvInt64Array* a, int64_t i) {
  if (a && i >= 0 && (uint64_t)i < (uint64_t)a->len) return a->data[i];
  return ov_i64a_get_slow(a, i);
}
static inline int64_t ov_i64a_pop(OvInt64Array* a) {
  if (!a || a->len == 0) return 0;
  return a->data[--a->len];
}
static inline void ov_i64a_set(OvInt64Array* a, int64_t i, int64_t v) {
  if (a && i >= 0 && (uint64_t)i < (uint64_t)a->len) a->data[i] = v;
}

/* Float64Array operations */
OvFloat64Array* ov_f64a_new_cap(uint32_t cap);
static inline OvFloat64Array* ov_f64a_new(void) { return ov_f64a_new_cap(4); }
static inline void ov_f64a_push(OvFloat64Array* a, double v) {
  if (a && a->len < a->cap) { a->data[a->len++] = v; return; }
  ov_f64a_push_slow(a, v);
}
static inline double ov_f64a_get(OvFloat64Array* a, int64_t i) {
  if (a && i >= 0 && (uint64_t)i < (uint64_t)a->len) return a->data[i];
  return ov_f64a_get_slow(a, i);
}
static inline double ov_f64a_pop(OvFloat64Array* a) {
  if (!a || a->len == 0) return 0.0;
  return a->data[--a->len];
}
static inline void ov_f64a_set(OvFloat64Array* a, int64_t i, double v) {
  if (a && i >= 0 && (uint64_t)i < (uint64_t)a->len) a->data[i] = v;
}

/* StringArray operations */
OvStringArray* ov_stra_new_cap(uint32_t cap);
static inline OvStringArray* ov_stra_new(void) { return ov_stra_new_cap(4); }
static inline void ov_stra_push(OvStringArray* a, OvStr* v) {
  if (a && a->len < a->cap) { a->data[a->len++] = v; return; }
  ov_stra_push_slow(a, v);
}
static inline OvStr* ov_stra_get(OvStringArray* a, int64_t i) {
  if (a && i >= 0 && (uint64_t)i < (uint64_t)a->len) return a->data[i];
  return ov_stra_get_slow(a, i);
}
static inline OvStr* ov_stra_pop(OvStringArray* a) {
  if (!a || a->len == 0) return NULL;
  return a->data[--a->len];
}
static inline void ov_stra_set(OvStringArray* a, int64_t i, OvStr* v) {
  if (a && i >= 0 && (uint64_t)i < (uint64_t)a->len) a->data[i] = v;
}

/* `hdr` first -- see the OvList note above: the GC tracks &m->hdr as the
 * block base, so it must sit at offset 0. */
struct OvMap {
  OvHeader hdr;
  uint32_t len;
  uint32_t cap;
  OvPair*  entries;   /* open addressing; cap is a power of two          */
};

OvMap*  ov_map_new(void);
OvValue ov_map_get(OvMap* m, OvValue key);      /* nil when absent          */
void    ov_map_set(OvMap* m, OvValue key, OvValue v);
int     ov_map_has(OvMap* m, OvValue key);
int     ov_map_del(OvMap* m, OvValue key);
int64_t ov_map_len(OvMap* m);
OvValue ov_map_keys(OvMap* m);
OvValue ov_map_values(OvMap* m);
void    ov_map_clear(OvMap* m);
OvList* ov_map_pairs(OvMap* m);                 /* [k1, v1, k2, v2, ...]    */

/* --------------------------------------------------------------- basics */

/* --------------------------------------------------------- panics */

/* `try { } catch e { }` unwinds with setjmp/longjmp into the frame that
 * entered the block. Both backends emit the same shape:
 *
 *     if (setjmp(*ov_try_push()) == 0) {
 *         ...body...
 *     } else {
 *         err = ov_caught;
 *     }
 *     ov_try_pop();
 *
 * A panic with no enclosing try reports on stderr and exits 70. */
jmp_buf* ov_try_push(void);
void     ov_try_pop(void);
int      ov_try_active(void);        /* inside a try block?                */
extern OvValue ov_caught;            /* value delivered by the last panic  */
/* Bracket an in-place container update (map rehash, list growth) so the
 * collector cannot run while the container is half-rebuilt. Not reentrant-
 * unsafe: the counter nests. */
void ov_gc_begin_mutation(void);
void ov_gc_end_mutation(void);

void ov_gc_disable(int on);
int  ov_gc_enabled(void);

/* Program entry/exit, called by generated code. */
void ov_runtime_init(int argc, char** argv);
void ov_runtime_shutdown(void);
int  ov_exit_code(void);
void ov_request_exit(int code);

/* --------------------------------------------------------------- basics */

/* P0 fast paths -- docs/PERFORMANCE-SPEC.md section 44.
 *
 * Every operator in generated code and in the interpreter funnels through
 * these functions. As out-of-line calls into libovrt.a (which is not LTO),
 * each one cost a real call plus ~7 tag tests before reaching int+int, and
 * no optimiser could see across that boundary -- this was the single largest
 * performance item in the language. The scalar cases now live here, where
 * clang sees constant tags, folds, and keeps values in registers; the slow
 * bodies in value.c are the original implementations, unchanged, and handle
 * strings, lists, coercions and type errors exactly as before. */

/* declared again in the panics section below */
void ov_type_error(const char* want, OvValue got) __attribute__((noreturn));
void ov_zero_error(void) __attribute__((noreturn));

static inline int    ov_isnum(OvValue v) { OvTag t = ov_tagof(v); return t == OV_INT || t == OV_FLOAT; }
static inline double ov_tof(OvValue v)   { return ov_tagof(v) == OV_INT ? (double)v.i : v.f; }

OvValue ov_add_slow(OvValue a, OvValue b);   /* strings, lists, nil identity  */
OvValue ov_sub_slow(OvValue a, OvValue b);
OvValue ov_mul_slow(OvValue a, OvValue b);   /* string/list repeat            */
OvValue ov_div_slow(OvValue a, OvValue b);
OvValue ov_mod_slow(OvValue a, OvValue b);   /* float fmod + type errors      */
OvValue ov_bitand_slow(OvValue a, OvValue b);
OvValue ov_bitxor_slow(OvValue a, OvValue b);
OvValue ov_bitor_slow(OvValue a, OvValue b);
OvValue ov_lshift_slow(OvValue a, OvValue b);
OvValue ov_rshift_slow(OvValue a, OvValue b);
int     ov_eq_slow(OvValue a, OvValue b);
int     ov_cmp_slow(OvValue a, OvValue b);

static inline int ov_truthy(OvValue v) {
  switch (ov_tagof(v)) {
    case OV_NIL:    return 0;
    case OV_BOOL:   return v.b != 0;
    case OV_INT:    return v.i != 0;
    case OV_FLOAT:  return v.f != 0.0;
    case OV_STRING: return v.str->len != 0;
    case OV_LIST:   return v.list->len != 0;
    case OV_MAP:    return v.map->len != 0;
    default:        return 1;
  }
}

static inline OvValue ov_add(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) return ov_int(a.i + b.i);
  if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT))
    return ov_float(ov_tof(a) + ov_tof(b));
  return ov_add_slow(a, b);
}

static inline OvValue ov_sub(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) return ov_int(a.i - b.i);
  if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT))
    return ov_float(ov_tof(a) - ov_tof(b));
  return ov_sub_slow(a, b);
}

static inline OvValue ov_mul(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) return ov_int(a.i * b.i);
  if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT))
    return ov_float(ov_tof(a) * ov_tof(b));
  return ov_mul_slow(a, b);
}

static inline OvValue ov_div(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) {
    if (b.i == 0) ov_zero_error();
    return ov_int(a.i / b.i);
  }
  if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT)) {
    double d = ov_tof(b);
    if (d == 0.0) ov_zero_error();
    return ov_float(ov_tof(a) / d);
  }
  return ov_div_slow(a, b);
}

static inline OvValue ov_mod(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) {
    if (b.i == 0) ov_zero_error();
    return ov_int(a.i % b.i);
  }
  return ov_mod_slow(a, b);   /* float path calls fmod() */
}

OvValue ov_pow(OvValue a, OvValue b);   /* always Float; libm pow */

static inline OvValue ov_neg(OvValue a) {
  if (ov_tagof(a) == OV_INT)   return ov_int(-a.i);
  if (ov_tagof(a) == OV_FLOAT) return ov_float(-a.f);
  ov_type_error("a number", a);
  return ov_nil();
}

static inline OvValue ov_pos(OvValue a) {
  if (ov_isnum(a)) return a;
  ov_type_error("a number", a);
  return ov_nil();
}

static inline OvValue ov_not(OvValue a) { return ov_bool(!ov_truthy(a)); }

static inline OvValue ov_bitand(OvValue a, OvValue b) {
  if (ov_tagof(a) == OV_INT && ov_tagof(b) == OV_INT) return ov_int(a.i & b.i);
  return ov_bitand_slow(a, b);
}
static inline OvValue ov_bitxor(OvValue a, OvValue b) {
  if (ov_tagof(a) == OV_INT && ov_tagof(b) == OV_INT) return ov_int(a.i ^ b.i);
  return ov_bitxor_slow(a, b);
}
static inline OvValue ov_bitor(OvValue a, OvValue b) {
  if (ov_tagof(a) == OV_INT && ov_tagof(b) == OV_INT) return ov_int(a.i | b.i);
  return ov_bitor_slow(a, b);
}
static inline OvValue ov_lshift(OvValue a, OvValue b) {
  if (ov_tagof(a) == OV_INT && ov_tagof(b) == OV_INT)
    return ov_int((int64_t)((uint64_t)a.i << b.i));
  return ov_lshift_slow(a, b);
}
static inline OvValue ov_rshift(OvValue a, OvValue b) {
  if (ov_tagof(a) == OV_INT && ov_tagof(b) == OV_INT) return ov_int(a.i >> b.i);
  return ov_rshift_slow(a, b);
}

static inline int ov_is(OvValue a, OvTag t) { return ov_tagof(a) == t; }

int     ov_in(OvValue needle, OvValue haystack);
OvValue ov_range(int64_t lo, int64_t hi, int64_t step);

static inline int ov_eq(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == tb) {
    switch (ta) {
      case OV_NIL:   return 1;
      case OV_BOOL:  return (a.b != 0) == (b.b != 0);
      case OV_INT:   return a.i == b.i;
      case OV_FLOAT: return a.f == b.f;
      default:       break;    /* string / list / map / func: slow */
    }
  } else if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT)) {
    return ov_tof(a) == ov_tof(b);
  }
  return ov_eq_slow(a, b);
}

static inline int ov_cmp(OvValue a, OvValue b) {
  OvTag ta = ov_tagof(a), tb = ov_tagof(b);
  if (ta == OV_INT && tb == OV_INT) return a.i < b.i ? -1 : (a.i > b.i);
  if ((ta == OV_INT || ta == OV_FLOAT) && (tb == OV_INT || tb == OV_FLOAT)) {
    double x = ov_tof(a), y = ov_tof(b);
    return x < y ? -1 : (x > y);
  }
  return ov_cmp_slow(a, b);
}

const char* ov_type_name(OvValue v);
const char* ov_tag_name(OvTag t);
OvValue ov_type_of(OvValue v);

/* ------------------------------------------------------------- console */

void     ov_print(OvStr* s);
void     ov_print_nl(OvStr* s);
void     ov_print_err(OvStr* s);
OvStr*   ov_read_line(void);      /* NULL at end of input               */
void     ov_flush(void);

/* print() / string rendering. A bare string prints unquoted; everything else
 * renders as JSON with nested strings quoted. */
OvStr*   ov_render(OvValue v);
OvStr*   ov_repr(OvValue v);      /* always valid JSON                  */

/* --------------------------------------------------------- environment */

OvStr*   ov_env_get(const char* name);      /* NULL when unset              */
OvValue  ov_env_get_val(const char* name);  /* nil when unset              */
void     ov_env_set(const char* name, OvStr* value);

/* -------------------------------------------------------------- args */

extern int    g_argc;
extern char** g_argv;
int       ov_argc(void);
const char* ov_argv(int i);
OvValue   ov_args(void);

/* ---------------------------------------------------------- json */

OvValue ov_json_parse(const char* text, size_t len);   /* nil on error     */
OvStr*  ov_json_stringify(OvValue v);                  /* caller frees     */
int     ov_json_valid(const char* text, size_t len);
const char* ov_json_error_slot(void);

/* ---------------------------------------------------------- http */

typedef struct OvHttpResponse OvHttpResponse;

struct OvHttpResponse {
  int64_t status;
  OvStr*  body;
  OvValue headers;      /* map of lowercase header -> string (array if dup) */
  OvStr*  error;        /* non-NULL when the request itself failed          */
  double  elapsed_ms;
};

/* method: one of "GET" "POST" "PUT" "PATCH" "DELETE" "HEAD"
 * url, body: NULL-terminated UTF-8 (may be NULL)
 * headers_json / params_json: JSON object strings, or NULL
 * returns NULL only on allocation failure; transport errors come back as
 * response.error so the language can `try/catch` them like any other value. */
OvHttpResponse* ov_http_request(const char* method, const char* url,
                                const char* body, const char* content_type,
                                const char* headers_json, const char* params_json,
                                double timeout_s);

const char* ov_http_libcurl_version(void);

/* --------------------------------------------------- user functions */

struct OvFunc;
typedef OvValue (*OvFnPtr)(struct OvFunc* fn, OvValue* args, int argc);
struct OvFunc {
  OvFnPtr   call;
  OvStr*    name;
  int       arity;
  int       variadic;
  void*     upvals;      /* opaque closure environment / upvalues */
};

/* ------------------------------------------------------- gc / memory */

/* A precise, non-moving mark-sweep collector over all three heap kinds
 * (string, list, map) -- see docs/memory-model.md. Roots are the registered
 * value slots (the backends' shadow stacks) plus whatever the backend passes
 * explicitly to ov_gc_collect_ex(). */
void  ov_gc_collect(void);
void  ov_gc_register_root(OvValue* slot);
void  ov_gc_unregister_root(OvValue* slot);
size_t ov_gc_roots_mark(void);
void   ov_gc_roots_restore(size_t mark);
typedef void (*OvGcScanner)(void);
void  ov_gc_set_scanner(OvGcScanner s);
void  ov_gc_mark_value(OvValue v);
void  ov_gc_collect_ex(OvValue* extra_roots, int n);
size_t ov_gc_live_bytes(void);
size_t ov_gc_heap_bytes(void);
size_t ov_heap_alloc_count(void);
void  ov_heap_reset_stats(void);
void ov_heap_note_realloc(size_t old_bytes, size_t new_bytes);
void ov_heap_track_str(OvStr* s, size_t len);
void ov_heap_track_list(OvList* l, size_t bytes);
void ov_heap_track_map(OvMap* m, size_t bytes);
uint32_t ov_str_hash_n(const char* p, size_t n);

/* ------------------------------------------------------------ panics */

void ov_error(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void ov_throw_value(OvValue v) __attribute__((noreturn));
void ov_throw_str(OvStr* s) __attribute__((noreturn));
void ov_type_error(const char* want, OvValue got) __attribute__((noreturn));
void ov_index_error(const char* what, int64_t idx, int64_t len) __attribute__((noreturn));
void ov_zero_error(void) __attribute__((noreturn));

/* pretty diagnostics: emitted by compiled code for `breakpoint` */
void ov_debug_note(const char* file, int line);

/* top-level panic result for compiled programs: the exit() code,
 * or 70 after reporting a throw that escaped every try */
int ov_uncaught_code(void);

/* -------------------------------------------------------- arena allocator */

/* Request-scoped region allocator.  All objects allocated from an arena are
 * freed in O(1) by ov_arena_reset() / ov_arena_free().  Arena strings are NOT
 * GC-tracked; they must not be stored in long-lived GC containers.          */
typedef struct OvArena OvArena;

OvArena* ov_arena_new(size_t block_size);   /* 0 -> 64 KB default          */
void*    ov_arena_alloc(OvArena* a, size_t n);
void*    ov_arena_calloc(OvArena* a, size_t n);
char*    ov_arena_strdup(OvArena* a, const char* s, size_t n);
OvStr*   ov_arena_str(OvArena* a, const char* p, size_t n);
void     ov_arena_reset(OvArena* a);   /* free all allocs, keep block       */
void     ov_arena_free(OvArena* a);    /* free everything incl. struct      */
size_t   ov_arena_used(OvArena* a);
size_t   ov_arena_peak(OvArena* a);

/* ------------------------------------------------- fast / SIMD JSON */

/* Drop-in replacement for ov_json_parse with optional arena for temporaries. */
OvValue ov_json_parse_fast(const char* text, size_t len, OvArena* arena);

/* Fast field extraction without building full AST. */
OvValue ov_json_extract_field(const char* json, size_t len, const char* key);

/* Typed field accessors -- avoid building intermediate OvValues. */
int     ov_json_get_str  (OvValue obj, const char* key, const char** out_data, size_t* out_len);
int     ov_json_get_int  (OvValue obj, const char* key, int64_t* out);
int     ov_json_get_float(OvValue obj, const char* key, double* out);
OvValue ov_json_get_array(OvValue obj, const char* key);

/* AI-specific chat/tool-call helpers. */
OvValue ov_json_chat_content(const char* body, size_t len);
int     ov_json_tool_call(const char* body, size_t len,
                          OvValue* out_name, OvValue* out_args);

/* --------------------------------------------------- HTTP connection pool */

typedef struct OvHttpPool OvHttpPool;

OvHttpPool*     ov_http_pool_new(int max_conns);   /* 0 -> 8 default        */
void            ov_http_pool_free(OvHttpPool* p);
OvHttpResponse* ov_http_pool_request(OvHttpPool* pool,
                                     const char* method, const char* url,
                                     const char* body, const char* content_type,
                                     const char* headers_json,
                                     const char* params_json,
                                     double timeout_s);
OvHttpPool*     ov_http_default_pool(void);   /* lazily-created global pool */
void            ov_http_pool_cleanup(void);   /* free global pool           */

#ifdef __cplusplus
}
#endif

#endif /* OVRT_H */
/* Vayu runtime :: include/vyrt.h
 *
 * The C ABI every compiled Vayu program links against.
 *
 * Value representation (ABI_V1)
 * -----------------------------
 *   nil      bool(false)      int      float     string    list    map
 *   TAG_NIL  TAG_BOOL         TAG_INT  TAG_FLOAT TAG_STRING TAG_LIST TAG_MAP
 *
 * Scalars are stored *by value* inside VyValue (never boxed), so a program
 * with no heap traffic has no allocations at all.
 *
 *   typedef struct VyValue {
 *     uint64_t tag;    // type tag ^ (heap pointer for tagged kinds)
 *     union {
 *       int64_t  i;
 *       double   f;
 *       VyStr*   str;
 *       VyList*  list;
 *       VyMap*   map;
 *       uint8_t  b;
 *     };
 *   } VyValue;
 *
 * Strings are immutable {len, bytes, hash}; lists and maps are GC-managed
 * ref-counted arrays. `tag` packs a 3-bit kind with 61 bits of payload, so a
 * tagged reference fits in one machine word -- the shape the compiler emits.
 */
#ifndef VYRT_H
#define VYRT_H

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ values */

typedef struct VyStr  VyStr;
typedef struct VyList VyList;
typedef struct VyMap  VyMap;
typedef struct VyFunc VyFunc;
typedef struct VyHeader VyHeader;

typedef struct VyValue {
  uint64_t tag;
  union {
    int64_t i;
    double  f;
    VyStr*  str;
    VyList* list;
    VyMap*  map;
    VyFunc* fn;
    uint8_t b;
  };
} VyValue;

/* Internal object header: every heap object starts with one, so the collector
 * can reach a VyStr (which lives inside its own allocation, header first) and
 * a VyList/VyMap (header inline) through one code path. Generated code never
 * reads these fields. */
typedef struct VyHeader {
  struct VyHeader* next;
  uint32_t len;     /* strings: payload length; lists/maps: spare        */
  uint32_t hash;    /* strings: cached hash                               */
  uint8_t  kind;    /* 0 string, 1 list, 2 map                            */
  uint8_t  marked;
  uint16_t pad;     /* bit 0 = VY_HDR_PIN, see below                      */
} VyHeader;

/* pad bit set on strings created by vy_str_lit(): generated code caches one
 * allocation per string-literal site and reuses it for every evaluation.
 * Their only root is a C static the collector cannot see, so sweep() keeps
 * them instead of freeing an object that is still very much in use. */
#define VY_HDR_PIN 1u

typedef enum VyTag {
  VY_NIL    = 0,
  VY_BOOL   = 1,
  VY_INT    = 2,
  VY_FLOAT  = 3,
  VY_STRING = 4,
  VY_LIST   = 5,
  VY_MAP    = 6,
  VY_FUNC   = 7,
  VY_MASK   = 7ULL
} VyTag;

/* Reference tags pack a 3-bit kind with the 61-bit heap address, so a tagged
 * reference is a single machine word; the union below is the fast unboxed
 * access path used by generated code. */
#define VY_TAG_OF(p, k)  (((uint64_t)(uintptr_t)(p) << 3) | (uint64_t)(k))
#define VY_PTR_OF(v)     ((void*)(uintptr_t)((v).tag >> 3))
#define VY_MAX_DEPTH 512

static inline VyValue vy_nil(void)      { VyValue v; v.tag = VY_NIL;   v.i = 0; return v; }
static inline VyValue vy_bool(int b)     { VyValue v; v.tag = VY_BOOL;  v.i = 0; v.b = (uint8_t)(b != 0); return v; }
static inline VyValue vy_int(int64_t i)  { VyValue v; v.tag = VY_INT;   v.i = i; return v; }
static inline VyValue vy_float(double f) { VyValue v; v.tag = VY_FLOAT; v.i = 0; v.f = f; return v; }
static inline VyValue vy_str(VyStr* s)      { VyValue v; v.tag = VY_TAG_OF(s, VY_STRING); v.str = s; return v; }
static inline VyValue vy_list(VyList* l)    { VyValue v; v.tag = VY_TAG_OF(l, VY_LIST);   v.list = l; return v; }
static inline VyValue vy_map(VyMap* m)      { VyValue v; v.tag = VY_TAG_OF(m, VY_MAP);     v.map = m; return v; }
static inline VyValue vy_func(VyFunc* f)    { VyValue v; v.tag = VY_TAG_OF(f, VY_FUNC);   v.fn = f; return v; }

#define vy_tagof(v)  ((VyTag)((v).tag & VY_MASK))
#define vy_ishas(v, T) (vy_tagof(v) == (T))
#define vy_isnil(v)   (vy_tagof(v) == VY_NIL)

/* ------------------------------------------------------------- strings */

struct VyStr {
  uint32_t len;
  uint32_t hash;      /* FNV-1a over the bytes, cached                    */
  char    bytes[1];   /* NUL terminated for C interop                    */
};

/* Constructors. Returned VyStr* has refcount 1; vy_str() wraps it. */
VyStr* vy_str_new(const char* p, size_t n);
VyStr* vy_str_lit(const char* p, size_t n);   /* pinned immortal literal     */
VyStr* vy_str_cstr(const char* p);
VyValue vy_str_val(const char* p);    /* convenience: new + wrap              */
VyValue vy_str_val_n(const char* p, size_t n);
VyValue vy_str_of(const char* p, size_t n);   /* same, but takes (ptr,len) */

int64_t vy_str_len(VyStr* s);
const char* vy_str_data(VyStr* s);
uint32_t vy_str_hash(VyStr* s);
int     vy_str_eq(VyStr* a, VyStr* b);
int32_t vy_str_cmp(VyStr* a, VyStr* b);
VyStr*  vy_str_concat(VyStr* a, VyStr* b);
int     vy_str_contains(VyStr* hay, VyStr* needle);
int64_t vy_str_find(VyStr* hay, VyStr* needle, int64_t from);
VyStr*  vy_str_slice(VyStr* s, int64_t lo, int64_t hi);
VyStr*  vy_str_upper(VyStr* s);
VyStr*  vy_str_lower(VyStr* s);
VyStr*  vy_str_trim(VyStr* s);
VyStr*  vy_str_repeat(VyStr* s, int64_t n);
/* pad(s, width[, fill]): grow s to `width` by repeating `fill` (a space when
 * 0) on the left when `left`, on the right otherwise. Never truncates. */
VyStr*  vy_str_pad(VyStr* s, int64_t width, int64_t fill, int left);
VyStr*  vy_str_replace(VyStr* s, VyStr* from, VyStr* to);
VyValue vy_str_split(VyStr* s, VyStr* sep);
/* --- string builder ---
 * `acc = acc + "x"` in a loop copies the whole accumulator on every step, which
 * is quadratic in the total output length. The builder amortises that to a
 * single allocation per doubling: appends copy only the new bytes. The compiler
 * recognises the accumulate-in-a-loop pattern and lowers it to these calls;
 * using it directly is also fine. */
typedef struct VyStrBuilder VyStrBuilder;
VyStrBuilder* vy_sb_new(void);
void  vy_sb_free(VyStrBuilder* sb);
void  vy_sb_append(VyStrBuilder* sb, const char* p, size_t n);
void  vy_sb_append_str(VyStrBuilder* sb, VyStr* s);
void  vy_sb_append_value(VyStrBuilder* sb, VyValue v);  /* renders v */
size_t vy_sb_len(VyStrBuilder* sb);
VyStr* vy_sb_finish(VyStrBuilder* sb);   /* consumes the builder */

VyValue vy_str_chars(VyStr* s);   /* split into UTF-8 code point list     */
VyValue vy_str_bytes(VyStr* s);   /* split into UTF-8 byte list           */

/* ---------------------------------------------------------------- lists */

/* `hdr` must stay the FIRST field: the GC tracks &l->hdr as the allocation
 * base, and sweep() casts it back to VyList* to free the block. Moving it
 * anywhere else offsets the free() target into the middle of the malloc and
 * aborts. (Strings use the other layout -- header PREFIXED in the same block.
 * Both work; what is not allowed is the header living at a nonzero offset
 * inside a standalone calloc of the struct.) */
struct VyList {
  VyHeader hdr;
  uint32_t len;
  uint32_t cap;
  VyValue* items;
};

VyList* vy_list_new(void);
VyList* vy_list_new_cap(uint32_t cap);

/* P0 fast path: push runs once per element in every builder loop. The old
 * shape was two calls per push (vy_list_push -> list_grow even when there was
 * room). No allocation happens here and the collector only runs at allocation
 * points, so writing the slot before bumping len cannot be observed. */
void    vy_list_push_slow(VyList* l, VyValue v);   /* NULL or growth needed   */
static inline void vy_list_push(VyList* l, VyValue v) {
  if (l && l->len < l->cap) { l->items[l->len++] = v; return; }
  vy_list_push_slow(l, v);
}

VyValue vy_list_get_slow(VyList* l, int64_t i);    /* negative idx + errors   */
static inline VyValue vy_list_get(VyList* l, int64_t i) {
  if (l && i >= 0 && (uint64_t)i < (uint64_t)l->len) return l->items[i];
  return vy_list_get_slow(l, i);
}

void    vy_list_set(VyList* l, int64_t i, VyValue v);
int64_t vy_list_len(VyList* l);
VyValue vy_list_pop(VyList* l);
void    vy_list_insert(VyList* l, int64_t i, VyValue v);
int     vy_list_remove(VyList* l, int64_t i);
int64_t vy_list_index(VyList* l, VyValue v);
VyList* vy_list_copy(VyList* l);
void    vy_list_sort(VyList* l);
void    vy_list_reverse(VyList* l);
int     vy_list_contains(VyList* l, VyValue v);

/* ----------------------------------------------------------------- maps */

typedef struct VyPair {
  VyValue key;
  VyValue val;
} VyPair;

/* `hdr` first -- see the VyList note above: the GC tracks &m->hdr as the
 * block base, so it must sit at offset 0. */
struct VyMap {
  VyHeader hdr;
  uint32_t len;
  uint32_t cap;
  VyPair*  entries;   /* open addressing; cap is a power of two          */
};

VyMap*  vy_map_new(void);
VyValue vy_map_get(VyMap* m, VyValue key);      /* nil when absent          */
void    vy_map_set(VyMap* m, VyValue key, VyValue v);
int     vy_map_has(VyMap* m, VyValue key);
int     vy_map_del(VyMap* m, VyValue key);
int64_t vy_map_len(VyMap* m);
VyValue vy_map_keys(VyMap* m);
VyValue vy_map_values(VyMap* m);
void    vy_map_clear(VyMap* m);
VyList* vy_map_pairs(VyMap* m);                 /* [k1, v1, k2, v2, ...]    */

/* --------------------------------------------------------------- basics */

/* --------------------------------------------------------- panics */

/* `try { } catch e { }` unwinds with setjmp/longjmp into the frame that
 * entered the block. Both backends emit the same shape:
 *
 *     if (setjmp(*vy_try_push()) == 0) {
 *         ...body...
 *     } else {
 *         err = vy_caught;
 *     }
 *     vy_try_pop();
 *
 * A panic with no enclosing try reports on stderr and exits 70. */
jmp_buf* vy_try_push(void);
void     vy_try_pop(void);
int      vy_try_active(void);        /* inside a try block?                */
extern VyValue vy_caught;            /* value delivered by the last panic  */
/* Bracket an in-place container update (map rehash, list growth) so the
 * collector cannot run while the container is half-rebuilt. Not reentrant-
 * unsafe: the counter nests. */
void vy_gc_begin_mutation(void);
void vy_gc_end_mutation(void);

void vy_gc_disable(int on);
int  vy_gc_enabled(void);

/* Program entry/exit, called by generated code. */
void vy_runtime_init(int argc, char** argv);
void vy_runtime_shutdown(void);
int  vy_exit_code(void);
void vy_request_exit(int code);

/* --------------------------------------------------------------- basics */

/* P0 fast paths -- docs/PERFORMANCE-SPEC.md section 44.
 *
 * Every operator in generated code and in the interpreter funnels through
 * these functions. As out-of-line calls into libvyrt.a (which is not LTO),
 * each one cost a real call plus ~7 tag tests before reaching int+int, and
 * no optimiser could see across that boundary -- this was the single largest
 * performance item in the language. The scalar cases now live here, where
 * clang sees constant tags, folds, and keeps values in registers; the slow
 * bodies in value.c are the original implementations, unchanged, and handle
 * strings, lists, coercions and type errors exactly as before. */

/* declared again in the panics section below */
void vy_type_error(const char* want, VyValue got) __attribute__((noreturn));
void vy_zero_error(void) __attribute__((noreturn));

static inline int    vy_isnum(VyValue v) { VyTag t = vy_tagof(v); return t == VY_INT || t == VY_FLOAT; }
static inline double vy_tof(VyValue v)   { return vy_tagof(v) == VY_INT ? (double)v.i : v.f; }

VyValue vy_add_slow(VyValue a, VyValue b);   /* strings, lists, nil identity  */
VyValue vy_sub_slow(VyValue a, VyValue b);
VyValue vy_mul_slow(VyValue a, VyValue b);   /* string/list repeat            */
VyValue vy_div_slow(VyValue a, VyValue b);
VyValue vy_mod_slow(VyValue a, VyValue b);   /* float fmod + type errors      */
VyValue vy_bitand_slow(VyValue a, VyValue b);
VyValue vy_bitxor_slow(VyValue a, VyValue b);
VyValue vy_bitor_slow(VyValue a, VyValue b);
VyValue vy_lshift_slow(VyValue a, VyValue b);
VyValue vy_rshift_slow(VyValue a, VyValue b);
int     vy_eq_slow(VyValue a, VyValue b);
int     vy_cmp_slow(VyValue a, VyValue b);

static inline int vy_truthy(VyValue v) {
  switch (vy_tagof(v)) {
    case VY_NIL:    return 0;
    case VY_BOOL:   return v.b != 0;
    case VY_INT:    return v.i != 0;
    case VY_FLOAT:  return v.f != 0.0;
    case VY_STRING: return v.str->len != 0;
    case VY_LIST:   return v.list->len != 0;
    case VY_MAP:    return v.map->len != 0;
    default:        return 1;
  }
}

static inline VyValue vy_add(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) return vy_int(a.i + b.i);
  if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT))
    return vy_float(vy_tof(a) + vy_tof(b));
  return vy_add_slow(a, b);
}

static inline VyValue vy_sub(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) return vy_int(a.i - b.i);
  if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT))
    return vy_float(vy_tof(a) - vy_tof(b));
  return vy_sub_slow(a, b);
}

static inline VyValue vy_mul(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) return vy_int(a.i * b.i);
  if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT))
    return vy_float(vy_tof(a) * vy_tof(b));
  return vy_mul_slow(a, b);
}

static inline VyValue vy_div(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) {
    if (b.i == 0) vy_zero_error();
    return vy_int(a.i / b.i);
  }
  if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT)) {
    double d = vy_tof(b);
    if (d == 0.0) vy_zero_error();
    return vy_float(vy_tof(a) / d);
  }
  return vy_div_slow(a, b);
}

static inline VyValue vy_mod(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) {
    if (b.i == 0) vy_zero_error();
    return vy_int(a.i % b.i);
  }
  return vy_mod_slow(a, b);   /* float path calls fmod() */
}

VyValue vy_pow(VyValue a, VyValue b);   /* always Float; libm pow */

static inline VyValue vy_neg(VyValue a) {
  if (vy_tagof(a) == VY_INT)   return vy_int(-a.i);
  if (vy_tagof(a) == VY_FLOAT) return vy_float(-a.f);
  vy_type_error("a number", a);
  return vy_nil();
}

static inline VyValue vy_pos(VyValue a) {
  if (vy_isnum(a)) return a;
  vy_type_error("a number", a);
  return vy_nil();
}

static inline VyValue vy_not(VyValue a) { return vy_bool(!vy_truthy(a)); }

static inline VyValue vy_bitand(VyValue a, VyValue b) {
  if (vy_tagof(a) == VY_INT && vy_tagof(b) == VY_INT) return vy_int(a.i & b.i);
  return vy_bitand_slow(a, b);
}
static inline VyValue vy_bitxor(VyValue a, VyValue b) {
  if (vy_tagof(a) == VY_INT && vy_tagof(b) == VY_INT) return vy_int(a.i ^ b.i);
  return vy_bitxor_slow(a, b);
}
static inline VyValue vy_bitor(VyValue a, VyValue b) {
  if (vy_tagof(a) == VY_INT && vy_tagof(b) == VY_INT) return vy_int(a.i | b.i);
  return vy_bitor_slow(a, b);
}
static inline VyValue vy_lshift(VyValue a, VyValue b) {
  if (vy_tagof(a) == VY_INT && vy_tagof(b) == VY_INT)
    return vy_int((int64_t)((uint64_t)a.i << b.i));
  return vy_lshift_slow(a, b);
}
static inline VyValue vy_rshift(VyValue a, VyValue b) {
  if (vy_tagof(a) == VY_INT && vy_tagof(b) == VY_INT) return vy_int(a.i >> b.i);
  return vy_rshift_slow(a, b);
}

static inline int vy_is(VyValue a, VyTag t) { return vy_tagof(a) == t; }

int     vy_in(VyValue needle, VyValue haystack);
VyValue vy_range(int64_t lo, int64_t hi, int64_t step);

static inline int vy_eq(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == tb) {
    switch (ta) {
      case VY_NIL:   return 1;
      case VY_BOOL:  return (a.b != 0) == (b.b != 0);
      case VY_INT:   return a.i == b.i;
      case VY_FLOAT: return a.f == b.f;
      default:       break;    /* string / list / map / func: slow */
    }
  } else if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT)) {
    return vy_tof(a) == vy_tof(b);
  }
  return vy_eq_slow(a, b);
}

static inline int vy_cmp(VyValue a, VyValue b) {
  VyTag ta = vy_tagof(a), tb = vy_tagof(b);
  if (ta == VY_INT && tb == VY_INT) return a.i < b.i ? -1 : (a.i > b.i);
  if ((ta == VY_INT || ta == VY_FLOAT) && (tb == VY_INT || tb == VY_FLOAT)) {
    double x = vy_tof(a), y = vy_tof(b);
    return x < y ? -1 : (x > y);
  }
  return vy_cmp_slow(a, b);
}

const char* vy_type_name(VyValue v);
const char* vy_tag_name(VyTag t);
VyValue vy_type_of(VyValue v);

/* ------------------------------------------------------------- console */

void     vy_print(VyStr* s);
void     vy_print_nl(VyStr* s);
void     vy_print_err(VyStr* s);
VyStr*   vy_read_line(void);      /* NULL at end of input               */
void     vy_flush(void);

/* print() / string rendering. A bare string prints unquoted; everything else
 * renders as JSON with nested strings quoted. */
VyStr*   vy_render(VyValue v);
VyStr*   vy_repr(VyValue v);      /* always valid JSON                  */

/* --------------------------------------------------------- environment */

VyStr*   vy_env_get(const char* name);      /* NULL when unset              */
VyValue  vy_env_get_val(const char* name);  /* nil when unset              */
void     vy_env_set(const char* name, VyStr* value);

/* -------------------------------------------------------------- args */

extern int    g_argc;
extern char** g_argv;
int       vy_argc(void);
const char* vy_argv(int i);
VyValue   vy_args(void);

/* ---------------------------------------------------------- json */

VyValue vy_json_parse(const char* text, size_t len);   /* nil on error     */
VyStr*  vy_json_stringify(VyValue v);                  /* caller frees     */
int     vy_json_valid(const char* text, size_t len);
const char* vy_json_error_slot(void);

/* ---------------------------------------------------------- http */

typedef struct VyHttpResponse VyHttpResponse;

struct VyHttpResponse {
  int64_t status;
  VyStr*  body;
  VyValue headers;      /* map of lowercase header -> string (array if dup) */
  VyStr*  error;        /* non-NULL when the request itself failed          */
  double  elapsed_ms;
};

/* method: one of "GET" "POST" "PUT" "PATCH" "DELETE" "HEAD"
 * url, body: NULL-terminated UTF-8 (may be NULL)
 * headers_json / params_json: JSON object strings, or NULL
 * returns NULL only on allocation failure; transport errors come back as
 * response.error so the language can `try/catch` them like any other value. */
VyHttpResponse* vy_http_request(const char* method, const char* url,
                                const char* body, const char* content_type,
                                const char* headers_json, const char* params_json,
                                double timeout_s);

const char* vy_http_libcurl_version(void);

/* --------------------------------------------------- user functions */

struct VyFunc;
typedef VyValue (*VyFnPtr)(struct VyFunc* fn, VyValue* args, int argc);
struct VyFunc {
  VyFnPtr   call;
  VyStr*    name;
  int       arity;
  int       variadic;
  void*     upvals;      /* opaque closure environment / upvalues */
};

/* ------------------------------------------------------- gc / memory */

/* A precise, non-moving mark-sweep collector over all three heap kinds
 * (string, list, map) -- see docs/memory-model.md. Roots are the registered
 * value slots (the backends' shadow stacks) plus whatever the backend passes
 * explicitly to vy_gc_collect_ex(). */
void  vy_gc_collect(void);
void  vy_gc_register_root(VyValue* slot);
void  vy_gc_unregister_root(VyValue* slot);
size_t vy_gc_roots_mark(void);
void   vy_gc_roots_restore(size_t mark);
typedef void (*VyGcScanner)(void);
void  vy_gc_set_scanner(VyGcScanner s);
void  vy_gc_mark_value(VyValue v);
void  vy_gc_collect_ex(VyValue* extra_roots, int n);
size_t vy_gc_live_bytes(void);
size_t vy_gc_heap_bytes(void);
size_t vy_heap_alloc_count(void);
void  vy_heap_reset_stats(void);
void vy_heap_note_realloc(size_t old_bytes, size_t new_bytes);
void vy_heap_track_str(VyStr* s, size_t len);
void vy_heap_track_list(VyList* l, size_t bytes);
void vy_heap_track_map(VyMap* m, size_t bytes);
uint32_t vy_str_hash_n(const char* p, size_t n);

/* ------------------------------------------------------------ panics */

void vy_error(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void vy_throw_value(VyValue v) __attribute__((noreturn));
void vy_throw_str(VyStr* s) __attribute__((noreturn));
void vy_type_error(const char* want, VyValue got) __attribute__((noreturn));
void vy_index_error(const char* what, int64_t idx, int64_t len) __attribute__((noreturn));
void vy_zero_error(void) __attribute__((noreturn));

/* -------------------------------------------------------- arena allocator */

/* Request-scoped region allocator.  All objects allocated from an arena are
 * freed in O(1) by vy_arena_reset() / vy_arena_free().  Arena strings are NOT
 * GC-tracked; they must not be stored in long-lived GC containers.          */
typedef struct VyArena VyArena;

VyArena* vy_arena_new(size_t block_size);   /* 0 -> 64 KB default          */
void*    vy_arena_alloc(VyArena* a, size_t n);
void*    vy_arena_calloc(VyArena* a, size_t n);
char*    vy_arena_strdup(VyArena* a, const char* s, size_t n);
VyStr*   vy_arena_str(VyArena* a, const char* p, size_t n);
void     vy_arena_reset(VyArena* a);   /* free all allocs, keep block       */
void     vy_arena_free(VyArena* a);    /* free everything incl. struct      */
size_t   vy_arena_used(VyArena* a);
size_t   vy_arena_peak(VyArena* a);

/* ------------------------------------------------- fast / SIMD JSON */

/* Drop-in replacement for vy_json_parse with optional arena for temporaries. */
VyValue vy_json_parse_fast(const char* text, size_t len, VyArena* arena);

/* Fast field extraction without building full AST. */
VyValue vy_json_extract_field(const char* json, size_t len, const char* key);

/* Typed field accessors -- avoid building intermediate VyValues. */
int     vy_json_get_str  (VyValue obj, const char* key, const char** out_data, size_t* out_len);
int     vy_json_get_int  (VyValue obj, const char* key, int64_t* out);
int     vy_json_get_float(VyValue obj, const char* key, double* out);
VyValue vy_json_get_array(VyValue obj, const char* key);

/* AI-specific chat/tool-call helpers. */
VyValue vy_json_chat_content(const char* body, size_t len);
int     vy_json_tool_call(const char* body, size_t len,
                          VyValue* out_name, VyValue* out_args);

/* --------------------------------------------------- HTTP connection pool */

typedef struct VyHttpPool VyHttpPool;

VyHttpPool*     vy_http_pool_new(int max_conns);   /* 0 -> 8 default        */
void            vy_http_pool_free(VyHttpPool* p);
VyHttpResponse* vy_http_pool_request(VyHttpPool* pool,
                                     const char* method, const char* url,
                                     const char* body, const char* content_type,
                                     const char* headers_json,
                                     const char* params_json,
                                     double timeout_s);
VyHttpPool*     vy_http_default_pool(void);   /* lazily-created global pool */
void            vy_http_pool_cleanup(void);   /* free global pool           */

#ifdef __cplusplus
}
#endif

#endif /* VYRT_H */
/* Ovyth runtime :: src/ovrt.c
 *
 * Heap + precise mark-sweep GC + panic/unwind.
 *
 * Design note (docs/memory-model.md): v0.1 uses one uniform strategy -- a
 * precise, non-moving mark-sweep collector over all three heap kinds. Every
 * object begins with a OvHeader; a OvStr lives inside its own allocation so
 * the header sits immediately before the string body. Correctness first; the
 * harness in benchmarks/ reports the cost so a later version can choose
 * between arenas, escape analysis and stack slots with data instead of taste.
 */
#include "ovrt.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* oom(size_t n) {
  fprintf(stderr, "ovyth: out of memory (%zu bytes)\n", n);
  exit(70);
  return NULL;
}

/* ---------------------------------------------------------------- lists */

OvList* ov_list_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  OvList* l = (OvList*)calloc(1, sizeof(OvList));
  if (!l) oom(sizeof(OvList));
  l->cap = cap;
  l->len = 0;
  l->items = (OvValue*)calloc(cap, sizeof(OvValue));
  if (!l->items) oom((size_t)cap * sizeof(OvValue));
  ov_heap_track_list(l, sizeof(OvList) + (size_t)cap * sizeof(OvValue));
  return l;
}

OvList* ov_list_new(void) { return ov_list_new_cap(4); }

int64_t ov_list_len(OvList* l) { return l ? (int64_t)l->len : 0; }

/* Items live in their own trailing allocation so growth is a plain realloc.
 * (A flexible array member would save one malloc but force a memmove on every
 * growth; benchmark numbers, not taste, should decide that later.) */
static void list_grow(OvList* l, uint32_t need) {
  if (need <= l->cap) return;
  /* Bracket the swap: see map_rehash. l->items is replaced before the new
   * array is fully in place, so a collection here would size the heap from a
   * list that is mid-move. */
  ov_gc_begin_mutation();
  size_t old_bytes = sizeof(OvList) + (size_t)l->cap * sizeof(OvValue);
  uint32_t cap = l->cap ? l->cap : 4;
  while (cap < need) cap *= 2;
  OvValue* items = (OvValue*)calloc(cap, sizeof(OvValue));
  if (!items) { ov_gc_end_mutation(); oom((size_t)cap * sizeof(OvValue)); }
  if (l->len) memcpy(items, l->items, (size_t)l->len * sizeof(OvValue));
  free(l->items);
  l->items = items;
  l->cap = cap;
  ov_heap_note_realloc(old_bytes, sizeof(OvList) + (size_t)cap * sizeof(OvValue));
  ov_gc_end_mutation();
}

void ov_list_push_slow(OvList* l, OvValue v) {
  if (!l) return;
  list_grow(l, l->len + 1);
  l->items[l->len++] = v;
}

OvValue ov_list_get_slow(OvList* l, int64_t i) {
  if (!l) return ov_nil();
  if (i < 0) i += l->len;  /* negative index counts from the end */
  if (i < 0 || i >= (int64_t)l->len) ov_index_error("list", i, (int64_t)l->len);
  return l->items[i];
}

void ov_list_set(OvList* l, int64_t i, OvValue v) {
  if (!l || i < 0 || i >= (int64_t)l->len)
    ov_index_error("list", i, l ? (int64_t)l->len : 0);
  l->items[i] = v;
}

OvValue ov_list_pop(OvList* l) {
  if (!l || l->len == 0) return ov_nil();
  return l->items[--l->len];
}

void ov_list_insert(OvList* l, int64_t i, OvValue v) {
  if (!l) return;
  if (i < 0) i += l->len;
  if (i < 0) i = 0;
  if (i > (int64_t)l->len) i = l->len;
  list_grow(l, l->len + 1);
  memmove(&l->items[i + 1], &l->items[i], (l->len - i) * sizeof(OvValue));
  l->items[i] = v;
  l->len++;
}

int ov_list_remove(OvList* l, int64_t i) {
  if (!l || i < 0 || i >= (int64_t)l->len) return 0;
  memmove(&l->items[i], &l->items[i + 1], (l->len - i - 1) * sizeof(OvValue));
  l->len--;
  return 1;
}

OvList* ov_list_copy(OvList* l) {
  OvList* c = ov_list_new_cap(l && l->len ? l->len : 4);
  if (l && l->len) {
    memcpy(c->items, l->items, (size_t)l->len * sizeof(OvValue));
    c->len = l->len;
  }
  return c;
}

void ov_list_reverse(OvList* l) {
  if (!l) return;
  for (uint32_t i = 0, j = l->len ? l->len - 1 : 0; i < j; i++, j--) {
    OvValue t = l->items[i];
    l->items[i] = l->items[j];
    l->items[j] = t;
  }
}

int64_t ov_list_index(OvList* l, OvValue v) {
  if (!l) return -1;
  for (uint32_t i = 0; i < l->len; i++)
    if (ov_eq(l->items[i], v)) return (int64_t)i;
  return -1;
}

int ov_list_contains(OvList* l, OvValue v) { return ov_list_index(l, v) >= 0; }

static int cmp_for_sort(const void* a, const void* b) {
  return ov_cmp(*(const OvValue*)a, *(const OvValue*)b);
}

void ov_list_sort(OvList* l) {
  if (l && l->len > 1) qsort(l->items, l->len, sizeof(OvValue), cmp_for_sort);
}

/* ---------------------------------------------------------- specialized arrays */

/* Int64Array */
OvInt64Array* ov_i64a_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  OvInt64Array* a = (OvInt64Array*)calloc(1, sizeof(OvInt64Array));
  if (!a) oom(sizeof(OvInt64Array));
  a->cap = cap;
  a->len = 0;
  a->data = (int64_t*)calloc(cap, sizeof(int64_t));
  if (!a->data) oom((size_t)cap * sizeof(int64_t));
  return a;
}

void ov_i64a_push_slow(OvInt64Array* a, int64_t v) {
  if (!a) return;
  uint32_t cap = a->cap ? a->cap : 4;
  while (cap < a->len + 1) cap *= 2;
  int64_t* items = (int64_t*)realloc(a->data, cap * sizeof(int64_t));
  if (!items) oom((size_t)cap * sizeof(int64_t));
  a->data = items;
  a->cap = cap;
  a->data[a->len++] = v;
}

int64_t ov_i64a_get_slow(OvInt64Array* a, int64_t i) {
  if (!a) return 0;
  if (i < 0) i += a->len;
  if (i < 0 || i >= (int64_t)a->len) return 0;
  return a->data[i];
}

void ov_i64a_free(OvInt64Array* a) {
  if (!a) return;
  free(a->data);
  free(a);
}

/* Float64Array */
OvFloat64Array* ov_f64a_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  OvFloat64Array* a = (OvFloat64Array*)calloc(1, sizeof(OvFloat64Array));
  if (!a) oom(sizeof(OvFloat64Array));
  a->cap = cap;
  a->len = 0;
  a->data = (double*)calloc(cap, sizeof(double));
  if (!a->data) oom((size_t)cap * sizeof(double));
  return a;
}

void ov_f64a_push_slow(OvFloat64Array* a, double v) {
  if (!a) return;
  uint32_t cap = a->cap ? a->cap : 4;
  while (cap < a->len + 1) cap *= 2;
  double* items = (double*)realloc(a->data, cap * sizeof(double));
  if (!items) oom((size_t)cap * sizeof(double));
  a->data = items;
  a->cap = cap;
  a->data[a->len++] = v;
}

double ov_f64a_get_slow(OvFloat64Array* a, int64_t i) {
  if (!a) return 0.0;
  if (i < 0) i += a->len;
  if (i < 0 || i >= (int64_t)a->len) return 0.0;
  return a->data[i];
}

void ov_f64a_free(OvFloat64Array* a) {
  if (!a) return;
  free(a->data);
  free(a);
}

/* StringArray */
OvStringArray* ov_stra_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  OvStringArray* a = (OvStringArray*)calloc(1, sizeof(OvStringArray));
  if (!a) oom(sizeof(OvStringArray));
  a->cap = cap;
  a->len = 0;
  a->data = (OvStr**)calloc(cap, sizeof(OvStr*));
  if (!a->data) oom((size_t)cap * sizeof(OvStr*));
  return a;
}

void ov_stra_push_slow(OvStringArray* a, OvStr* v) {
  if (!a) return;
  uint32_t cap = a->cap ? a->cap : 4;
  while (cap < a->len + 1) cap *= 2;
  OvStr** items = (OvStr**)realloc(a->data, cap * sizeof(OvStr*));
  if (!items) oom((size_t)cap * sizeof(OvStr*));
  a->data = items;
  a->cap = cap;
  a->data[a->len++] = v;
}

OvStr* ov_stra_get_slow(OvStringArray* a, int64_t i) {
  if (!a) return NULL;
  if (i < 0) i += a->len;
  if (i < 0 || i >= (int64_t)a->len) return NULL;
  return a->data[i];
}

void ov_stra_free(OvStringArray* a) {
  if (!a) return;
  free(a->data);
  free(a);
}

/* Specialized array value wrappers */
OvValue ov_i64a_val(OvInt64Array* a)   { OvValue v; v.tag = OV_TAG_OF(a, OV_I64A);  v.i64a = a; return v; }
OvValue ov_f64a_val(OvFloat64Array* a) { OvValue v; v.tag = OV_TAG_OF(a, OV_F64A);  v.f64a = a; return v; }
OvValue ov_stra_val(OvStringArray* a)  { OvValue v; v.tag = OV_TAG_OF(a, OV_STRA);  v.stra = a; return v; }

/* ----------------------------------------------------------------- maps */

/* An empty slot is an all-zero OvPair. Note this is NOT the same as "the key
 * is nil": a real entry whose *value* is nil has key.tag != 0, so it is
 * distinguishable. calloc provides the zero state for freshly allocated
 * tables and map_del restores it explicitly. */
static int slot_empty(const OvPair* p) {
  return p->key.tag == 0 && p->val.tag == 0 && p->key.i == 0 && p->val.i == 0;
}

static uint64_t hash_value(OvValue v) {
  uint64_t x;
  switch (ov_tagof(v)) {
    case OV_NIL:   return 0x9e3779b97f4a7c15ULL;
    case OV_BOOL:  return v.b ? 0xbf58476d1ce4e5b9ULL : 0x94d049bb133111ebULL;
    case OV_INT:
      x = (uint64_t)v.i;
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      return x ^ (x >> 33);
    case OV_FLOAT:
      if (v.f == (double)(int64_t)v.f) return hash_value(ov_int((int64_t)v.f));
      memcpy(&x, &v.f, 8);
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      return x ^ (x >> 33);
    case OV_STRING: return (uint64_t)v.str->hash * 0x9e3779b97f4a7c15ULL;
    case OV_I64A:   return (uint64_t)(uintptr_t)v.i64a * 0x9e3779b97f4a7c15ULL;
    case OV_F64A:   return (uint64_t)(uintptr_t)v.f64a * 0x9e3779b97f4a7c15ULL;
    case OV_STRA:   return (uint64_t)(uintptr_t)v.stra * 0x9e3779b97f4a7c15ULL;
    default:       return (uint64_t)(uintptr_t)OV_PTR_OF(v) >> 3;
  }
}

/* An entry with a nil key is how an index-map records "no value". Because a
 * real nil key would collide with it, a nil key is stored in slot 0 and
 * lookups probe it explicitly. Callers guarantee entries is non-empty. */
static int64_t map_find_slot(OvMap* m, OvValue key);

/* Returns a free slot for `key`, or NULL if the table is full (which cannot
 * happen below the 0.75 load factor, but is bounded anyway so a corrupted
 * table degrades into "entry dropped" rather than an infinite probe). */
static OvPair* map_insert_slot(OvMap* m, OvValue key) {
  if (ov_tagof(key) == OV_NIL) {
    if (slot_empty(&m->entries[0])) return &m->entries[0];
    return NULL;
  }
  uint32_t mask = m->cap - 1;
  uint32_t i = (uint32_t)(hash_value(key) & mask);
  for (uint32_t probe = 0; probe < m->cap; probe++) {
    if (slot_empty(&m->entries[i])) return &m->entries[i];
    i = (i + 1) & mask;
  }
  return NULL;
}

static int64_t map_find_slot(OvMap* m, OvValue key) {
  if (!m || !m->cap) return -1;
  if (ov_tagof(key) == OV_NIL) return slot_empty(&m->entries[0]) ? -1 : 0;
  uint32_t mask = m->cap - 1;
  uint32_t i = (uint32_t)(hash_value(key) & mask);
  for (uint32_t probe = 0; probe < m->cap; probe++) {
    OvPair* p = &m->entries[i];
    if (slot_empty(p)) return -1;
    if (ov_eq(p->key, key)) return (int64_t)i;
    i = (i + 1) & mask;
  }
  return -1;
}

static void map_rehash(OvMap* m, uint32_t new_cap) {
  /* The table is swapped and then repopulated; a collection in that window
   * would see m->len as the partially rebuilt count and size the heap from a
   * table that is not yet consistent. Bracket the whole operation. */
  ov_gc_begin_mutation();
  OvPair* old = m->entries;
  uint32_t old_cap = m->cap;
  size_t old_bytes = sizeof(OvMap) + (size_t)old_cap * sizeof(OvPair);
  m->entries = (OvPair*)calloc(new_cap, sizeof(OvPair));
  if (!m->entries) { ov_gc_end_mutation(); oom((size_t)new_cap * sizeof(OvPair)); }
  m->cap = new_cap;
  m->len = 0;
  for (uint32_t i = 0; i < old_cap; i++) {
    if (slot_empty(&old[i])) continue;
    OvPair* slot = map_insert_slot(m, old[i].key);
    if (slot) {
      *slot = old[i];
      m->len++;  /* the table is being repopulated, so recount as we go */
    }
  }
  free(old);
  ov_heap_note_realloc(old_bytes, sizeof(OvMap) + (size_t)new_cap * sizeof(OvPair));
  ov_gc_end_mutation();
}

OvMap* ov_map_new(void) {
  OvMap* m = (OvMap*)calloc(1, sizeof(OvMap));
  if (!m) oom(sizeof(OvMap));
  m->cap = 8;
  m->len = 0;
  m->entries = (OvPair*)calloc(m->cap, sizeof(OvPair));
  if (!m->entries) oom((size_t)m->cap * sizeof(OvPair));
  ov_heap_track_map(m, sizeof(OvMap) + (size_t)m->cap * sizeof(OvPair));
  return m;
}

OvValue ov_map_get(OvMap* m, OvValue key) {
  if (!m) return ov_nil();
  int64_t s = map_find_slot(m, key);
  return s < 0 ? ov_nil() : m->entries[s].val;
}

int ov_map_has(OvMap* m, OvValue key) {
  return m && map_find_slot(m, key) >= 0;
}

void ov_map_set(OvMap* m, OvValue key, OvValue v) {
  if (!m) return;
  int64_t s = map_find_slot(m, key);
  if (s >= 0) { m->entries[s].val = v; return; }
  /* Grow *before* the table fills. Inserting only when a free slot cannot be
   * found (i.e. at 100% load) means every table fills to load factor ~1.0, and
   * with a well-avalanched hash the trailing insertions degenerate: the probe
   * for the last few thousand entries walks four- and five-figure slot counts
   * each. benchmarks/cases/mapops.ov exposed it -- 100k ordered int keys cost
   * 176ms, of which almost all was the tail of each doubling. Growing at 0.7
   * keeps the expected probe count under ~2 and the fill linear. */
  if ((size_t)(m->len + 1) * 10 >= (size_t)m->cap * 7)
    map_rehash(m, m->cap * 2);
  /* Retry once: a rehash repacks every entry, so the free slot for this key
   * may only have appeared after it (see the note that used to live here). */
  for (int attempt = 0; attempt < 2; attempt++) {
    OvPair* slot = map_insert_slot(m, key);
    if (slot) {
      slot->key = key;
      slot->val = v;
      m->len++;
      return;
    }
    map_rehash(m, m->cap * 2);
  }
}

int ov_map_del(OvMap* m, OvValue key) {
  if (!m) return 0;
  int64_t s = map_find_slot(m, key);
  if (s < 0) return 0;
  if (ov_tagof(key) == OV_NIL) {
    m->entries[0].key.tag = 0; m->entries[0].key.i = 0;
    m->entries[0].val.tag = 0; m->entries[0].val.i = 0;
    m->len--;
    return 1;
  }
  m->entries[s].key.tag = 0; m->entries[s].key.i = 0;
  m->entries[s].val.tag = 0; m->entries[s].val.i = 0;
  m->len--;
  /* backward-shift deletion keeps the probe chains intact */
  uint32_t mask = m->cap - 1;
  uint32_t i = (uint32_t)((s + 1) & mask);
  while (!slot_empty(&m->entries[i])) {
    OvPair moved = m->entries[i];
    m->entries[i].key.tag = 0; m->entries[i].key.i = 0;
    m->entries[i].val.tag = 0; m->entries[i].val.i = 0;
    uint32_t home = (uint32_t)(hash_value(moved.key) & mask);
    uint32_t k = (home + 1) & mask;
    while (k != i) {
      if (slot_empty(&m->entries[k])) break;
      k = (k + 1) & mask;
    }
    m->entries[home] = moved;
    i = (i + 1) & mask;
  }
  return 1;
}

int64_t ov_map_len(OvMap* m) { return m ? (int64_t)m->len : 0; }

void ov_map_clear(OvMap* m) {
  if (!m) return;
  memset(m->entries, 0, (size_t)m->cap * sizeof(OvPair));
  m->len = 0;
}

OvList* ov_map_pairs(OvMap* m) {
  ov_gc_begin_mutation();
  OvList* out = ov_list_new_cap(m && m->len ? m->len * 2 : 4);
  if (!m) { ov_gc_end_mutation(); return out; }
  for (uint32_t i = 0; i < m->cap; i++) {
    if (slot_empty(&m->entries[i])) continue;
    ov_list_push(out, m->entries[i].key);
    ov_list_push(out, m->entries[i].val);
  }
  ov_gc_end_mutation();
  return out;
}

OvValue ov_map_keys(OvMap* m) {
  ov_gc_begin_mutation();
  OvList* out = ov_list_new_cap(m && m->len ? m->len : 4);
  if (!m) { ov_gc_end_mutation(); return ov_list(out); }
  for (uint32_t i = 0; i < m->cap; i++)
    if (!slot_empty(&m->entries[i])) ov_list_push(out, m->entries[i].key);
  ov_gc_end_mutation();
  return ov_list(out);
}

OvValue ov_map_values(OvMap* m) {
  ov_gc_begin_mutation();
  OvList* out = ov_list_new_cap(m && m->len ? m->len : 4);
  if (!m) { ov_gc_end_mutation(); return ov_list(out); }
  for (uint32_t i = 0; i < m->cap; i++)
    if (!slot_empty(&m->entries[i])) ov_list_push(out, m->entries[i].val);
  ov_gc_end_mutation();
  return ov_list(out);
}


/* --------------------------------------------------------- panics */

OvValue ov_caught;

static jmp_buf g_jmp;
static int     g_jmp_active = 0;
static int     g_exit_code = 0;

jmp_buf* ov_try_push(void)    { g_jmp_active = 1; return &g_jmp; }
void     ov_try_pop(void)     { g_jmp_active = 0; }
int      ov_try_active(void)  { return g_jmp_active; }
int      ov_exit_code(void)   { return g_exit_code; }

static void raise(OvValue v) {
  ov_caught = v;
  if (g_jmp_active) {
    g_jmp_active = 0;
    longjmp(g_jmp, 1);
  }
  OvStr* s = ov_json_stringify(v);
  fprintf(stderr, "\novyth: uncaught error: %s\n", ov_str_data(s));
  ov_runtime_shutdown();
  exit(70);
}

void ov_throw_value(OvValue v) { raise(v); }
void ov_throw_str(OvStr* s)    { raise(ov_str(s)); }

void ov_request_exit(int code) {
  g_exit_code = code;
  raise(ov_nil());
}

void ov_error(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  raise(ov_str_val(buf));
}

void ov_type_error(const char* want, OvValue got) {
  ov_error("expected %s but got %s", want, ov_type_name(got));
}

void ov_index_error(const char* what, int64_t idx, int64_t len) {
  ov_error("%s index %lld out of range (length %lld)", what, (long long)idx,
           (long long)len);
}

void ov_zero_error(void) { ov_error("division by zero"); }
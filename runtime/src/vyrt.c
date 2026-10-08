/* Vayu runtime :: src/vyrt.c
 *
 * Heap + precise mark-sweep GC + panic/unwind.
 *
 * Design note (docs/memory-model.md): v0.1 uses one uniform strategy -- a
 * precise, non-moving mark-sweep collector over all three heap kinds. Every
 * object begins with a VyHeader; a VyStr lives inside its own allocation so
 * the header sits immediately before the string body. Correctness first; the
 * harness in benchmarks/ reports the cost so a later version can choose
 * between arenas, escape analysis and stack slots with data instead of taste.
 */
#include "vyrt.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* oom(size_t n) {
  fprintf(stderr, "vayu: out of memory (%zu bytes)\n", n);
  exit(70);
  return NULL;
}

/* ---------------------------------------------------------------- lists */

VyList* vy_list_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  VyList* l = (VyList*)calloc(1, sizeof(VyList));
  if (!l) oom(sizeof(VyList));
  l->cap = cap;
  l->len = 0;
  l->items = (VyValue*)calloc(cap, sizeof(VyValue));
  if (!l->items) oom((size_t)cap * sizeof(VyValue));
  vy_heap_track_list(l, sizeof(VyList) + (size_t)cap * sizeof(VyValue));
  return l;
}

VyList* vy_list_new(void) { return vy_list_new_cap(4); }

int64_t vy_list_len(VyList* l) { return l ? (int64_t)l->len : 0; }

/* Items live in their own trailing allocation so growth is a plain realloc.
 * (A flexible array member would save one malloc but force a memmove on every
 * growth; benchmark numbers, not taste, should decide that later.) */
static void list_grow(VyList* l, uint32_t need) {
  if (need <= l->cap) return;
  /* Bracket the swap: see map_rehash. l->items is replaced before the new
   * array is fully in place, so a collection here would size the heap from a
   * list that is mid-move. */
  vy_gc_begin_mutation();
  size_t old_bytes = sizeof(VyList) + (size_t)l->cap * sizeof(VyValue);
  uint32_t cap = l->cap ? l->cap : 4;
  while (cap < need) cap *= 2;
  VyValue* items = (VyValue*)calloc(cap, sizeof(VyValue));
  if (!items) { vy_gc_end_mutation(); oom((size_t)cap * sizeof(VyValue)); }
  if (l->len) memcpy(items, l->items, (size_t)l->len * sizeof(VyValue));
  free(l->items);
  l->items = items;
  l->cap = cap;
  vy_heap_note_realloc(old_bytes, sizeof(VyList) + (size_t)cap * sizeof(VyValue));
  vy_gc_end_mutation();
}

void vy_list_push_slow(VyList* l, VyValue v) {
  if (!l) return;
  list_grow(l, l->len + 1);
  l->items[l->len++] = v;
}

VyValue vy_list_get_slow(VyList* l, int64_t i) {
  if (!l) return vy_nil();
  if (i < 0) i += l->len;  /* negative index counts from the end */
  if (i < 0 || i >= (int64_t)l->len) vy_index_error("list", i, (int64_t)l->len);
  return l->items[i];
}

void vy_list_set(VyList* l, int64_t i, VyValue v) {
  if (!l || i < 0 || i >= (int64_t)l->len)
    vy_index_error("list", i, l ? (int64_t)l->len : 0);
  l->items[i] = v;
}

VyValue vy_list_pop(VyList* l) {
  if (!l || l->len == 0) return vy_nil();
  return l->items[--l->len];
}

void vy_list_insert(VyList* l, int64_t i, VyValue v) {
  if (!l) return;
  if (i < 0) i += l->len;
  if (i < 0) i = 0;
  if (i > (int64_t)l->len) i = l->len;
  list_grow(l, l->len + 1);
  memmove(&l->items[i + 1], &l->items[i], (l->len - i) * sizeof(VyValue));
  l->items[i] = v;
  l->len++;
}

int vy_list_remove(VyList* l, int64_t i) {
  if (!l || i < 0 || i >= (int64_t)l->len) return 0;
  memmove(&l->items[i], &l->items[i + 1], (l->len - i - 1) * sizeof(VyValue));
  l->len--;
  return 1;
}

VyList* vy_list_copy(VyList* l) {
  VyList* c = vy_list_new_cap(l && l->len ? l->len : 4);
  if (l && l->len) {
    memcpy(c->items, l->items, (size_t)l->len * sizeof(VyValue));
    c->len = l->len;
  }
  return c;
}

void vy_list_reverse(VyList* l) {
  if (!l) return;
  for (uint32_t i = 0, j = l->len ? l->len - 1 : 0; i < j; i++, j--) {
    VyValue t = l->items[i];
    l->items[i] = l->items[j];
    l->items[j] = t;
  }
}

int64_t vy_list_index(VyList* l, VyValue v) {
  if (!l) return -1;
  for (uint32_t i = 0; i < l->len; i++)
    if (vy_eq(l->items[i], v)) return (int64_t)i;
  return -1;
}

int vy_list_contains(VyList* l, VyValue v) { return vy_list_index(l, v) >= 0; }

static int cmp_for_sort(const void* a, const void* b) {
  return vy_cmp(*(const VyValue*)a, *(const VyValue*)b);
}

void vy_list_sort(VyList* l) {
  if (l && l->len > 1) qsort(l->items, l->len, sizeof(VyValue), cmp_for_sort);
}

/* ---------------------------------------------------------- specialized arrays */

/* Int64Array */
VyInt64Array* vy_i64a_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  VyInt64Array* a = (VyInt64Array*)calloc(1, sizeof(VyInt64Array));
  if (!a) oom(sizeof(VyInt64Array));
  a->cap = cap;
  a->len = 0;
  a->data = (int64_t*)calloc(cap, sizeof(int64_t));
  if (!a->data) oom((size_t)cap * sizeof(int64_t));
  return a;
}

void vy_i64a_push_slow(VyInt64Array* a, int64_t v) {
  if (!a) return;
  uint32_t cap = a->cap ? a->cap : 4;
  while (cap < a->len + 1) cap *= 2;
  int64_t* items = (int64_t*)realloc(a->data, cap * sizeof(int64_t));
  if (!items) oom((size_t)cap * sizeof(int64_t));
  a->data = items;
  a->cap = cap;
  a->data[a->len++] = v;
}

int64_t vy_i64a_get_slow(VyInt64Array* a, int64_t i) {
  if (!a) return 0;
  if (i < 0) i += a->len;
  if (i < 0 || i >= (int64_t)a->len) return 0;
  return a->data[i];
}

void vy_i64a_free(VyInt64Array* a) {
  if (!a) return;
  free(a->data);
  free(a);
}

/* Float64Array */
VyFloat64Array* vy_f64a_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  VyFloat64Array* a = (VyFloat64Array*)calloc(1, sizeof(VyFloat64Array));
  if (!a) oom(sizeof(VyFloat64Array));
  a->cap = cap;
  a->len = 0;
  a->data = (double*)calloc(cap, sizeof(double));
  if (!a->data) oom((size_t)cap * sizeof(double));
  return a;
}

void vy_f64a_push_slow(VyFloat64Array* a, double v) {
  if (!a) return;
  uint32_t cap = a->cap ? a->cap : 4;
  while (cap < a->len + 1) cap *= 2;
  double* items = (double*)realloc(a->data, cap * sizeof(double));
  if (!items) oom((size_t)cap * sizeof(double));
  a->data = items;
  a->cap = cap;
  a->data[a->len++] = v;
}

double vy_f64a_get_slow(VyFloat64Array* a, int64_t i) {
  if (!a) return 0.0;
  if (i < 0) i += a->len;
  if (i < 0 || i >= (int64_t)a->len) return 0.0;
  return a->data[i];
}

void vy_f64a_free(VyFloat64Array* a) {
  if (!a) return;
  free(a->data);
  free(a);
}

/* StringArray */
VyStringArray* vy_stra_new_cap(uint32_t cap) {
  if (cap < 4) cap = 4;
  VyStringArray* a = (VyStringArray*)calloc(1, sizeof(VyStringArray));
  if (!a) oom(sizeof(VyStringArray));
  a->cap = cap;
  a->len = 0;
  a->data = (VyStr**)calloc(cap, sizeof(VyStr*));
  if (!a->data) oom((size_t)cap * sizeof(VyStr*));
  return a;
}

void vy_stra_push_slow(VyStringArray* a, VyStr* v) {
  if (!a) return;
  uint32_t cap = a->cap ? a->cap : 4;
  while (cap < a->len + 1) cap *= 2;
  VyStr** items = (VyStr**)realloc(a->data, cap * sizeof(VyStr*));
  if (!items) oom((size_t)cap * sizeof(VyStr*));
  a->data = items;
  a->cap = cap;
  a->data[a->len++] = v;
}

VyStr* vy_stra_get_slow(VyStringArray* a, int64_t i) {
  if (!a) return NULL;
  if (i < 0) i += a->len;
  if (i < 0 || i >= (int64_t)a->len) return NULL;
  return a->data[i];
}

void vy_stra_free(VyStringArray* a) {
  if (!a) return;
  free(a->data);
  free(a);
}

/* Specialized array value wrappers */
VyValue vy_i64a_val(VyInt64Array* a)   { VyValue v; v.tag = VY_TAG_OF(a, VY_I64A);  v.i64a = a; return v; }
VyValue vy_f64a_val(VyFloat64Array* a) { VyValue v; v.tag = VY_TAG_OF(a, VY_F64A);  v.f64a = a; return v; }
VyValue vy_stra_val(VyStringArray* a)  { VyValue v; v.tag = VY_TAG_OF(a, VY_STRA);  v.stra = a; return v; }

/* ----------------------------------------------------------------- maps */

/* An empty slot is an all-zero VyPair. Note this is NOT the same as "the key
 * is nil": a real entry whose *value* is nil has key.tag != 0, so it is
 * distinguishable. calloc provides the zero state for freshly allocated
 * tables and map_del restores it explicitly. */
static int slot_empty(const VyPair* p) {
  return p->key.tag == 0 && p->val.tag == 0 && p->key.i == 0 && p->val.i == 0;
}

static uint64_t hash_value(VyValue v) {
  uint64_t x;
  switch (vy_tagof(v)) {
    case VY_NIL:   return 0x9e3779b97f4a7c15ULL;
    case VY_BOOL:  return v.b ? 0xbf58476d1ce4e5b9ULL : 0x94d049bb133111ebULL;
    case VY_INT:
      x = (uint64_t)v.i;
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      return x ^ (x >> 33);
    case VY_FLOAT:
      if (v.f == (double)(int64_t)v.f) return hash_value(vy_int((int64_t)v.f));
      memcpy(&x, &v.f, 8);
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      return x ^ (x >> 33);
    case VY_STRING: return (uint64_t)v.str->hash * 0x9e3779b97f4a7c15ULL;
    case VY_I64A:   return (uint64_t)(uintptr_t)v.i64a * 0x9e3779b97f4a7c15ULL;
    case VY_F64A:   return (uint64_t)(uintptr_t)v.f64a * 0x9e3779b97f4a7c15ULL;
    case VY_STRA:   return (uint64_t)(uintptr_t)v.stra * 0x9e3779b97f4a7c15ULL;
    default:       return (uint64_t)(uintptr_t)VY_PTR_OF(v) >> 3;
  }
}

/* An entry with a nil key is how an index-map records "no value". Because a
 * real nil key would collide with it, a nil key is stored in slot 0 and
 * lookups probe it explicitly. Callers guarantee entries is non-empty. */
static int64_t map_find_slot(VyMap* m, VyValue key);

/* Returns a free slot for `key`, or NULL if the table is full (which cannot
 * happen below the 0.75 load factor, but is bounded anyway so a corrupted
 * table degrades into "entry dropped" rather than an infinite probe). */
static VyPair* map_insert_slot(VyMap* m, VyValue key) {
  if (vy_tagof(key) == VY_NIL) {
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

static int64_t map_find_slot(VyMap* m, VyValue key) {
  if (!m || !m->cap) return -1;
  if (vy_tagof(key) == VY_NIL) return slot_empty(&m->entries[0]) ? -1 : 0;
  uint32_t mask = m->cap - 1;
  uint32_t i = (uint32_t)(hash_value(key) & mask);
  for (uint32_t probe = 0; probe < m->cap; probe++) {
    VyPair* p = &m->entries[i];
    if (slot_empty(p)) return -1;
    if (vy_eq(p->key, key)) return (int64_t)i;
    i = (i + 1) & mask;
  }
  return -1;
}

static void map_rehash(VyMap* m, uint32_t new_cap) {
  /* The table is swapped and then repopulated; a collection in that window
   * would see m->len as the partially rebuilt count and size the heap from a
   * table that is not yet consistent. Bracket the whole operation. */
  vy_gc_begin_mutation();
  VyPair* old = m->entries;
  uint32_t old_cap = m->cap;
  size_t old_bytes = sizeof(VyMap) + (size_t)old_cap * sizeof(VyPair);
  m->entries = (VyPair*)calloc(new_cap, sizeof(VyPair));
  if (!m->entries) { vy_gc_end_mutation(); oom((size_t)new_cap * sizeof(VyPair)); }
  m->cap = new_cap;
  m->len = 0;
  for (uint32_t i = 0; i < old_cap; i++) {
    if (slot_empty(&old[i])) continue;
    VyPair* slot = map_insert_slot(m, old[i].key);
    if (slot) {
      *slot = old[i];
      m->len++;  /* the table is being repopulated, so recount as we go */
    }
  }
  free(old);
  vy_heap_note_realloc(old_bytes, sizeof(VyMap) + (size_t)new_cap * sizeof(VyPair));
  vy_gc_end_mutation();
}

VyMap* vy_map_new(void) {
  VyMap* m = (VyMap*)calloc(1, sizeof(VyMap));
  if (!m) oom(sizeof(VyMap));
  m->cap = 8;
  m->len = 0;
  m->entries = (VyPair*)calloc(m->cap, sizeof(VyPair));
  if (!m->entries) oom((size_t)m->cap * sizeof(VyPair));
  vy_heap_track_map(m, sizeof(VyMap) + (size_t)m->cap * sizeof(VyPair));
  return m;
}

VyValue vy_map_get(VyMap* m, VyValue key) {
  if (!m) return vy_nil();
  int64_t s = map_find_slot(m, key);
  return s < 0 ? vy_nil() : m->entries[s].val;
}

int vy_map_has(VyMap* m, VyValue key) {
  return m && map_find_slot(m, key) >= 0;
}

void vy_map_set(VyMap* m, VyValue key, VyValue v) {
  if (!m) return;
  int64_t s = map_find_slot(m, key);
  if (s >= 0) { m->entries[s].val = v; return; }
  /* Grow *before* the table fills. Inserting only when a free slot cannot be
   * found (i.e. at 100% load) means every table fills to load factor ~1.0, and
   * with a well-avalanched hash the trailing insertions degenerate: the probe
   * for the last few thousand entries walks four- and five-figure slot counts
   * each. benchmarks/cases/mapops.vy exposed it -- 100k ordered int keys cost
   * 176ms, of which almost all was the tail of each doubling. Growing at 0.7
   * keeps the expected probe count under ~2 and the fill linear. */
  if ((size_t)(m->len + 1) * 10 >= (size_t)m->cap * 7)
    map_rehash(m, m->cap * 2);
  /* Retry once: a rehash repacks every entry, so the free slot for this key
   * may only have appeared after it (see the note that used to live here). */
  for (int attempt = 0; attempt < 2; attempt++) {
    VyPair* slot = map_insert_slot(m, key);
    if (slot) {
      slot->key = key;
      slot->val = v;
      m->len++;
      return;
    }
    map_rehash(m, m->cap * 2);
  }
}

int vy_map_del(VyMap* m, VyValue key) {
  if (!m) return 0;
  int64_t s = map_find_slot(m, key);
  if (s < 0) return 0;
  if (vy_tagof(key) == VY_NIL) {
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
    VyPair moved = m->entries[i];
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

int64_t vy_map_len(VyMap* m) { return m ? (int64_t)m->len : 0; }

void vy_map_clear(VyMap* m) {
  if (!m) return;
  memset(m->entries, 0, (size_t)m->cap * sizeof(VyPair));
  m->len = 0;
}

VyList* vy_map_pairs(VyMap* m) {
  vy_gc_begin_mutation();
  VyList* out = vy_list_new_cap(m && m->len ? m->len * 2 : 4);
  if (!m) { vy_gc_end_mutation(); return out; }
  for (uint32_t i = 0; i < m->cap; i++) {
    if (slot_empty(&m->entries[i])) continue;
    vy_list_push(out, m->entries[i].key);
    vy_list_push(out, m->entries[i].val);
  }
  vy_gc_end_mutation();
  return out;
}

VyValue vy_map_keys(VyMap* m) {
  vy_gc_begin_mutation();
  VyList* out = vy_list_new_cap(m && m->len ? m->len : 4);
  if (!m) { vy_gc_end_mutation(); return vy_list(out); }
  for (uint32_t i = 0; i < m->cap; i++)
    if (!slot_empty(&m->entries[i])) vy_list_push(out, m->entries[i].key);
  vy_gc_end_mutation();
  return vy_list(out);
}

VyValue vy_map_values(VyMap* m) {
  vy_gc_begin_mutation();
  VyList* out = vy_list_new_cap(m && m->len ? m->len : 4);
  if (!m) { vy_gc_end_mutation(); return vy_list(out); }
  for (uint32_t i = 0; i < m->cap; i++)
    if (!slot_empty(&m->entries[i])) vy_list_push(out, m->entries[i].val);
  vy_gc_end_mutation();
  return vy_list(out);
}


/* --------------------------------------------------------- panics */

VyValue vy_caught;

static jmp_buf g_jmp;
static int     g_jmp_active = 0;
static int     g_exit_code = 0;

jmp_buf* vy_try_push(void)    { g_jmp_active = 1; return &g_jmp; }
void     vy_try_pop(void)     { g_jmp_active = 0; }
int      vy_try_active(void)  { return g_jmp_active; }
int      vy_exit_code(void)   { return g_exit_code; }

static void raise(VyValue v) {
  vy_caught = v;
  if (g_jmp_active) {
    g_jmp_active = 0;
    longjmp(g_jmp, 1);
  }
  VyStr* s = vy_json_stringify(v);
  fprintf(stderr, "\nvayu: uncaught error: %s\n", vy_str_data(s));
  vy_runtime_shutdown();
  exit(70);
}

void vy_throw_value(VyValue v) { raise(v); }
void vy_throw_str(VyStr* s)    { raise(vy_str(s)); }

void vy_request_exit(int code) {
  g_exit_code = code;
  raise(vy_nil());
}

void vy_error(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  raise(vy_str_val(buf));
}

void vy_type_error(const char* want, VyValue got) {
  vy_error("expected %s but got %s", want, vy_type_name(got));
}

void vy_index_error(const char* what, int64_t idx, int64_t len) {
  vy_error("%s index %lld out of range (length %lld)", what, (long long)idx,
           (long long)len);
}

void vy_zero_error(void) { vy_error("division by zero"); }
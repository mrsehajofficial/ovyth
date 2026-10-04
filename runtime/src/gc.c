/* Vayu runtime :: src/gc.c
 *
 * Precise, non-moving, mark-sweep collector.
 *
 * Roots
 *   - slots registered with vy_gc_register_root() (backend shadow stacks)
 *   - extra slots passed to vy_gc_collect_ex() (used when a backend collects
 *     while a partial result is only reachable from C locals)
 *
 * The mark phase is iterative: a Vayu program can build a list nested as
 * deeply as it likes, and a recursive collector would blow the C stack.
 */
#include "vyrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { H_STRING = 0, H_LIST = 1, H_MAP = 2 };

typedef struct {
  VyHeader* heap;
  size_t    heap_bytes;
  size_t    live_bytes;
  size_t    alloc_count;
  double    next_gc;
  int       disabled;

  VyValue** roots;
  size_t    roots_len, roots_cap;

  VyValue*  mark;
  size_t    mark_len, mark_cap;
} GC;

static GC g;

/* the object currently being constructed, so realloc growth can extend the
 * tracked total without a collection running under a half-updated object */
static __thread int    t_in_gc = 0;

size_t vy_gc_heap_bytes(void)    { return g.heap_bytes; }
size_t vy_gc_live_bytes(void)    { return g.live_bytes; }
size_t vy_heap_alloc_count(void) { return g.alloc_count; }

void vy_heap_reset_stats(void) {
  g.alloc_count = 0;
  g.heap_bytes = 0;
  g.live_bytes = 0;
  g.next_gc = 8 * 1024 * 1024;
}

void vy_gc_disable(int on) { g.disabled = on; }
int  vy_gc_enabled(void)   { return !g.disabled; }

void vy_gc_register_root(VyValue* slot) {
  if (g.roots_len == g.roots_cap) {
    g.roots_cap = g.roots_cap ? g.roots_cap * 2 : 1024;
    g.roots = (VyValue**)realloc(g.roots, g.roots_cap * sizeof(VyValue*));
  }
  g.roots[g.roots_len++] = slot;
}

void vy_gc_unregister_root(VyValue* slot) {
  for (size_t i = g.roots_len; i-- > 0;) {
    if (g.roots[i] == slot) {
      g.roots[i] = g.roots[--g.roots_len];
      return;
    }
  }
}

/* Collect only when it is safe to do so.
 *
 * The runtime mutates containers in place: map_rehash swaps `entries` and then
 * repopulates the new table, list growth reallocs the item array. A collection
 * running in that window would size the heap from a half-rebuilt table and, in
 * the worst case, free a container the caller is still holding. So a
 * container marks itself "mutating" for the duration of such an operation and
 * the threshold check simply skips; the next allocation after the operation
 * returns picks the collection up. */
static __thread int t_mutating = 0;

void vy_gc_begin_mutation(void) { t_mutating++; }
void vy_gc_end_mutation(void)   { if (t_mutating) t_mutating--; }

static void maybe_collect(void) {
  if (g.disabled || t_in_gc || t_mutating) return;
  if (g.heap_bytes >= g.next_gc) vy_gc_collect();
}

/* Allocate-black: the object being constructed is marked *before* any
 * collection can run, because a collector triggered by this very allocation
 * would otherwise find the half-built object unreachable and free it out from
 * under the constructor. sweep() clears the mark on survivors, so the bit is
 * reusable. */
static void track(VyHeader* h, size_t bytes) {
  h->marked = 1;
  h->next = g.heap;
  g.heap = h;
  g.heap_bytes += bytes;
  g.live_bytes += bytes;
  g.alloc_count++;
  maybe_collect();
}

/* A container that reallocs its backing array updates the tracked size of an
 * object that is *already* reachable.
 *
 * `old_bytes` must be the size this object was last accounted at -- NOT some
 * shared "last allocation" slot. A single shared slot silently corrupts the
 * heap total when a string allocation happens between two container updates:
 * the delta is computed against the wrong baseline, inflating heap_bytes and
 * live_bytes without any memory having been allocated. That drives the heap
 * over the collection threshold and the process thrashes.
 *
 * This also must not trigger a collection: the container is mid-update and the
 * collector would size the heap from a table that is not yet consistent.
 * The threshold is only recorded here; the next real allocation picks it up. */
void vy_heap_note_realloc(size_t old_bytes, size_t new_bytes) {
  size_t delta = new_bytes - old_bytes;
  g.heap_bytes += delta;
  g.live_bytes += delta;
  /* Deliberately no collect() trigger here -- see note above. */
}

/* The header sits immediately *before* the VyStr inside one allocation:
 *
 *     +----------------+------------------+-----------+---------+
 *     | VyHeader (24B) | VyStr (len, hash) | bytes ... |  NUL    |
 *     +----------------+------------------+-----------+---------+
 *                       ^ s
 *
 * sizeof(VyStr) is len-independent (flexible tail), so the real block is
 * sizeof(VyHeader) + sizeof(VyStr) + len. Both the accounting here and the
 * allocation in string.c must use that same expression -- an off-by-header
 * size here makes free() abort.
 */
void vy_heap_track_str(VyStr* s, size_t len) {
  VyHeader* h = &((VyHeader*)s)[-1];
  h->kind = H_STRING;
  h->len = (uint32_t)len;
  track(h, sizeof(VyHeader) + sizeof(VyStr) + len);
}

void vy_heap_track_list(VyList* l, size_t bytes) {
  l->hdr.kind = H_LIST;
  track(&l->hdr, bytes);
}

void vy_heap_track_map(VyMap* m, size_t bytes) {
  m->hdr.kind = H_MAP;
  track(&m->hdr, bytes);
}

static VyHeader* header_of(VyValue v) {
  switch (vy_tagof(v)) {
    case VY_STRING: return &((VyHeader*)v.str)[-1];
    case VY_LIST:   return &v.list->hdr;
    case VY_MAP:    return &v.map->hdr;
    default:        return NULL;
  }
}

static void mark_push(VyValue v) {
  if (g.mark_len == g.mark_cap) {
    g.mark_cap = g.mark_cap ? g.mark_cap * 2 : 1024;
    g.mark = (VyValue*)realloc(g.mark, g.mark_cap * sizeof(VyValue));
  }
  g.mark[g.mark_len++] = v;
}

/* The accounted size of a heap object, derived from its header exactly as
 * sweep() derives it. Marking must use the same arithmetic as sweeping, or the
 * live total and the freed total drift apart. */
static size_t object_bytes(VyHeader* h) {
  switch (h->kind) {
    case H_STRING: return sizeof(VyHeader) + sizeof(VyStr) + h->len;
    case H_LIST:   return sizeof(VyList) + (size_t)((VyList*)h)->cap * sizeof(VyValue);
    case H_MAP:    return sizeof(VyMap) + (size_t)((VyMap*)h)->cap * sizeof(VyPair);
    default:       return 0;
  }
}

static void mark_value(VyValue v) {
  VyHeader* h = header_of(v);
  if (!h || h->marked) return;
  h->marked = 1;
  /* Accumulate here, not in mark_loop: this is the only place that sees each
   * survivor exactly once (the marked bit makes it idempotent). Without it
   * live_bytes is reset to 0 and never rebuilt, so next_gc collapses to the
   * 8MB floor and the collector then runs on *every* allocation. */
  g.live_bytes += object_bytes(h);
  mark_push(v);
}

static void mark_loop(void) {
  while (g.mark_len) {
    VyValue v = g.mark[--g.mark_len];
    if (vy_tagof(v) == VY_LIST) {
      VyList* l = v.list;
      for (uint32_t i = 0; i < l->len; i++) {
        /* Nil/bool/int/float carry no heap object (the tag enum orders the
         * three container kinds at VY_STRING and above), so the tag test
         * replaces a call that would have returned immediately. Marking a
         * 100k-entry map of ints used to cost 200k calls per collection; now
         * it costs 200k tag compares and no calls at all. */
        if ((uint32_t)vy_tagof(l->items[i]) >= (uint32_t)VY_STRING)
          mark_value(l->items[i]);
      }
    } else if (vy_tagof(v) == VY_MAP) {
      VyMap* m = v.map;
      for (uint32_t i = 0; i < m->cap; i++) {
        /* empty slots are the all-zero pair; skip them cheaply. A nil key
         * stored in slot 0 is a real entry and is traced like any other. */
        if (m->entries[i].key.tag == 0 && m->entries[i].val.tag == 0 &&
            m->entries[i].key.i == 0 && m->entries[i].val.i == 0)
          continue;
        if ((uint32_t)vy_tagof(m->entries[i].key) >= (uint32_t)VY_STRING)
          mark_value(m->entries[i].key);
        if ((uint32_t)vy_tagof(m->entries[i].val) >= (uint32_t)VY_STRING)
          mark_value(m->entries[i].val);
      }
    }
  }
}

static void sweep(void) {
  VyHeader** link = &g.heap;
  while (*link) {
    VyHeader* h = *link;
    if (h->marked) {
      h->marked = 0;
      link = &h->next;
      continue;
    }
    /* Immortal string literals (vy_str_lit): their only root is a C static
     * in the generated code, which this collector cannot see. Keep them on
     * the heap list instead of freeing. */
    if (h->kind == H_STRING && (h->pad & VY_HDR_PIN)) {
      link = &h->next;
      continue;
    }
    *link = h->next;
    size_t bytes = 0;
    switch (h->kind) {
      case H_STRING: {
        bytes = sizeof(VyHeader) + sizeof(VyStr) + h->len;
        free(h);
        break;
      }
      case H_LIST: {
        VyList* l = (VyList*)h;
        bytes = sizeof(VyList) + (size_t)l->cap * sizeof(VyValue);
        free(l->items);
        free(l);
        break;
      }
      case H_MAP: {
        VyMap* m = (VyMap*)h;
        bytes = sizeof(VyMap) + (size_t)m->cap * sizeof(VyPair);
        free(m->entries);
        free(m);
        break;
      }
      default:
        fprintf(stderr, "[gc] unknown kind %u at %p\n", h->kind, (void*)h);
        break;
    }
    g.heap_bytes -= bytes;
    /* live_bytes is NOT adjusted here. vy_gc_collect_ex zeroes it and then
     * rebuilds it from the marked survivors (mark_value -> object_bytes), so
     * the objects being freed here were never part of the new total. Subtracting
     * them double-counted the garbage: live_bytes came out too low, which
     * shrank next_gc (live*2 + 8MB) and made the collector run more often than
     * the growth policy intends. heap_bytes is a running total, so it does need
     * the subtraction above. */
  }
}

void vy_gc_collect_ex(VyValue* extra, int n) {
  if (t_in_gc) return;
  t_in_gc = 1;
  g.mark_len = 0;
  g.live_bytes = 0;
  for (size_t i = 0; i < g.roots_len; i++)
    if (g.roots[i]) mark_value(*g.roots[i]);
  for (int i = 0; i < n; i++)
    if (extra) mark_value(extra[i]);
  mark_loop();
  sweep();
  /* trigger next time the heap roughly doubles past the live set, so a
   * steady-state program collects rarely instead of on every append */
  g.next_gc = g.live_bytes * 2 + (8 * 1024 * 1024);
  t_in_gc = 0;
}

void vy_gc_collect(void) { vy_gc_collect_ex(NULL, 0); }
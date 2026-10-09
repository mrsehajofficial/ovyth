/* Ovyth runtime :: src/arena.c
 *
 * Region/arena allocator for request-scoped allocations.
 *
 * AI agent requests have a natural lifetime: they start when a request arrives
 * and end when the response is sent. All intermediate allocations (JSON parse
 * trees, chunked text, embedding inputs, tool-call arguments, assembled context)
 * share that lifetime. Instead of malloc/free per object + GC pressure, the arena
 * lets the entire set be freed in one call -- no per-object bookkeeping, zero
 * fragmentation, and cache-friendly bump-pointer allocation.
 *
 * Usage:
 *   OvArena* a = ov_arena_new(64 * 1024);   // 64 KB initial block
 *   void* p    = ov_arena_alloc(a, 128);     // bump pointer -- O(1)
 *   OvStr* s   = ov_arena_str(a, ptr, len);  // string inside the arena
 *   ov_arena_reset(a);                        // free everything, keep block
 *   ov_arena_free(a);                         // return block to OS
 *
 * Arena strings are NOT registered with the GC -- they are valid only while
 * the arena lives. Do not store them in GC-managed containers across a reset.
 */
#include "ovrt.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ types */

typedef struct OvArenaBlock OvArenaBlock;
struct OvArenaBlock {
  OvArenaBlock* next;
  size_t        cap;
  size_t        used;
  /* data follows immediately */
};

struct OvArena {
  OvArenaBlock* head;       /* current block (bump pointer here)           */
  OvArenaBlock* spare;      /* one recycled block kept after reset()        */
  size_t        block_size; /* minimum new-block size                       */
  size_t        total_bytes;/* bytes handed to caller so far (stats)        */
  size_t        peak_bytes; /* peak total_bytes (stats)                     */
};

/* ---------------------------------------------------------------- helpers */

static OvArenaBlock* block_new(size_t cap) {
  OvArenaBlock* b = (OvArenaBlock*)malloc(sizeof(OvArenaBlock) + cap);
  if (!b) { fprintf(stderr, "ovyth: arena out of memory\n"); exit(70); }
  b->next = NULL;
  b->cap  = cap;
  b->used = 0;
  return b;
}

/* -------------------------------------------------------------- public API */

OvArena* ov_arena_new(size_t block_size) {
  if (block_size < 4096) block_size = 4096;
  OvArena* a = (OvArena*)malloc(sizeof(OvArena));
  if (!a) { fprintf(stderr, "ovyth: arena out of memory\n"); exit(70); }
  a->head        = block_new(block_size);
  a->spare       = NULL;
  a->block_size  = block_size;
  a->total_bytes = 0;
  a->peak_bytes  = 0;
  return a;
}

void* ov_arena_alloc(OvArena* a, size_t n) {
  /* Align to 8 bytes -- covers all OvValue/pointer requirements. */
  n = (n + 7u) & ~7u;
  if (!n) return NULL;

  OvArenaBlock* b = a->head;
  if (b->used + n <= b->cap) {
    /* Fast path: bump pointer in current block. */
    void* p = (char*)(b + 1) + b->used;
    b->used += n;
    a->total_bytes += n;
    if (a->total_bytes > a->peak_bytes) a->peak_bytes = a->total_bytes;
    return p;
  }

  /* Need a new block: at least block_size or the request itself. */
  size_t cap = a->block_size;
  if (n > cap) cap = n;
  OvArenaBlock* nb = block_new(cap);
  nb->next = a->head;
  a->head  = nb;
  void* p  = (char*)(nb + 1);
  nb->used = n;
  a->total_bytes += n;
  if (a->total_bytes > a->peak_bytes) a->peak_bytes = a->total_bytes;
  return p;
}

/* Allocate n zeroed bytes. */
void* ov_arena_calloc(OvArena* a, size_t n) {
  void* p = ov_arena_alloc(a, n);
  memset(p, 0, n);
  return p;
}

/* Copy a C string into the arena, returning a NUL-terminated pointer. */
char* ov_arena_strdup(OvArena* a, const char* s, size_t n) {
  char* p = (char*)ov_arena_alloc(a, n + 1);
  if (n) memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

/* Build a OvStr whose payload lives inside the arena.
 * The OvStr header itself is also in the arena (not GC-tracked).
 * Callers must NOT put this string into GC-managed containers that outlive
 * the arena. Safe for: local temporaries, JSON field names, chunk slices. */
OvStr* ov_arena_str(OvArena* a, const char* p, size_t n) {
  /* Layout: OvStr{len,hash,bytes[1]} then n-1 more bytes then NUL. */
  size_t total = sizeof(OvStr) + n; /* bytes[1] in struct counts as 1 */
  OvStr* s = (OvStr*)ov_arena_alloc(a, total);
  s->len  = (uint32_t)n;
  s->hash = ov_str_hash_n(p, n);
  if (n) memcpy(s->bytes, p, n);
  s->bytes[n] = '\0';
  return s;
}

/* Reset: free all blocks except the first (kept as spare), rewind used=0.
 * After reset the arena is reusable with zero OS calls. */
void ov_arena_reset(OvArena* a) {
  /* Walk the chain, keeping the largest block as a spare. */
  OvArenaBlock* cur = a->head;
  while (cur->next) {
    OvArenaBlock* next = cur->next;
    if (!a->spare || cur->cap > a->spare->cap) {
      free(a->spare);
      a->spare = cur;
      a->spare->next = NULL;
      a->spare->used = 0;
    } else {
      free(cur);
    }
    cur = next;
  }
  /* cur is now the last (oldest) block -- keep it as head. */
  cur->used = 0;
  a->head   = cur;
  a->total_bytes = 0;
}

/* Free everything including the arena struct itself. */
void ov_arena_free(OvArena* a) {
  if (!a) return;
  OvArenaBlock* b = a->head;
  while (b) {
    OvArenaBlock* n = b->next;
    free(b);
    b = n;
  }
  free(a->spare);
  free(a);
}

size_t ov_arena_used(OvArena* a)  { return a ? a->total_bytes : 0; }
size_t ov_arena_peak(OvArena* a)  { return a ? a->peak_bytes  : 0; }

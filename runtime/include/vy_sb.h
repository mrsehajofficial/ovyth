/* Vayu runtime :: src/vy_sb.h
 *
 * Fast string builder — avoids per-character allocation in hot loops.
 *
 * Every `acc = acc + "x"` in a loop currently does:
 *   calloc(new_size) → memcpy(a) → memcpy(b) → free(old_a)
 * For N iterations this is O(N²) memory churn.
 *
 * The builder keeps one contiguous buffer that grows geometrically
 * (like std::string / StringBuilder). Only the final VyStr is GC-tracked.
 */
#pragma once
#include <stdlib.h>
#include <string.h>
#include "vyrt.h"

typedef struct {
  char*  buf;     /* owned backing storage           */
  size_t len;     /* current length                  */
  size_t cap;     /* allocated capacity              */
} VyJsonBuf;

static inline void vy_jbuf_init(VyJsonBuf* sb) {
  sb->buf = NULL;
  sb->len = 0;
  sb->cap = 0;
}

/* Grow to at least `min_cap` bytes (excluding terminator). Returns 0 on ok. */
static inline int vy_jbuf_grow(VyJsonBuf* sb, size_t min_cap) {
  if (min_cap <= sb->cap) return 0;
  size_t new_cap = sb->cap ? sb->cap * 2 : 64;
  while (new_cap < min_cap) new_cap *= 2;
  char* nb = (char*)realloc(sb->buf, new_cap + 1); /* +1 for NUL terminator */
  if (!nb) return -1;
  sb->buf = nb;
  sb->cap = new_cap;
  return 0;
}

static inline void vy_jbuf_append(VyJsonBuf* sb, const char* p, size_t n) {
  if (vy_jbuf_grow(sb, sb->len + n)) return; /* oom — silently truncate */
  memcpy(sb->buf + sb->len, p, n);
  sb->len += n;
}

/* Push a single character. */
static inline void vy_jbuf_putc(VyJsonBuf* sb, char c) {
  if (vy_jbuf_grow(sb, sb->len + 1)) return;
  sb->buf[sb->len++] = c;
}

/* Finalize into a GC-tracked VyStr. The builder is reset afterwards. */
static inline VyStr* vy_jbuf_finish(VyJsonBuf* sb) {
  if (!sb || !sb->buf) return vy_str_new("", 0);
  VyStr* s = vy_str_new(sb->buf, sb->len);
  free(sb->buf);
  sb->buf = NULL;
  sb->len = 0;
  sb->cap = 0;
  return s;
}

static inline void vy_jbuf_free(VyJsonBuf* sb) {
  if (sb && sb->buf) { free(sb->buf); sb->buf = NULL; sb->len = 0; sb->cap = 0; }
}

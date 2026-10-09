/* Ovyth runtime :: src/ov_sb.h
 *
 * Fast string builder — avoids per-character allocation in hot loops.
 *
 * Every `acc = acc + "x"` in a loop currently does:
 *   calloc(new_size) → memcpy(a) → memcpy(b) → free(old_a)
 * For N iterations this is O(N²) memory churn.
 *
 * The builder keeps one contiguous buffer that grows geometrically
 * (like std::string / StringBuilder). Only the final OvStr is GC-tracked.
 */
#pragma once
#include <stdlib.h>
#include <string.h>
#include "ovrt.h"

typedef struct {
  char*  buf;     /* owned backing storage           */
  size_t len;     /* current length                  */
  size_t cap;     /* allocated capacity              */
} OvJsonBuf;

static inline void ov_jbuf_init(OvJsonBuf* sb) {
  sb->buf = NULL;
  sb->len = 0;
  sb->cap = 0;
}

/* Grow to at least `min_cap` bytes (excluding terminator). Returns 0 on ok. */
static inline int ov_jbuf_grow(OvJsonBuf* sb, size_t min_cap) {
  if (min_cap <= sb->cap) return 0;
  size_t new_cap = sb->cap ? sb->cap * 2 : 64;
  while (new_cap < min_cap) new_cap *= 2;
  char* nb = (char*)realloc(sb->buf, new_cap + 1); /* +1 for NUL terminator */
  if (!nb) return -1;
  sb->buf = nb;
  sb->cap = new_cap;
  return 0;
}

static inline void ov_jbuf_append(OvJsonBuf* sb, const char* p, size_t n) {
  if (ov_jbuf_grow(sb, sb->len + n)) return; /* oom — silently truncate */
  memcpy(sb->buf + sb->len, p, n);
  sb->len += n;
}

/* Push a single character. */
static inline void ov_jbuf_putc(OvJsonBuf* sb, char c) {
  if (ov_jbuf_grow(sb, sb->len + 1)) return;
  sb->buf[sb->len++] = c;
}

/* Finalize into a GC-tracked OvStr. The builder is reset afterwards. */
static inline OvStr* ov_jbuf_finish(OvJsonBuf* sb) {
  if (!sb || !sb->buf) return ov_str_new("", 0);
  OvStr* s = ov_str_new(sb->buf, sb->len);
  free(sb->buf);
  sb->buf = NULL;
  sb->len = 0;
  sb->cap = 0;
  return s;
}

static inline void ov_jbuf_free(OvJsonBuf* sb) {
  if (sb && sb->buf) { free(sb->buf); sb->buf = NULL; sb->len = 0; sb->cap = 0; }
}

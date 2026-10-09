/* Ovyth runtime :: src/string.c
 *
 * Immutable UTF-8 strings with a cached FNV-1a hash. Strings are the dominant
 * allocation in AI/automation code (prompt building, JSON, headers), so the
 * constructors are small and the hot ones avoid double work.
 *
 * Layout: one heap block, HeapObj header immediately before the OvStr body,
 * so the GC can reach every live string through the same list as lists/maps.
 */
#include "ovrt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t ov_str_hash_n(const char* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)p[i];
    h *= 16777619u;
  }
  return h;
}

/* Layout -- one allocation, so the collector can reach the header and the
 * allocator can free the whole thing with a single free():
 *
 *   +--------------+------------------------+--------+-------+
 *   | OvHeader 24B | OvStr{len, hash, bytes} | ...    | NUL   |
 *   +--------------+------------------------+--------+-------+
 *   ^ block                                   ^ s
 *
 * sizeof(OvStr) covers `len`, `hash` and one byte of tail, so the total is
 * OvHeader + OvStr + n more payload bytes + the terminator. The returned s
 * MUST point past the header, or the collector's &header[-1] lands outside the
 * block. */
#define OV_STR_ALLOC(n) (sizeof(OvHeader) + sizeof(OvStr) + (n) + 1)

OvStr* ov_str_new(const char* p, size_t n) {
  size_t bytes = OV_STR_ALLOC(n);
  char* raw = (char*)calloc(1, bytes);
  if (!raw) {
    fprintf(stderr, "ovyth: out of memory (string of %zu bytes)\n", n);
    exit(70);
  }
  OvStr* s = (OvStr*)(raw + sizeof(OvHeader));
  s->len = (uint32_t)n;
  s->hash = ov_str_hash_n(p, n);
  if (n) memcpy(s->bytes, p, n);
  s->bytes[n] = '\0';
  ov_heap_track_str(s, n);
  return s;
}

/* A literal the generated code allocates once per site (cached in a C static)
 * and reuses forever: evaluating `"key"` 200,000 times in a loop must not
 * calloc 200,000 times. Pinned with OV_HDR_PIN so sweep() keeps it alive. */
OvStr* ov_str_lit(const char* p, size_t n) {
  OvStr* s = ov_str_new(p, n);
  ((OvHeader*)s - 1)->pad |= OV_HDR_PIN;
  return s;
}

OvStr* ov_str_cstr(const char* p) { return ov_str_new(p, strlen(p)); }

OvValue ov_str_val_n(const char* p, size_t n) { return ov_str(ov_str_new(p, n)); }
OvValue ov_str_val(const char* p)             { return ov_str_val_n(p, strlen(p)); }
OvValue ov_str_of(const char* p, size_t n)    { return ov_str_val_n(p, n); }

int64_t     ov_str_len(OvStr* s)  { return s ? (int64_t)s->len : 0; }
const char* ov_str_data(OvStr* s) { return s ? s->bytes : ""; }
uint32_t    ov_str_hash(OvStr* s) { return s ? s->hash : 0; }

int ov_str_eq(OvStr* a, OvStr* b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  if (a->len != b->len || a->hash != b->hash) return 0;
  return memcmp(a->bytes, b->bytes, a->len) == 0;
}

int32_t ov_str_cmp(OvStr* a, OvStr* b) {
  size_t n = a->len < b->len ? a->len : b->len;
  int c = n ? memcmp(a->bytes, b->bytes, n) : 0;
  if (c) return c < 0 ? -1 : 1;
  if (a->len == b->len) return 0;
  return a->len < b->len ? -1 : 1;
}


OvStr* ov_str_concat(OvStr* a, OvStr* b) {
  size_t n = (size_t)a->len + b->len;
  /* One allocation, built in place.
   *
   * The previous version malloc'd a scratch buffer, copied both operands in,
   * then handed it to ov_str_new() which allocated again and copied a third
   * time -- two allocations and three copies to produce one string. Writing
   * straight into the tracked allocation removes the scratch buffer entirely.
   * The hash is computed from the finished bytes, so it never reads past the
   * end of `a` the way hashing `a`'s storage for the full length would. */
  size_t bytes = OV_STR_ALLOC(n);
  char* raw = (char*)calloc(1, bytes);
  if (!raw) {
    fprintf(stderr, "ovyth: out of memory (string of %zu bytes)\n", n);
    exit(70);
  }
  OvStr* s = (OvStr*)(raw + sizeof(OvHeader));
  s->len = (uint32_t)n;
  if (a->len) memcpy(s->bytes, a->bytes, a->len);
  if (b->len) memcpy(s->bytes + a->len, b->bytes, b->len);
  s->bytes[n] = '\0';
  s->hash = ov_str_hash_n(s->bytes, n);
  ov_heap_track_str(s, n);
  return s;
}

int64_t ov_str_find(OvStr* hay, OvStr* needle, int64_t from) {
  int64_t n = (int64_t)hay->len;
  if (from < 0) from = 0;
  if (!needle->len) return from <= n ? from : -1;
  if ((int64_t)needle->len > n) return -1;
  if (from > n - (int64_t)needle->len) return -1;
  const char* start = hay->bytes + from;
  const char* end = hay->bytes + n - needle->len;
  for (const char* p = start; p <= end;) {
    const char* hit = (const char*)memchr(p, needle->bytes[0], (size_t)(end - p + 1));
    if (!hit) return -1;
    if (memcmp(hit, needle->bytes, needle->len) == 0)
      return (int64_t)(hit - hay->bytes);
    p = hit + 1;
  }
  return -1;
}

int ov_str_contains(OvStr* hay, OvStr* needle) {
  return ov_str_find(hay, needle, 0) >= 0;
}

OvStr* ov_str_slice(OvStr* s, int64_t lo, int64_t hi) {
  int64_t n = (int64_t)s->len;
  if (lo < 0) lo += n;
  if (hi < 0) hi += n;
  if (lo < 0) lo = 0;
  if (hi > n) hi = n;
  if (hi < lo) hi = lo;
  return ov_str_new(s->bytes + lo, (size_t)(hi - lo));
}

OvStr* ov_str_upper(OvStr* s) {
  OvStr* r = ov_str_new(s->bytes, s->len);
  for (uint32_t i = 0; i < r->len; i++)
    r->bytes[i] = (char)toupper((unsigned char)r->bytes[i]);
  return r;
}

OvStr* ov_str_lower(OvStr* s) {
  OvStr* r = ov_str_new(s->bytes, s->len);
  for (uint32_t i = 0; i < r->len; i++)
    r->bytes[i] = (char)tolower((unsigned char)r->bytes[i]);
  return r;
}

OvStr* ov_str_trim(OvStr* s) {
  uint32_t i = 0, j = s->len;
  while (i < j && isspace((unsigned char)s->bytes[i])) i++;
  while (j > i && isspace((unsigned char)s->bytes[j - 1])) j--;
  return ov_str_new(s->bytes + i, j - i);
}

OvStr* ov_str_repeat(OvStr* s, int64_t n) {
  if (n <= 0 || s->len == 0) return ov_str_new("", 0);
  size_t total = s->len * (size_t)n;
  OvStr* r = ov_str_new(s->bytes, total);
  for (size_t off = s->len; off < total; off += s->len)
    memcpy(r->bytes + off, s->bytes, s->len);
  return r;
}

OvStr* ov_str_pad(OvStr* s, int64_t width, int64_t fill, int left) {
  if (width <= (int64_t)s->len) return ov_str_new(s->bytes, s->len);
  char c = (char)(fill ? fill : ' ');
  /* Built from a repeated 1-char string rather than a malloc'd scratch buffer:
   * ov_str_new copies exactly `n` bytes, so the padding region cannot be read
   * out of a shorter source. */
  OvStr* pad = ov_str_repeat(ov_str_new(&c, 1), width - (int64_t)s->len);
  return left ? ov_str_concat(pad, s) : ov_str_concat(s, pad);
}

OvStr* ov_str_replace(OvStr* s, OvStr* from, OvStr* to) {
  if (!from->len) return ov_str_new(s->bytes, s->len);
  ov_gc_begin_mutation();
  OvList* parts = ov_list_new();
  int64_t pos = 0;
  for (;;) {
    int64_t at = ov_str_find(s, from, pos);
    if (at < 0) break;
    if (at > pos)
      ov_list_push(parts, ov_str_val_n(s->bytes + pos, (size_t)(at - pos)));
    ov_list_push(parts, ov_str(to));
    pos = at + (int64_t)from->len;
  }
  if (pos < (int64_t)s->len)
    ov_list_push(parts, ov_str_val_n(s->bytes + pos, s->len - (size_t)pos));
  size_t total = 0;
  for (uint32_t i = 0; i < parts->len; i++) total += parts->items[i].str->len;
  OvStr* r = ov_str_new(s->bytes, total);
  size_t off = 0;
  for (uint32_t i = 0; i < parts->len; i++) {
    memcpy(r->bytes + off, parts->items[i].str->bytes, parts->items[i].str->len);
    off += parts->items[i].str->len;
  }
  ov_gc_end_mutation();
  return r;
}

static uint32_t utf8_width(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;  /* invalid lead byte: consume one and let the caller see it */
}

OvValue ov_str_split(OvStr* s, OvStr* sep) {
  ov_gc_begin_mutation();
  OvList* out = ov_list_new();
  if (!sep->len) {
    OvValue res = ov_str_chars(s);
    ov_gc_end_mutation();
    return res;
  }
  int64_t pos = 0;
  for (;;) {
    int64_t at = ov_str_find(s, sep, pos);
    if (at < 0) break;
    ov_list_push(out, ov_str_val_n(s->bytes + pos, (size_t)(at - pos)));
    pos = at + (int64_t)sep->len;
  }
  ov_list_push(out, ov_str_val_n(s->bytes + pos, s->len - (size_t)pos));
  ov_gc_end_mutation();
  return ov_list(out);
}

OvValue ov_str_chars(OvStr* s) {
  ov_gc_begin_mutation();
  OvList* out = ov_list_new_cap(s->len);
  for (uint32_t i = 0; i < s->len;) {
    uint32_t w = utf8_width((unsigned char)s->bytes[i]);
    if (i + w > s->len) w = 1;
    ov_list_push(out, ov_str_val_n(s->bytes + i, w));
    i += w;
  }
  ov_gc_end_mutation();
  return ov_list(out);
}

OvValue ov_str_bytes(OvStr* s) {
  ov_gc_begin_mutation();
  OvList* out = ov_list_new_cap(s->len);
  for (uint32_t i = 0; i < s->len; i++)
    ov_list_push(out, ov_int((unsigned char)s->bytes[i]));
  ov_gc_end_mutation();
  return ov_list(out);
}


/* --------------------------------------------------------------- builder
 * A plain growable byte buffer: appends copy only the new bytes, and the
 * backing array doubles, so building n bytes costs O(n) total instead of the
 * O(n^2) of `acc = acc + chunk`. It is *not* a GC object -- it is an ordinary
 * malloc the caller owns -- so nothing here needs rooting. `ov_sb_finish`
 * hands the bytes to the normal tracked string and frees the buffer. */
struct OvStrBuilder {
  char*  buf;
  size_t len;
  size_t cap;
};

OvStrBuilder* ov_sb_new(void) {
  OvStrBuilder* sb = (OvStrBuilder*)malloc(sizeof(OvStrBuilder));
  if (!sb) { fprintf(stderr, "ovyth: out of memory (string builder)\n"); exit(70); }
  sb->cap = 64;
  sb->len = 0;
  sb->buf = (char*)malloc(sb->cap);
  if (!sb->buf) { free(sb); fprintf(stderr, "ovyth: out of memory (string builder)\n"); exit(70); }
  return sb;
}

void ov_sb_free(OvStrBuilder* sb) {
  if (!sb) return;
  free(sb->buf);
  free(sb);
}

void ov_sb_append(OvStrBuilder* sb, const char* p, size_t n) {
  if (!sb || n == 0) return;
  if (sb->len + n > sb->cap) {
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < sb->len + n) cap *= 2;
    char* nb = (char*)realloc(sb->buf, cap);
    if (!nb) { fprintf(stderr, "ovyth: out of memory (string builder)\n"); exit(70); }
    sb->buf = nb;
    sb->cap = cap;
  }
  memcpy(sb->buf + sb->len, p, n);
  sb->len += n;
}

void     ov_sb_append_str(OvStrBuilder* sb, OvStr* s) { if (s) ov_sb_append(sb, s->bytes, s->len); }

/* Append any Ovyth value, rendered exactly as print() would show it. */
void ov_sb_append_value(OvStrBuilder* sb, OvValue v) {
  OvStr* r = ov_render(v);
  ov_sb_append(sb, r->bytes, r->len);
}
size_t   ov_sb_len(OvStrBuilder* sb) { return sb ? sb->len : 0; }

OvStr* ov_sb_finish(OvStrBuilder* sb) {
  if (!sb) return ov_str_new("", 0);
  OvStr* out = ov_str_new(sb->buf, sb->len);
  ov_sb_free(sb);
  return out;
}

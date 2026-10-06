/* Vayu runtime :: src/json_fast.c
 *
 * Fast JSON parser that reuses a temporary buffer instead of allocating
 * a fresh VyStr for every character in a string literal.  This reduces
 * a 60-char JSON string from ~61 calloc calls down to ~1-2.
 *
 * The parsed map/list values are still normal GC-tracked objects so they
 * survive beyond the parse call.
 */
#include "vyrt.h"
#include "vy_sb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 200

typedef struct {
  const char* p;
  const char* end;
  int         depth;
  int         failed;
  char        err[192];
} J;

static VyValue parse_value(J* j, VyJsonBuf* sb);

static void fail(J* j, const char* msg) {
  if (!j->failed) {
    j->failed = 1;
    snprintf(j->err, sizeof(j->err), "%s at offset %ld", msg, (long)(j->p - j->end));
  }
}

static void skip_ws(J* j) {
  while (j->p < j->end) {
    char c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
    else break;
  }
}

static int at(J* j, char c) { return j->p < j->end && *j->p == c; }

static int lit(J* j, const char* s) {
  size_t n = strlen(s);
  if ((size_t)(j->end - j->p) < n || memcmp(j->p, s, n) != 0) return 0;
  j->p += n;
  return 1;
}

static int hex4(const char* p, uint32_t* out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

static void emit_utf8(VyJsonBuf* sb, uint32_t cp) {
  char buf[4];
  int n = 0;
  if (cp < 0x80) {
    buf[n++] = (char)cp;
  } else if (cp < 0x800) {
    buf[n++] = (char)(0xC0 | (cp >> 6));
    buf[n++] = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    buf[n++] = (char)(0xE0 | (cp >> 12));
    buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[n++] = (char)(0x80 | (cp & 0x3F));
  } else {
    buf[n++] = (char)(0xF0 | (cp >> 18));
    buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[n++] = (char)(0x80 | (cp & 0x3F));
  }
  vy_jbuf_append(sb, buf, (size_t)n);
}

/*
 * Parse a JSON string value into `sb`.
 * Reuses the same VyJsonBuf across calls — only one heap block per
 * logical string regardless of escape sequence count.
 */
static int parse_json_string(J* j, VyJsonBuf* sb) {
  if (!at(j, '"')) return 0;
  j->p++;
  vy_jbuf_init(sb);
  while (j->p < j->end && *j->p != '"') {
    unsigned char c = (unsigned char)*j->p;
    if (c == '\\') {
      j->p++;
      if (j->p >= j->end) break;
      char e = *j->p++;
      switch (e) {
        case '"':  vy_jbuf_putc(sb, '"');  break;
        case '\\': vy_jbuf_putc(sb, '\\'); break;
        case '/':  vy_jbuf_putc(sb, '/');  break;
        case 'b':  vy_jbuf_putc(sb, '\b'); break;
        case 'f':  vy_jbuf_putc(sb, '\f'); break;
        case 'n':  vy_jbuf_putc(sb, '\n'); break;
        case 'r':  vy_jbuf_putc(sb, '\r'); break;
        case 't':  vy_jbuf_putc(sb, '\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(j->p, &cp)) { fail(j, "bad unicode escape"); return 0; }
          j->p += 4;
          /* Handle surrogate pairs */
          if (cp >= 0xDC00 && cp <= 0xDFFF) {
            fail(j, "unexpected low surrogate"); return 0;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (j->p + 2 < j->end && j->p[0] == '\\' && j->p[1] == 'u') {
              uint32_t cp2;
              if (hex4(j->p + 2, &cp2) && cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
                j->p += 6;
              } else {
                fail(j, "malformed surrogate pair"); return 0;
              }
            } else {
              fail(j, "unexpected high surrogate"); return 0;
            }
          }
          emit_utf8(sb, cp);
          break;
        }
        default:
          fail(j, "bad escape"); return 0;
      }
    } else {
      vy_jbuf_putc(sb, (char)c);
      j->p++;
    }
  }
  if (j->p >= j->end) { fail(j, "unterminated string"); return 0; }
  j->p++; /* skip closing '"' */
  return 1;
}

static VyValue parse_value(J* j, VyJsonBuf* sb) {
  skip_ws(j);
  if (j->p >= j->end) { fail(j, "unexpected EOF"); return vy_nil(); }

  char c = *j->p;

  /* --- string --------------------------------------------------------- */
  if (c == '"') {
    if (!parse_json_string(j, sb)) return vy_nil();
    VyStr* s = vy_jbuf_finish(sb);
    return vy_str(s);
  }

  /* --- number --------------------------------------------------------- */
  if (c == '-' || (c >= '0' && c <= '9')) {
    char* end;
    double d = strtod(j->p, &end);
    if (end == j->p) { fail(j, "bad number"); return vy_nil(); }
    j->p = end;
    skip_ws(j);
    if (*j->p == '.') {
      /* float: re-parse including decimal point */
      char buf[64];
      size_t n = (size_t)(j->p - end); /* already consumed integer part */
      /* re-scan from start */
      {
        const char* q = j->p - n;
        size_t m = 0;
        while (q < j->p && m < sizeof(buf)-1) buf[m++] = *q++;
        while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' &&
               *j->p != ' '  && *j->p != '\t' && *j->p != '\n' && *j->p != '\r') {
          if (m < sizeof(buf)-1) buf[m++] = *j->p++;
        }
        buf[m] = '\0';
        d = strtod(buf, NULL);
      }
      return vy_float(d);
    }
    int64_t i64 = (int64_t)d;
    if ((double)i64 == d) return vy_int(i64);
    return vy_float(d);
  }

  /* --- object --------------------------------------------------------- */
  if (c == '{') {
    j->p++;
    if (j->depth++ > JSON_MAX_DEPTH) { fail(j, "object too deep"); return vy_nil(); }
    VyMap* m = vy_map_new();
    skip_ws(j);
    if (at(j, '}')) { j->p++; j->depth--; return vy_map(m); }
    while (1) {
      skip_ws(j);
      /* key must be a string */
      VyJsonBuf key_sb;
      if (!parse_json_string(j, &key_sb)) { j->depth--; return vy_nil(); }
      VyStr* key_s = vy_jbuf_finish(&key_sb);
      VyValue key_v = vy_str(key_s);
      skip_ws(j);
      if (!at(j, ':')) { fail(j, "expected ':'"); j->depth--; return vy_nil(); }
      j->p++;
      skip_ws(j);
      VyValue val = parse_value(j, sb);
      vy_map_set(m, key_v, val);
      skip_ws(j);
      if (at(j, ',')) { j->p++; continue; }
      if (at(j, '}')) { j->p++; break; }
      fail(j, "expected ',' or '}'"); j->depth--; return vy_nil();
    }
    j->depth--;
    return vy_map(m);
  }

  /* --- array ---------------------------------------------------------- */
  if (c == '[') {
    j->p++;
    if (j->depth++ > JSON_MAX_DEPTH) { fail(j, "array too deep"); return vy_nil(); }
    VyList* l = vy_list_new();
    skip_ws(j);
    if (at(j, ']')) { j->p++; j->depth--; return vy_list(l); }
    while (1) {
      skip_ws(j);
      VyValue elem = parse_value(j, sb);
      vy_list_push(l, elem);
      skip_ws(j);
      if (at(j, ',')) { j->p++; continue; }
      if (at(j, ']')) { j->p++; break; }
      fail(j, "expected ',' or ']'"); j->depth--; return vy_nil();
    }
    j->depth--;
    return vy_list(l);
  }

  /* --- literals ------------------------------------------------------- */
  if (lit(j, "true"))  return vy_bool(1);
  if (lit(j, "false")) return vy_bool(0);
  if (lit(j, "null"))  return vy_nil();

  fail(j, "unexpected character");
  return vy_nil();
}

/* --------------------------------------------------------------------- public */

VyValue vy_json_parse_fast(const char* text, size_t len, VyArena* arena) {
  (void)arena;
  J j;
  j.p      = text;
  j.end    = text + len;
  j.depth  = 0;
  j.failed = 0;
  /* One reusable buffer per parse call. */
  VyJsonBuf sb;
  vy_jbuf_init(&sb);
  /* Disable GC for the duration: intermediate C-stack locals (VyMap*, VyStr*,
   * sub-value VyValues) are not registered as GC roots. A collection triggered
   * by any allocation inside parse_value would free live objects. */
  vy_gc_begin_mutation();
  VyValue v = parse_value(&j, &sb);
  vy_gc_end_mutation();
  vy_jbuf_free(&sb);
  if (j.failed) {
    /* On failure, try the legacy parser for the error message. */
    (void)vy_json_parse(text, len);
    return vy_nil();
  }
  skip_ws(&j);
  if (j.p < j.end) fail(&j, "trailing data");
  return v;
}

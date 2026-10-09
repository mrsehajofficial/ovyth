/* Ovyth runtime :: src/json.c
 *
 * JSON <-> native values. Parsing builds the language's own list/map values,
 * so `response.json["choices"][0]["message"]["content"]` needs no glue code:
 * this is the single place where JSON shapes become Ovyth shapes.
 *
 * Hand-written recursive-descent parser with an explicit depth limit and
 * strict trailing-comma / duplicate handling, because an API response is
 * untrusted input.
 */
#include "ovrt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 200

typedef struct {
  const char* p;
  const char* end;
  int depth;
  int failed;
  char err[192];
} J;

static OvValue parse_value(J* j);

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

static void encode_utf8(OvStr** out, uint32_t cp) {
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
  *out = ov_str_concat(*out, ov_str_new(buf, (size_t)n));
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

static OvValue parse_string(J* j) {
  if (!at(j, '"')) { fail(j, "expected a string"); return ov_nil(); }
  j->p++;
  OvStr* out = ov_str_new("", 0);
  while (j->p < j->end && *j->p != '"') {
    unsigned char c = (unsigned char)*j->p;
    if (c == '\\') {
      j->p++;
      if (j->p >= j->end) break;
      char e = *j->p++;
      switch (e) {
        case '"':  out = ov_str_concat(out, ov_str_new("\"", 1)); break;
        case '\\': out = ov_str_concat(out, ov_str_new("\\", 1)); break;
        case '/':  out = ov_str_concat(out, ov_str_new("/", 1));  break;
        case 'b':  out = ov_str_concat(out, ov_str_new("\b", 1)); break;
        case 'f':  out = ov_str_concat(out, ov_str_new("\f", 1)); break;
        case 'n':  out = ov_str_concat(out, ov_str_new("\n", 1)); break;
        case 'r':  out = ov_str_concat(out, ov_str_new("\r", 1)); break;
        case 't':  out = ov_str_concat(out, ov_str_new("\t", 1)); break;
        case 'u': {
          uint32_t cp;
          if (j->end - j->p < 4 || !hex4(j->p, &cp)) { fail(j, "bad \\u escape"); return ov_nil(); }
          j->p += 4;
          /* surrogate pair */
          if (cp >= 0xD800 && cp <= 0xDBFF && j->end - j->p >= 6 &&
              j->p[0] == '\\' && j->p[1] == 'u') {
            uint32_t lo;
            if (hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              j->p += 6;
            }
          }
          encode_utf8(&out, cp);
          break;
        }
        default: fail(j, "unknown escape"); return ov_nil();
      }
      continue;
    }
    if (c < 0x20) { fail(j, "control character in string"); return ov_nil(); }
    out = ov_str_concat(out, ov_str_new((const char*)j->p, 1));
    j->p++;
  }
  if (!at(j, '"')) { fail(j, "unterminated string"); return ov_nil(); }
  j->p++;
  return ov_str(out);
}

static OvValue parse_number(J* j) {
  const char* start = j->p;
  if (at(j, '-')) j->p++;
  while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  int is_float = 0;
  if (at(j, '.')) {
    is_float = 1;
    j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    is_float = 1;
    j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (j->p == start) { fail(j, "expected a number"); return ov_nil(); }
  size_t n = (size_t)(j->p - start);
  char buf[64];
  if (n >= sizeof(buf)) { fail(j, "number too long"); return ov_nil(); }
  memcpy(buf, start, n);
  buf[n] = '\0';
  if (!is_float) {
    char* endp = NULL;
    long long v = strtoll(buf, &endp, 10);
    if (endp && *endp == '\0') return ov_int((int64_t)v);
  }
  return ov_float(strtod(buf, NULL));
}

static OvValue parse_array(J* j) {
  j->p++;  /* [ */
  OvList* out = ov_list_new();
  skip_ws(j);
  if (at(j, ']')) { j->p++; return ov_list(out); }
  for (;;) {
    OvValue v = parse_value(j);
    if (j->failed) return ov_nil();
    ov_list_push(out, v);
    skip_ws(j);
    if (at(j, ',')) { j->p++; skip_ws(j); continue; }
    break;
  }
  if (!at(j, ']')) { fail(j, "expected ',' or ']'"); return ov_nil(); }
  j->p++;
  return ov_list(out);
}

static OvValue parse_object(J* j) {
  j->p++;  /* { */
  OvMap* out = ov_map_new();
  skip_ws(j);
  if (at(j, '}')) { j->p++; return ov_map(out); }
  for (;;) {
    skip_ws(j);
    OvValue key = parse_string(j);
    if (j->failed) return ov_nil();
    skip_ws(j);
    if (!at(j, ':')) { fail(j, "expected ':'"); return ov_nil(); }
    j->p++;
    OvValue val = parse_value(j);
    if (j->failed) return ov_nil();
    ov_map_set(out, key, val);
    skip_ws(j);
    if (at(j, ',')) { j->p++; skip_ws(j); continue; }
    break;
  }
  if (!at(j, '}')) { fail(j, "expected ',' or '}'"); return ov_nil(); }
  j->p++;
  return ov_map(out);
}

static OvValue parse_value(J* j) {
  skip_ws(j);
  if (j->p >= j->end) { fail(j, "unexpected end of input"); return ov_nil(); }
  if (++j->depth > JSON_MAX_DEPTH) { fail(j, "nesting too deep"); j->depth--; return ov_nil(); }
  OvValue out;
  char c = *j->p;
  if (c == '{') out = parse_object(j);
  else if (c == '[') out = parse_array(j);
  else if (c == '"') out = parse_string(j);
  else if (lit(j, "true")) out = ov_bool(1);
  else if (lit(j, "false")) out = ov_bool(0);
  else if (lit(j, "null")) out = ov_nil();
  else if (c == '-' || (c >= '0' && c <= '9')) out = parse_number(j);
  else { fail(j, "unexpected character"); out = ov_nil(); }
  j->depth--;
  return out;
}

OvValue ov_json_parse(const char* text, size_t len) {
  J j = {.p = text, .end = text + len, .depth = 0, .failed = 0, .err = {0}};
  ov_gc_begin_mutation();
  OvValue v = parse_value(&j);
  ov_gc_end_mutation();
  if (j.failed) return ov_nil();
  skip_ws(&j);
  if (j.p != j.end) return ov_nil();
  return v;
}

int ov_json_valid(const char* text, size_t len) {
  J j = {.p = text, .end = text + len, .depth = 0, .failed = 0, .err = {0}};
  ov_gc_begin_mutation();
  OvValue v = parse_value(&j);
  ov_gc_end_mutation();
  (void)v;
  if (j.failed) return 0;
  skip_ws(&j);
  return j.p == j.end;
}

/* The error text of the last failed parse, for `json.parse` diagnostics. */
const char* ov_json_last_error(void) {
  extern const char* ov_json_error_slot(void);
  return ov_json_error_slot();
}

static char g_json_err[192];
const char* ov_json_error_slot(void) { return g_json_err; }

/* ------------------------------------------------------------ stringify */

/* Direct append into one buffer: stringify is on the hot path for every API
 * request, so it never builds an intermediate list of fragments. */
typedef struct {
  char*  buf;
  size_t len, cap;
  int    depth;
  int    failed;
} S;

static void s_init(S* s) {
  s->cap = 256;
  s->len = 0;
  s->buf = (char*)malloc(s->cap);
  s->depth = 0;
  s->failed = 0;
  if (!s->buf) { s->failed = 1; return; }
  s->buf[0] = '\0';
}

static void s_free(S* s) { free(s->buf); s->buf = NULL; }

static void s_raw(S* s, const char* p, size_t n) {
  if (s->failed) return;
  if (s->len + n + 1 > s->cap) {
    while (s->len + n + 1 > s->cap) s->cap *= 2;
    char* nb = (char*)realloc(s->buf, s->cap);
    if (!nb) { s->failed = 1; return; }
    s->buf = nb;
  }
  memcpy(s->buf + s->len, p, n);
  s->len += n;
  s->buf[s->len] = '\0';
}

static void s_str(S* s, const char* p) { s_raw(s, p, strlen(p)); }
static void s_ch(S* s, char c) { s_raw(s, &c, 1); }

static void s_json_string(S* s, const char* p, size_t n) {
  s_ch(s, '"');
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    switch (c) {
      case '"':  s_str(s, "\\\""); break;
      case '\\': s_str(s, "\\\\"); break;
      case '\n': s_str(s, "\\n");  break;
      case '\r': s_str(s, "\\r");  break;
      case '\t': s_str(s, "\\t");  break;
      case '\b': s_str(s, "\\b");  break;
      case '\f': s_str(s, "\\f");  break;
      default:
        if (c < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          s_str(s, esc);
        } else {
          s_ch(s, (char)c);
        }
    }
  }
  s_ch(s, '"');
}

static void s_double(S* s, double d) {
  char buf[40];
  if (isnan(d) || isinf(d)) { s_str(s, "null"); return; }
  if (d == (double)(int64_t)d && d < 1e15 && d > -1e15) {
    snprintf(buf, sizeof(buf), "%lld.0", (long long)d);
  } else {
    snprintf(buf, sizeof(buf), "%.17g", d);
  }
  s_str(s, buf);
}

static void s_value(S* s, OvValue v) {
  if (s->failed) return;
  if (++s->depth > JSON_MAX_DEPTH) { s->failed = 1; s->depth--; return; }
  switch (ov_tagof(v)) {
    case OV_NIL:    s_str(s, "null"); break;
    case OV_BOOL:   s_str(s, v.b ? "true" : "false"); break;
    case OV_INT: {
      char buf[24];
      snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
      s_str(s, buf);
      break;
    }
    case OV_FLOAT:  s_double(s, v.f); break;
    case OV_STRING: s_json_string(s, v.str->bytes, v.str->len); break;
    case OV_LIST: {
      OvList* l = v.list;
      s_ch(s, '[');
      for (uint32_t i = 0; i < l->len; i++) {
        if (i) s_ch(s, ',');
        s_value(s, l->items[i]);
      }
      s_ch(s, ']');
      break;
    }
    case OV_MAP: {
      OvMap* m = v.map;
      s_ch(s, '{');
      for (uint32_t i = 0; i < m->cap; i++) {
        if (m->entries[i].key.tag == 0 && m->entries[i].val.tag == 0 &&
            m->entries[i].key.i == 0 && m->entries[i].val.i == 0)
          continue;  /* empty slot */
        if (s->len && s->buf[s->len - 1] != '{') s_ch(s, ',');
        OvValue k = m->entries[i].key;
        if (ov_tagof(k) == OV_STRING) s_json_string(s, k.str->bytes, k.str->len);
        else {
          OvStr* ks = ov_json_stringify(k);
          s_raw(s, ks->bytes, ks->len);
        }
        s_ch(s, ':');
        s_value(s, m->entries[i].val);
      }
      s_ch(s, '}');
      break;
    }
    default: s_str(s, "null"); break;
  }
  s->depth--;
}

OvStr* ov_json_stringify(OvValue v) {
  S s;
  s_init(&s);
  s_value(&s, v);
  if (s.failed) {
    s_free(&s);
    snprintf(g_json_err, sizeof(g_json_err), "value cannot be serialised to JSON");
    return ov_str_new("", 0);
  }
  OvStr* out = ov_str_new(s.buf, s.len);
  s_free(&s);
  return out;
}

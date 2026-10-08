/* Vayu runtime :: src/console.c
 *
 * Terminal I/O and the value renderer behind `print`.
 *
 * print() renders a value the way a developer expects to see it: strings
 * bare, maps and lists as JSON, strings inside containers quoted. That single
 * rule means `print(response)` is readable and `print(["a","b"])` is valid
 * JSON, which is what makes debugging an API script pleasant.
 */
#include "vyrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int    g_argc = 0;
char** g_argv = NULL;

void vy_runtime_init(int argc, char** argv) {
  g_argc = argc;
  g_argv = argv;
}

void vy_runtime_shutdown(void) {
  fflush(stdout);
  fflush(stderr);
  /* Run the collector once at exit so leaks in long-running services are
   * visible in vy_heap_bytes() during benchmarks. */
  vy_gc_collect();
}

void vy_flush(void) { fflush(stdout); }

/* ------------------------------------------------------------- rendering */

typedef struct {
  char*  buf;
  size_t len, cap;
  int    failed;
} Buf;

static void b_init(Buf* b) {
  b->cap = 256;
  b->len = 0;
  b->buf = (char*)malloc(b->cap);
  b->failed = b->buf == NULL;
  if (!b->failed) b->buf[0] = '\0';
}

static void b_raw(Buf* b, const char* p, size_t n) {
  if (b->failed) return;
  if (b->len + n + 1 > b->cap) {
    while (b->len + n + 1 > b->cap) b->cap *= 2;
    char* nb = (char*)realloc(b->buf, b->cap);
    if (!nb) { b->failed = 1; return; }
    b->buf = nb;
  }
  memcpy(b->buf + b->len, p, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static void b_str(Buf* b, const char* s) { b_raw(b, s, strlen(s)); }

static void render(Buf* b, VyValue v, int depth, int quoted);

static void render_string(Buf* b, VyStr* s) {
  b_str(b, "\"");
  for (uint32_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->bytes[i];
    switch (c) {
      case '"':  b_str(b, "\\\""); break;
      case '\\': b_str(b, "\\\\"); break;
      case '\n': b_str(b, "\\n");  break;
      case '\r': b_str(b, "\\r");  break;
      case '\t': b_str(b, "\\t");  break;
      case '\b': b_str(b, "\\b");  break;
      case '\f': b_str(b, "\\f");  break;
      default:
        if (c < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          b_str(b, esc);
        } else {
          b_raw(b, (const char*)&c, 1);
        }
    }
  }
  b_str(b, "\"");
}

static void render(Buf* b, VyValue v, int depth, int quoted) {
  char num[64];
  switch (vy_tagof(v)) {
    case VY_NIL:    b_str(b, "nil"); break;
    case VY_BOOL:   b_str(b, v.b ? "true" : "false"); break;
    case VY_INT:
      snprintf(num, sizeof(num), "%lld", (long long)v.i);
      b_str(b, num);
      break;
    case VY_FLOAT:
      snprintf(num, sizeof(num), "%g", v.f);
      b_str(b, num);
      break;
    case VY_STRING:
      if (quoted) render_string(b, v.str);
      else b_raw(b, v.str->bytes, v.str->len);
      break;
    case VY_LIST: {
      if (depth > 64) { b_str(b, "[...]"); break; }
      b_str(b, "[");
      for (uint32_t i = 0; i < v.list->len; i++) {
        if (i) b_str(b, ", ");
        render(b, v.list->items[i], depth + 1, 1);
      }
      b_str(b, "]");
      break;
    }
    case VY_I64A: {
      if (depth > 64) { b_str(b, "[...]"); break; }
      b_str(b, "[");
      for (uint32_t i = 0; i < v.i64a->len; i++) {
        if (i) b_str(b, ", ");
        snprintf(num, sizeof(num), "%lld", (long long)v.i64a->data[i]);
        b_str(b, num);
      }
      b_str(b, "]");
      break;
    }
    case VY_F64A: {
      if (depth > 64) { b_str(b, "[...]"); break; }
      b_str(b, "[");
      for (uint32_t i = 0; i < v.f64a->len; i++) {
        if (i) b_str(b, ", ");
        snprintf(num, sizeof(num), "%g", v.f64a->data[i]);
        b_str(b, num);
      }
      b_str(b, "]");
      break;
    }
    case VY_STRA: {
      if (depth > 64) { b_str(b, "[...]"); break; }
      b_str(b, "[");
      for (uint32_t i = 0; i < v.stra->len; i++) {
        if (i) b_str(b, ", ");
        render_string(b, v.stra->data[i]);
      }
      b_str(b, "]");
      break;
    }
    case VY_MAP: {
      if (depth > 64) { b_str(b, "{...}"); break; }
      b_str(b, "{");
      int first = 1;
      VyMap* m = v.map;
      for (uint32_t i = 0; i < m->cap; i++) {
        if (m->entries[i].key.tag == 0 && m->entries[i].val.tag == 0 &&
            m->entries[i].key.i == 0 && m->entries[i].val.i == 0)
          continue;
        if (!first) b_str(b, ", ");
        first = 0;
        VyValue k = m->entries[i].key;
        if (vy_tagof(k) == VY_STRING) {
          if (quoted) render_string(b, k.str);
          else b_raw(b, k.str->bytes, k.str->len);
        } else {
          render(b, k, depth + 1, 1);
        }
        b_str(b, ": ");
        render(b, m->entries[i].val, depth + 1, 1);
      }
      b_str(b, "}");
      break;
    }
    case VY_FUNC: {
      VyStr* n = v.fn->name;
      b_str(b, "<function ");
      if (n) b_raw(b, n->bytes, n->len);
      b_str(b, ">");
      break;
    }
  }
}

/* Human-readable form used by print() and by string concatenation of
 * non-string values. */
VyStr* vy_render(VyValue v) {
  /* Scalars are the hot case -- str(i), coercion in `"x" + 42`, print of a
   * number. They used to take the Buf path: a 256-byte malloc, the render,
   * a copy into a fresh VyStr, then a free. One stack buffer and one
   * allocation produce byte-identical output; only containers still need
   * the growable builder.
   *
   * Strings return the SAME object: every VyStr in the runtime is immutable
   * by construction (audited -- bytes[] is only ever written to a freshly
   * allocated block), so a copy buys nothing and costs an allocation per
   * print(), per str(), and per builder append. */
  static VyStr* s_nil = NULL, *s_true = NULL, *s_false = NULL;
  char num[64];
  switch (vy_tagof(v)) {
    case VY_NIL:    if (!s_nil)   s_nil   = vy_str_lit("nil", 3);   return s_nil;
    case VY_BOOL:   if (v.b) { if (!s_true)  s_true  = vy_str_lit("true", 4);  return s_true; }
                    if (!s_false) s_false = vy_str_lit("false", 5); return s_false;
    case VY_INT: {
      /* Hand-rolled: snprintf("%lld") costs ~150ns of format parsing, and
       * mapops.vy builds 200k int keys. INT64_MIN is handled by negating
       * without overflowing. */
      int64_t iv = v.i;
      char* p = num + sizeof num;
      uint64_t u = iv < 0 ? (uint64_t)(-(iv + 1)) + 1 : (uint64_t)iv;
      do { *--p = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
      if (iv < 0) *--p = '-';
      return vy_str_new(p, (size_t)((num + sizeof num) - p));
    }
    case VY_FLOAT:  snprintf(num, sizeof num, "%g", v.f);
                    return vy_str_new(num, strlen(num));
    case VY_STRING: return v.str;   /* immutable -- no copy needed */
    default: break;
  }
  Buf b;
  b_init(&b);
  render(&b, v, 0, 1);   /* list / map / func: containers render quoted */
  if (b.failed) { free(b.buf); return vy_str_new("<render failed>", 15); }
  VyStr* out = vy_str_new(b.buf, b.len);
  free(b.buf);
  return out;
}

/* Strict form: everything as valid JSON, used by json.stringify and by the
 * error reporter. */
VyStr* vy_repr(VyValue v) { return vy_json_stringify(v); }

/* ---------------------------------------------------------------- output */

void vy_print(VyStr* s) {
  if (!s) return;
  fwrite(s->bytes, 1, s->len, stdout);
}

void vy_print_nl(VyStr* s) {
  if (s && s->len) fwrite(s->bytes, 1, s->len, stdout);
  fputc('\n', stdout);
}

void vy_print_err(VyStr* s) {
  if (!s) return;
  fflush(stdout);
  fwrite(s->bytes, 1, s->len, stderr);
  fputc('\n', stderr);
  fflush(stderr);
}

VyStr* vy_read_line(void) {
  size_t cap = 128, len = 0;
  char* buf = (char*)malloc(cap);
  if (!buf) return vy_str_new("", 0);
  int c;
  for (;;) {
    c = fgetc(stdin);
    if (c == EOF || c == '\n') break;
    if (len + 1 >= cap) {
      cap *= 2;
      char* nb = (char*)realloc(buf, cap);
      if (!nb) break;
      buf = nb;
    }
    buf[len++] = (char)c;
  }
  if (len == 0 && c == EOF) {
    free(buf);
    return NULL;  /* end of input; the compiler turns this into nil */
  }
  if (len > 0 && buf[len - 1] == '\r') len--;
  VyStr* s = vy_str_new(buf, len);
  free(buf);
  return s;
}
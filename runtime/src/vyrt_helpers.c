// Vayu runtime :: src/vyrt_helpers.c
//
// Thin C layer the native codegen emits calls into. It concentrates the
// runtime plumbing that would otherwise bloat every generated expression:
// value-method dispatch and the http.* namespace. Keeping it here means the
// emitter stays small and stays one-to-one with the interpreter's
// `interp_builtins.cpp`, so the two backends cannot drift apart.
#include "../include/vyrt_helpers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ state */
static VyValue g_err;

VyValue vy_h_err_value(void) { return g_err; }

static int fail(const char* msg, size_t n) {
  g_err = vy_str(vy_str_new(msg, n));
  return 1;
}
static int fail_with(VyValue v) {
  g_err = v;
  return 1;
}

/* -------------------------------------------- value method dispatch */
// base.method(a0, a1). out receives the method's return value. Returns 0 on
// success, 1 on failure (then vy_h_err_value() holds the message). Mirrors
// the value-method block in Interp::call_builtin exactly.
int vy_h_value_method(VyValue base, const char* name, VyValue a0, VyValue a1,
                      VyValue* out) {
  *out = vy_nil();

  if (vy_tagof(base) == VY_STRING) {
    VyStr* s = base.str;
    if (!strcmp(name, "upper")) { *out = vy_str(vy_str_upper(s)); return 0; }
    if (!strcmp(name, "lower")) { *out = vy_str(vy_str_lower(s)); return 0; }
    if (!strcmp(name, "trim"))  { *out = vy_str(vy_str_trim(s));  return 0; }
    if (!strcmp(name, "length")){ *out = vy_int(s->len); return 0; }
    if (!strcmp(name, "repeat")){ *out = vy_str(vy_str_repeat(s, a0.i)); return 0; }
    if (!strcmp(name, "chars")) { *out = vy_str_chars(s); return 0; }
    if (!strcmp(name, "bytes")) { *out = vy_str_bytes(s); return 0; }
    if (!strcmp(name, "split")) { *out = vy_str_split(s, a0.str); return 0; }
    if (!strcmp(name, "replace")){ *out = vy_str(vy_str_replace(s, a0.str, a1.str)); return 0; }
    if (!strcmp(name, "slice")) { *out = vy_str(vy_str_slice(s, a0.i, a1.i)); return 0; }
    if (!strcmp(name, "pad"))   { *out = vy_str(vy_str_pad(s, a0.i, a1.i, 1)); return 0; }
    if (!strcmp(name, "startswith")) {
      *out = vy_bool(vy_str_find(s, a0.str, 0) == 0); return 0;
    }
    if (!strcmp(name, "endswith")) {
      int64_t at = vy_str_find(s, a0.str, 0);
      *out = vy_bool(at >= 0 && (size_t)at + a0.str->len == s->len); return 0;
    }
    if (!strcmp(name, "contains")) {
      *out = vy_bool(vy_str_contains(s, a0.str)); return 0;
    }
    if (!strcmp(name, "indexof") || !strcmp(name, "find")) {
      *out = vy_int(vy_str_find(s, a0.str, 0)); return 0;
    }
  }

  if (vy_tagof(base) == VY_LIST) {
    if (!strcmp(name, "push") || !strcmp(name, "append")) {
      vy_list_push(base.list, a0); *out = base; return 0;
    }
    if (!strcmp(name, "pop"))     { *out = vy_list_pop(base.list); return 0; }
    if (!strcmp(name, "insert"))  { vy_list_insert(base.list, a0.i, a1); *out = base; return 0; }
    if (!strcmp(name, "remove"))  { *out = vy_bool(vy_list_remove(base.list, a0.i)); return 0; }
    if (!strcmp(name, "sort"))    { vy_list_sort(base.list); *out = base; return 0; }
    if (!strcmp(name, "reverse")) { vy_list_reverse(base.list); *out = base; return 0; }
    if (!strcmp(name, "contains")){ *out = vy_bool(vy_list_contains(base.list, a0)); return 0; }
    if (!strcmp(name, "indexof")) { *out = vy_int(vy_list_index(base.list, a0)); return 0; }
    if (!strcmp(name, "length"))  { *out = vy_int(base.list->len); return 0; }
    if (!strcmp(name, "extend")) {
      if (vy_tagof(a0) == VY_LIST)
        for (uint32_t i = 0; i < a0.list->len; i++)
          vy_list_push(base.list, a0.list->items[i]);
      *out = base;
      return 0;
    }
  }

  if (vy_tagof(base) == VY_MAP) {
    if (!strcmp(name, "get")) { *out = vy_map_get(base.map, a0); return 0; }
    if (!strcmp(name, "set")) { vy_map_set(base.map, a0, a1); *out = base; return 0; }
    if (!strcmp(name, "has")) { *out = vy_bool(vy_map_has(base.map, a0)); return 0; }
    if (!strcmp(name, "delete") || !strcmp(name, "remove")) {
      *out = vy_bool(vy_map_del(base.map, a0)); return 0;
    }
    if (!strcmp(name, "keys"))   { *out = vy_map_keys(base.map); return 0; }
    if (!strcmp(name, "values")) { *out = vy_map_values(base.map); return 0; }
    if (!strcmp(name, "items"))  { *out = vy_list(vy_map_pairs(base.map)); return 0; }
    if (!strcmp(name, "count") || !strcmp(name, "length")) {
      *out = vy_int(base.map->len); return 0;
    }
    if (!strcmp(name, "merge")) {
      if (vy_tagof(a0) == VY_MAP) {
        VyList* pr = vy_map_pairs(a0.map);
        for (uint32_t i = 0; i + 1 < pr->len; i += 2)
          vy_map_set(base.map, pr->items[i], pr->items[i + 1]);
      }
      *out = base;
      return 0;
    }
  }

  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s has no method '%s'", vy_type_name(base), name);
  return fail(tmp, strlen(tmp));
}

/* -------------------------------------------------------------- index */
// Mirrors Interp::index_get.
int vy_h_index(VyValue base, VyValue idx, VyValue* out) {
  *out = vy_nil();
  switch (vy_tagof(base)) {
    case VY_LIST:
      if (vy_tagof(idx) == VY_LIST || vy_tagof(idx) == VY_MAP)
        return fail("list index must be an Int", 25);
      *out = vy_list_get(base.list, vy_tagof(idx) == VY_FLOAT ? (int64_t)idx.f : idx.i);
      return 0;
    case VY_MAP:
      *out = vy_map_get(base.map, idx);
      return 0;
    case VY_STRING:
      if (vy_tagof(idx) == VY_STRING) {
        int64_t at = vy_str_find(base.str, idx.str, 0);
        *out = (at < 0) ? vy_nil() : vy_int(at);
        return 0;
      }
      *out = vy_str(vy_str_slice(base.str, idx.i, idx.i + 1));
      return 0;
    default:
      char tmp[128];
      snprintf(tmp, sizeof(tmp), "cannot index a %s", vy_type_name(base));
      return fail(tmp, strlen(tmp));
  }
}

/* -------------------------------------------------------------- member */
// Mirrors Interp::member_get.
int vy_h_member(VyValue base, const char* name, VyValue* out) {
  *out = vy_nil();
  switch (vy_tagof(base)) {
    case VY_MAP:
      *out = vy_map_get(base.map, vy_str(vy_str_new(name, strlen(name))));
      return 0;
    case VY_STRING: {
      if (!strcmp(name, "length")) { *out = vy_int(base.str->len); return 0; }
      if (!strcmp(name, "chars")) { *out = vy_str_chars(base.str); return 0; }
      if (!strcmp(name, "bytes")) { *out = vy_str_bytes(base.str); return 0; }
      if (!strcmp(name, "split")) { *out = vy_str_split(base.str, vy_str_new(",", 1)); return 0; }
      VyStr* r = NULL;
      if (!strcmp(name, "upper")) r = vy_str_upper(base.str);
      else if (!strcmp(name, "lower")) r = vy_str_lower(base.str);
      else if (!strcmp(name, "trim")) r = vy_str_trim(base.str);
      else {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "String has no field '%s'", name);
        return fail(tmp, strlen(tmp));
      }
      *out = vy_str(r);
      return 0;
    }
    case VY_LIST:
      if (!strcmp(name, "length")) { *out = vy_int(base.list->len); return 0; }
      { char tmp[160];
        snprintf(tmp, sizeof(tmp), "List has no field '%s'", name);
        return fail(tmp, strlen(tmp)); }
    case VY_NIL: { char tmp[160];
      snprintf(tmp, sizeof(tmp), "cannot read field '%s' of nil", name);
      return fail(tmp, strlen(tmp)); }
    default: { char tmp[160];
      snprintf(tmp, sizeof(tmp), "cannot read field '%s' from %s", name,
               vy_type_name(base));
      return fail(tmp, strlen(tmp)); }
  }
}

/* -------------------------------------------------------------- input */
VyValue vy_h_input(VyValue prompt) {
  if (vy_tagof(prompt) == VY_STRING)
    fwrite(prompt.str->bytes, 1, prompt.str->len, stdout);
  fflush(stdout);
  VyStr* line = vy_read_line();
  return line ? vy_str(line) : vy_nil();
}

/* -------------------------------------------------------------- env */
VyValue vy_h_env(VyValue key, int* threw) {
  *threw = 0;
  if (vy_tagof(key) != VY_STRING) { *threw = 1; fail("env() needs a String key", 20); return vy_h_err_value(); }
  const char* v = getenv(key.str->bytes);
  if (!v) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "environment variable '%s' is not set", key.str->bytes);
    *threw = 1;
    fail(tmp, strlen(tmp)); return vy_h_err_value();
  }
  return vy_str(vy_str_new(v, strlen(v)));
}

/* -------------------------------------------------------------- json */
VyValue vy_h_json_parse(VyValue s, int* threw) {
  *threw = 0;
  if (vy_tagof(s) != VY_STRING) { *threw = 1; fail("json.parse needs a String", 23); return vy_h_err_value(); }
  if (!vy_json_valid(s.str->bytes, s.str->len)) {
    size_t n = s.str->len < 80 ? s.str->len : 80;
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "invalid JSON: %.*s", (int)n, s.str->bytes);
    fail(tmp, strlen(tmp)); return vy_h_err_value();
  }
  return vy_json_parse(s.str->bytes, s.str->len);
}

/* ---------------------------------------------------------- number cast */
static VyValue to_number(VyValue v, int* threw, int is_int_cast) {
  *threw = 0;
  switch (vy_tagof(v)) {
    case VY_INT:
      return is_int_cast ? v : vy_float((double)v.i);
    case VY_FLOAT:
      return is_int_cast ? vy_int((int64_t)v.f) : v;
    case VY_BOOL:
      return is_int_cast ? vy_int(v.b) : vy_float(v.b);
    case VY_STRING: {
      const char* p = v.str->bytes;
      char* end = NULL;
      double d = strtod(p, &end);
      if (end && end != p && *end == '\0') {
        int has_dot = strchr(p, '.') || strchr(p, 'e') || strchr(p, 'E');
        return has_dot ? vy_float(d) : vy_int((int64_t)d);
      }
      break;
    }
    default: break;
  }
  char what[64];
  if (vy_tagof(v) == VY_STRING)
    snprintf(what, sizeof(what), "\"%s\"", v.str->bytes);
  else
    snprintf(what, sizeof(what), "%s", vy_type_name(v));
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "cannot read a number from %s", what);
  *threw = 1;
  fail(tmp, strlen(tmp)); return vy_h_err_value();
}
VyValue vy_h_to_int(VyValue v, int* threw)   { return to_number(v, threw, 1); }
VyValue vy_h_to_float(VyValue v, int* threw) { return to_number(v, threw, 0); }

/* -------------------------------------------------------------- len */
VyValue vy_h_len(VyValue v, int* threw) {
  *threw = 0;
  switch (vy_tagof(v)) {
    case VY_STRING: return vy_int(v.str->len);
    case VY_LIST:   return vy_int(v.list->len);
    case VY_MAP:    return vy_int(v.map->len);
    default: {
      char tmp[128];
      snprintf(tmp, sizeof(tmp), "len() needs a String, List or Map, got %s",
               vy_type_name(v));
      *threw = 1;
      fail(tmp, strlen(tmp)); return vy_h_err_value();
    }
  }
}

/* ------------------------------------------------------------------ http */
// http.<METHOD>(url, headers=..., json=..., params=..., body=..., content_type,
// timeout=...). out receives the response map {status, ok, body, headers,
// elapsed_ms} -- the same shape the interpreter's http_call builds, so a
// program reads response.status / response.body. Returns 0 on success,
// 1 on failure.
int vy_h_http(const char* method, VyValue url, VyValue headers, VyValue json_body,
              VyValue params, VyValue named_params, VyValue body, VyValue ctype,
              VyValue timeout, VyValue* out) {
  const char* url_s = (vy_tagof(url) == VY_STRING) ? url.str->bytes : "";
  if (!url_s[0]) return fail("http requires a url", 18);

  // Positional body only applies to requests that carry one; GET/DELETE/HEAD
  // use the second positional slot for the query map instead.
  int is_getlike = !strcmp(method, "GET") || !strcmp(method, "DELETE") ||
                   !strcmp(method, "HEAD");
  VyValue qparams = vy_tagof(named_params) != VY_NIL ? named_params : params;

  const char* body_json = NULL;
  VyStr* body_s = NULL;
  const char* ctype_s = NULL;
  VyValue extra[4];
  int ne = 0;
  if (!is_getlike) {
    if (vy_tagof(json_body) != VY_NIL) {
      body_s = vy_json_stringify(json_body);
      body_json = body_s->bytes;
      ctype_s = "application/json";
      extra[ne++] = vy_str(body_s);
    } else if (vy_tagof(body) != VY_NIL) {
      if (vy_tagof(body) == VY_STRING) body_json = body.str->bytes;
      else {
        body_s = vy_json_stringify(body);
        body_json = body_s->bytes;
        ctype_s = "application/json";
        extra[ne++] = vy_str(body_s);
      }
    }
    if (vy_tagof(ctype) == VY_STRING && ctype.str->len) ctype_s = ctype.str->bytes;
  }
  const char* par_json = NULL;
  VyStr* par_s = NULL;
  if (vy_tagof(qparams) != VY_NIL) {
    par_s = vy_json_stringify(qparams);
    par_json = par_s->bytes;
    extra[ne++] = vy_str(par_s);
  }
  const char* hdr_json = NULL;
  VyStr* hdr_s = NULL;
  if (vy_tagof(headers) != VY_NIL) {
    hdr_s = vy_json_stringify(headers);
    hdr_json = hdr_s->bytes;
    extra[ne++] = vy_str(hdr_s);
  }
  double tmo = 30.0;
  if (vy_tagof(timeout) == VY_FLOAT) tmo = timeout.f;
  else if (vy_tagof(timeout) == VY_INT) tmo = (double)timeout.i;

  // The request below allocates (headers, buffers); a collection triggered by
  // those must still see the serialised payloads, whose only heap references
  // are the char* views above. Hold the heap quiet and keep the owners in
  // `extra` for the explicit collection points.
  vy_gc_begin_mutation();
  VyHttpResponse* r =
      vy_http_request(method, url_s, body_json, ctype_s, hdr_json,
                      par_json, tmo);
  vy_gc_end_mutation();
  (void)ne;
  (void)extra;
  if (!r) return fail("http request could not be allocated", 36);
  if (r->error) return fail_with(vy_str(r->error));

  VyMap* m = vy_map_new();
  vy_map_set(m, vy_str(vy_str_new("status", 6)), vy_int(r->status));
  vy_map_set(m, vy_str(vy_str_new("ok", 2)),
             vy_bool(r->status >= 200 && r->status < 400));
  vy_map_set(m, vy_str(vy_str_new("body", 4)), vy_str(r->body));
  vy_map_set(m, vy_str(vy_str_new("headers", 7)), r->headers);
  vy_map_set(m, vy_str(vy_str_new("elapsed_ms", 10)), vy_float(r->elapsed_ms));
  *out = vy_map(m);
  return 0;
}

/* --------------------------------------------------------------- closures */
// A generated native function has C's exact calling convention, so wrapping it
// in a VyFunc only needs to record the arity; the call goes through a cast of
// the stored pointer. (The interpreter's closures carry a different, AST-based
// payload in `upvals`; the native backend never produces those.)
VyFunc* vy_h_make_func(int arity, VyFnPtr fn) {
  VyFunc* f = (VyFunc*)calloc(1, sizeof(VyFunc));
  if (!f) return NULL;
  f->call = fn;
  f->arity = arity;
  f->variadic = 0;
  f->name = vy_str_new("closure", 7);
  f->upvals = NULL;
  return f;
}
VyValue vy_h_make_func_value(const char* name, int arity, VyFnPtr fn) {
  VyFunc* f = vy_h_make_func(arity, fn);
  if (!f) return vy_nil();
  if (name) f->name = vy_str_new(name, strlen(name));
  return vy_func(f);
}

VyValue vy_h_call(VyValue callee, VyValue args[], int n, VyValue** slot, int* argc) {
  if (vy_tagof(callee) != VY_FUNC || !callee.fn || !callee.fn->call) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "not callable");
    fail(tmp, strlen(tmp));
    return vy_nil();
  }
  VyFnPtr fn = callee.fn->call;
  if (slot) *slot = args;
  if (argc) *argc = n;
  return fn(args, n);
}

/* ------------------------------------------------------------------ misc */
double vy_h_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double as_double(VyValue v, int* threw) {
  VyValue n = vy_h_to_float(v, threw);
  return vy_tagof(n) == VY_FLOAT ? n.f : (double)n.i;
}

/* op: 'a' abs, 's' sqrt, 'f' floor, 'c' ceil, 'r' round */
VyValue vy_h_num1(VyValue v, int* threw, char op) {
  *threw = 0;
  if (op == 'a') {
    switch (vy_tagof(v)) {
      case VY_INT:   return vy_int(v.i < 0 ? -v.i : v.i);
      case VY_FLOAT: return vy_float(fabs(v.f));
      case VY_BOOL:  return vy_int(v.b);
      default: break;
    }
    // abs() of a numeric string still works, matching the interpreter.
    { VyValue n = vy_h_to_float(v, threw);
      if (*threw) return vy_nil();
      return vy_tagof(n) == VY_FLOAT ? vy_float(fabs(n.f)) : vy_int(n.i < 0 ? -n.i : n.i); }
  }
  double d = as_double(v, threw);
  if (*threw) return vy_nil();
  switch (op) {
    case 's': return vy_float(sqrt(d));
    case 'f': return vy_int((int64_t)floor(d));
    case 'c': return vy_int((int64_t)ceil(d));
    case 'r': return vy_int((int64_t)llround(d));
    default:  return vy_nil();
  }
}

/* --------------------------------------------------------------- gc */
VyValue vy_h_gc(VyValue mode) {
  const char* what = (vy_tagof(mode) == VY_STRING) ? vy_str_data(mode.str) : "";
  if (!strcmp(what, "stats")) {
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "live=%llu heap=%llu allocs=%llu",
             (unsigned long long)vy_gc_live_bytes(),
             (unsigned long long)vy_gc_heap_bytes(),
             (unsigned long long)vy_heap_alloc_count());
    return vy_str(vy_str_new(tmp, strlen(tmp)));
  }
  if (!strcmp(what, "reset")) { vy_heap_reset_stats(); return vy_nil(); }
  vy_gc_collect();
  return vy_nil();
}

/* -------------------------------------------------------------- pad */
VyValue vy_h_pad(VyValue s, VyValue width, VyValue fill, VyValue side, int* threw) {
  *threw = 0;
  if (vy_tagof(s) != VY_STRING) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "pad() needs a String first argument, got %s",
             vy_type_name(s));
    *threw = 1;
    fail(tmp, strlen(tmp)); return vy_h_err_value();
  }
  if (vy_tagof(width) != VY_INT && vy_tagof(width) != VY_FLOAT) {
    *threw = 1;
    const char* m = "pad() needs a number for the width";
    fail(m, strlen(m)); return vy_h_err_value();
  }
  int64_t w = vy_tagof(width) == VY_INT ? width.i : (int64_t)width.f;
  int64_t f = vy_tagof(fill) == VY_INT ? fill.i : 0;
  // The optional fourth argument names the side; the default pads on the left,
  // exactly like the s.pad(width) method form. Interp::call_builtin renders it
  // with str_arg and compares to "right", which matches this string test for
  // every other value too (no non-"right" render can equal "right").
  int left = 1;
  if (vy_tagof(side) == VY_STRING && side.str->len == 5 &&
      memcmp(side.str->bytes, "right", 5) == 0)
    left = 0;
  return vy_str(vy_str_pad(s.str, w, f, left));
}

VyValue vy_h_sum(VyValue list, int* threw) {
  *threw = 0;
  if (vy_tagof(list) != VY_LIST) return vy_int(0);
  int all_int = 1;
  double d = 0;
  int64_t i = 0;
  for (uint32_t k = 0; k < list.list->len; k++) {
    VyValue n = vy_h_to_float(list.list->items[k], threw);
    if (*threw) return vy_nil();
    double dn = vy_tagof(n) == VY_FLOAT ? n.f : (double)n.i;
    if (vy_tagof(list.list->items[k]) == VY_FLOAT) all_int = 0;
    else if (vy_tagof(list.list->items[k]) != VY_INT) all_int = 0;
    d += dn;
    i += (int64_t)dn;
  }
  return all_int ? vy_int(i) : vy_float(d);
}

VyValue vy_h_join(VyValue sep, VyValue list) {
  if (vy_tagof(list) != VY_LIST) return vy_str(vy_str_new("", 0));
  VyStr* acc = vy_str_new("", 0);
  for (uint32_t i = 0; i < list.list->len; i++) {
    if (i) acc = vy_str_concat(acc, sep.str);
    acc = vy_str_concat(acc, vy_render(list.list->items[i]));
  }
  return vy_str(acc);
}

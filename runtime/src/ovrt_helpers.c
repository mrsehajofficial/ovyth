// Ovyth runtime :: src/ovrt_helpers.c
//
// Thin C layer the native codegen emits calls into. It concentrates the
// runtime plumbing that would otherwise bloat every generated expression:
// value-method dispatch and the http.* namespace. Keeping it here means the
// emitter stays small and stays one-to-one with the interpreter's
// `interp_builtins.cpp`, so the two backends cannot drift apart.
#include "../include/ovrt_helpers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ state */
static OvValue g_err;

OvValue ov_h_err_value(void) { return g_err; }

static int fail(const char* msg, size_t n) {
  g_err = ov_str(ov_str_new(msg, n));
  return 1;
}
static int fail_with(OvValue v) {
  g_err = v;
  return 1;
}

/* -------------------------------------------- value method dispatch */
// base.method(a0, a1). out receives the method's return value. Returns 0 on
// success, 1 on failure (then ov_h_err_value() holds the message). Mirrors
// the value-method block in Interp::call_builtin exactly.
int ov_h_value_method(OvValue base, const char* name, OvValue a0, OvValue a1,
                      OvValue* out) {
  *out = ov_nil();

  if (ov_tagof(base) == OV_STRING) {
    OvStr* s = base.str;
    if (!strcmp(name, "upper")) { *out = ov_str(ov_str_upper(s)); return 0; }
    if (!strcmp(name, "lower")) { *out = ov_str(ov_str_lower(s)); return 0; }
    if (!strcmp(name, "trim"))  { *out = ov_str(ov_str_trim(s));  return 0; }
    if (!strcmp(name, "length")){ *out = ov_int(s->len); return 0; }
    if (!strcmp(name, "repeat")){ *out = ov_str(ov_str_repeat(s, a0.i)); return 0; }
    if (!strcmp(name, "chars")) { *out = ov_str_chars(s); return 0; }
    if (!strcmp(name, "bytes")) { *out = ov_str_bytes(s); return 0; }
    if (!strcmp(name, "split")) { *out = ov_str_split(s, a0.str); return 0; }
    if (!strcmp(name, "replace")){ *out = ov_str(ov_str_replace(s, a0.str, a1.str)); return 0; }
    if (!strcmp(name, "slice")) { *out = ov_str(ov_str_slice(s, a0.i, a1.i)); return 0; }
    if (!strcmp(name, "pad"))   { *out = ov_str(ov_str_pad(s, a0.i, a1.i, 1)); return 0; }
    if (!strcmp(name, "startswith")) {
      *out = ov_bool(ov_str_find(s, a0.str, 0) == 0); return 0;
    }
    if (!strcmp(name, "endswith")) {
      int64_t at = ov_str_find(s, a0.str, 0);
      *out = ov_bool(at >= 0 && (size_t)at + a0.str->len == s->len); return 0;
    }
    if (!strcmp(name, "contains")) {
      *out = ov_bool(ov_str_contains(s, a0.str)); return 0;
    }
    if (!strcmp(name, "indexof") || !strcmp(name, "find")) {
      *out = ov_int(ov_str_find(s, a0.str, 0)); return 0;
    }
  }

  if (ov_tagof(base) == OV_LIST) {
    if (!strcmp(name, "push") || !strcmp(name, "append")) {
      ov_list_push(base.list, a0); *out = base; return 0;
    }
    if (!strcmp(name, "pop"))     { *out = ov_list_pop(base.list); return 0; }
    if (!strcmp(name, "insert"))  { ov_list_insert(base.list, a0.i, a1); *out = base; return 0; }
    if (!strcmp(name, "remove"))  { *out = ov_bool(ov_list_remove(base.list, a0.i)); return 0; }
    if (!strcmp(name, "sort"))    { ov_list_sort(base.list); *out = base; return 0; }
    if (!strcmp(name, "reverse")) { ov_list_reverse(base.list); *out = base; return 0; }
    if (!strcmp(name, "contains")){ *out = ov_bool(ov_list_contains(base.list, a0)); return 0; }
    if (!strcmp(name, "indexof")) { *out = ov_int(ov_list_index(base.list, a0)); return 0; }
    if (!strcmp(name, "length"))  { *out = ov_int(base.list->len); return 0; }
    if (!strcmp(name, "extend")) {
      if (ov_tagof(a0) == OV_LIST)
        for (uint32_t i = 0; i < a0.list->len; i++)
          ov_list_push(base.list, a0.list->items[i]);
      *out = base;
      return 0;
    }
  }

  if (ov_tagof(base) == OV_I64A) {
    if (!strcmp(name, "push") || !strcmp(name, "append")) {
      ov_i64a_push(base.i64a, a0.i); *out = base; return 0;
    }
    if (!strcmp(name, "pop"))     { *out = ov_int(ov_i64a_pop(base.i64a)); return 0; }
    if (!strcmp(name, "length"))  { *out = ov_int(base.i64a->len); return 0; }
    if (!strcmp(name, "get"))     { *out = ov_int(ov_i64a_get(base.i64a, a0.i)); return 0; }
    if (!strcmp(name, "set"))     { ov_i64a_set(base.i64a, a0.i, a1.i); *out = base; return 0; }
    if (!strcmp(name, "sum")) {
      int64_t sum = 0;
      for (uint32_t i = 0; i < base.i64a->len; i++) sum += base.i64a->data[i];
      *out = ov_int(sum); return 0;
    }
  }

  if (ov_tagof(base) == OV_F64A) {
    if (!strcmp(name, "push") || !strcmp(name, "append")) {
      ov_f64a_push(base.f64a, a0.f); *out = base; return 0;
    }
    if (!strcmp(name, "pop"))     { *out = ov_float(ov_f64a_pop(base.f64a)); return 0; }
    if (!strcmp(name, "length"))  { *out = ov_int(base.f64a->len); return 0; }
    if (!strcmp(name, "get"))     { *out = ov_float(ov_f64a_get(base.f64a, a0.i)); return 0; }
    if (!strcmp(name, "set"))     { ov_f64a_set(base.f64a, a0.i, a1.f); *out = base; return 0; }
    if (!strcmp(name, "sum")) {
      double sum = 0.0;
      for (uint32_t i = 0; i < base.f64a->len; i++) sum += base.f64a->data[i];
      *out = ov_float(sum); return 0;
    }
  }

  if (ov_tagof(base) == OV_STRA) {
    if (!strcmp(name, "push") || !strcmp(name, "append")) {
      ov_stra_push(base.stra, a0.str); *out = base; return 0;
    }
    if (!strcmp(name, "pop"))     { *out = ov_str(ov_stra_pop(base.stra)); return 0; }
    if (!strcmp(name, "length"))  { *out = ov_int(base.stra->len); return 0; }
    if (!strcmp(name, "get"))     { *out = ov_str(ov_stra_get(base.stra, a0.i)); return 0; }
    if (!strcmp(name, "set"))     { ov_stra_set(base.stra, a0.i, a1.str); *out = base; return 0; }
  }

  if (ov_tagof(base) == OV_MAP) {
    if (!strcmp(name, "get")) { *out = ov_map_get(base.map, a0); return 0; }
    if (!strcmp(name, "set")) { ov_map_set(base.map, a0, a1); *out = base; return 0; }
    if (!strcmp(name, "has")) { *out = ov_bool(ov_map_has(base.map, a0)); return 0; }
    if (!strcmp(name, "delete") || !strcmp(name, "remove")) {
      *out = ov_bool(ov_map_del(base.map, a0)); return 0;
    }
    if (!strcmp(name, "keys"))   { *out = ov_map_keys(base.map); return 0; }
    if (!strcmp(name, "values")) { *out = ov_map_values(base.map); return 0; }
    if (!strcmp(name, "items"))  { *out = ov_list(ov_map_pairs(base.map)); return 0; }
    if (!strcmp(name, "count") || !strcmp(name, "length")) {
      *out = ov_int(base.map->len); return 0;
    }
    if (!strcmp(name, "merge")) {
      if (ov_tagof(a0) == OV_MAP) {
        OvList* pr = ov_map_pairs(a0.map);
        for (uint32_t i = 0; i + 1 < pr->len; i += 2)
          ov_map_set(base.map, pr->items[i], pr->items[i + 1]);
      }
      *out = base;
      return 0;
    }
  }

  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s has no method '%s'", ov_type_name(base), name);
  return fail(tmp, strlen(tmp));
}

/* -------------------------------------------------------------- index */
// Mirrors Interp::index_get.
int ov_h_index(OvValue base, OvValue idx, OvValue* out) {
  *out = ov_nil();
  switch (ov_tagof(base)) {
    case OV_LIST:
      if (ov_tagof(idx) == OV_LIST || ov_tagof(idx) == OV_MAP)
        return fail("list index must be an Int", 25);
      *out = ov_list_get(base.list, ov_tagof(idx) == OV_FLOAT ? (int64_t)idx.f : idx.i);
      return 0;
    case OV_I64A:
      if (ov_tagof(idx) == OV_LIST || ov_tagof(idx) == OV_MAP)
        return fail("array index must be an Int", 27);
      *out = ov_int(ov_i64a_get(base.i64a, ov_tagof(idx) == OV_FLOAT ? (int64_t)idx.f : idx.i));
      return 0;
    case OV_F64A:
      if (ov_tagof(idx) == OV_LIST || ov_tagof(idx) == OV_MAP)
        return fail("array index must be an Int", 27);
      *out = ov_float(ov_f64a_get(base.f64a, ov_tagof(idx) == OV_FLOAT ? (int64_t)idx.f : idx.i));
      return 0;
    case OV_STRA:
      if (ov_tagof(idx) == OV_LIST || ov_tagof(idx) == OV_MAP)
        return fail("array index must be an Int", 27);
      *out = ov_str(ov_stra_get(base.stra, ov_tagof(idx) == OV_FLOAT ? (int64_t)idx.f : idx.i));
      return 0;
    case OV_MAP:
      *out = ov_map_get(base.map, idx);
      return 0;
    case OV_STRING:
      if (ov_tagof(idx) == OV_STRING) {
        int64_t at = ov_str_find(base.str, idx.str, 0);
        *out = (at < 0) ? ov_nil() : ov_int(at);
        return 0;
      }
      *out = ov_str(ov_str_slice(base.str, idx.i, idx.i + 1));
      return 0;
    default:
      char tmp[128];
      snprintf(tmp, sizeof(tmp), "cannot index a %s", ov_type_name(base));
      return fail(tmp, strlen(tmp));
  }
}

/* -------------------------------------------------------------- member */
// Mirrors Interp::member_get.
int ov_h_member(OvValue base, const char* name, OvValue* out) {
  *out = ov_nil();
  switch (ov_tagof(base)) {
    case OV_MAP:
      *out = ov_map_get(base.map, ov_str(ov_str_new(name, strlen(name))));
      return 0;
    case OV_STRING: {
      if (!strcmp(name, "length")) { *out = ov_int(base.str->len); return 0; }
      if (!strcmp(name, "chars")) { *out = ov_str_chars(base.str); return 0; }
      if (!strcmp(name, "bytes")) { *out = ov_str_bytes(base.str); return 0; }
      if (!strcmp(name, "split")) { *out = ov_str_split(base.str, ov_str_new(",", 1)); return 0; }
      OvStr* r = NULL;
      if (!strcmp(name, "upper")) r = ov_str_upper(base.str);
      else if (!strcmp(name, "lower")) r = ov_str_lower(base.str);
      else if (!strcmp(name, "trim")) r = ov_str_trim(base.str);
      else {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "String has no field '%s'", name);
        return fail(tmp, strlen(tmp));
      }
      *out = ov_str(r);
      return 0;
    }
    case OV_LIST:
      if (!strcmp(name, "length")) { *out = ov_int(base.list->len); return 0; }
      { char tmp[160];
        snprintf(tmp, sizeof(tmp), "List has no field '%s'", name);
        return fail(tmp, strlen(tmp)); }
    case OV_I64A:
    case OV_F64A:
    case OV_STRA:
      if (!strcmp(name, "length")) { *out = ov_int(base.i64a->len); return 0; }
      { char tmp[160];
        snprintf(tmp, sizeof(tmp), "Array has no field '%s'", name);
        return fail(tmp, strlen(tmp)); }
    case OV_NIL: { char tmp[160];
      snprintf(tmp, sizeof(tmp), "cannot read field '%s' of nil", name);
      return fail(tmp, strlen(tmp)); }
    default: { char tmp[160];
      snprintf(tmp, sizeof(tmp), "cannot read field '%s' from %s", name,
               ov_type_name(base));
      return fail(tmp, strlen(tmp)); }
  }
}

/* -------------------------------------------------------------- input */
OvValue ov_h_input(OvValue prompt) {
  if (ov_tagof(prompt) == OV_STRING)
    fwrite(prompt.str->bytes, 1, prompt.str->len, stdout);
  fflush(stdout);
  OvStr* line = ov_read_line();
  return line ? ov_str(line) : ov_nil();
}

/* -------------------------------------------------------------- env */
OvValue ov_h_env(OvValue key, int* threw) {
  *threw = 0;
  if (ov_tagof(key) != OV_STRING) { *threw = 1; fail("env() needs a String key", 20); return ov_h_err_value(); }
  const char* v = getenv(key.str->bytes);
  if (!v) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "environment variable '%s' is not set", key.str->bytes);
    *threw = 1;
    fail(tmp, strlen(tmp)); return ov_h_err_value();
  }
  return ov_str(ov_str_new(v, strlen(v)));
}

/* -------------------------------------------------------------- json */
OvValue ov_h_json_parse(OvValue s, int* threw) {
  *threw = 0;
  if (ov_tagof(s) != OV_STRING) { *threw = 1; fail("json.parse needs a String", 23); return ov_h_err_value(); }
  if (!ov_json_valid(s.str->bytes, s.str->len)) {
    size_t n = s.str->len < 80 ? s.str->len : 80;
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "invalid JSON: %.*s", (int)n, s.str->bytes);
    fail(tmp, strlen(tmp)); return ov_h_err_value();
  }
  /* Use fast parser that reuses buffers */
  return ov_json_parse_fast(s.str->bytes, s.str->len, NULL);
}

/* ---------------------------------------------------------- number cast */
static OvValue to_number(OvValue v, int* threw, int is_int_cast) {
  *threw = 0;
  switch (ov_tagof(v)) {
    case OV_INT:
      return is_int_cast ? v : ov_float((double)v.i);
    case OV_FLOAT:
      return is_int_cast ? ov_int((int64_t)v.f) : v;
    case OV_BOOL:
      return is_int_cast ? ov_int(v.b) : ov_float(v.b);
    case OV_STRING: {
      const char* p = v.str->bytes;
      char* end = NULL;
      double d = strtod(p, &end);
      if (end && end != p && *end == '\0') {
        int has_dot = strchr(p, '.') || strchr(p, 'e') || strchr(p, 'E');
        return has_dot ? ov_float(d) : ov_int((int64_t)d);
      }
      break;
    }
    default: break;
  }
  char what[64];
  if (ov_tagof(v) == OV_STRING)
    snprintf(what, sizeof(what), "\"%s\"", v.str->bytes);
  else
    snprintf(what, sizeof(what), "%s", ov_type_name(v));
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "cannot read a number from %s", what);
  *threw = 1;
  fail(tmp, strlen(tmp)); return ov_h_err_value();
}
OvValue ov_h_to_int(OvValue v, int* threw)   { return to_number(v, threw, 1); }
OvValue ov_h_to_float(OvValue v, int* threw) { return to_number(v, threw, 0); }

/* -------------------------------------------------------------- len */
OvValue ov_h_len(OvValue v, int* threw) {
  *threw = 0;
  switch (ov_tagof(v)) {
    case OV_STRING: return ov_int(v.str->len);
    case OV_LIST:   return ov_int(v.list->len);
    case OV_MAP:    return ov_int(v.map->len);
    case OV_I64A:   return ov_int(v.i64a->len);
    case OV_F64A:   return ov_int(v.f64a->len);
    case OV_STRA:   return ov_int(v.stra->len);
    default: {
      char tmp[128];
      snprintf(tmp, sizeof(tmp), "len() needs a String, List or Map, got %s",
               ov_type_name(v));
      *threw = 1;
      fail(tmp, strlen(tmp)); return ov_h_err_value();
    }
  }
}

/* ------------------------------------------------------------------ http */
// http.<METHOD>(url, headers=..., json=..., params=..., body=..., content_type,
// timeout=...). out receives the response map {status, ok, body, headers,
// elapsed_ms} -- the same shape the interpreter's http_call builds, so a
// program reads response.status / response.body. Returns 0 on success,
// 1 on failure.
int ov_h_http(const char* method, OvValue url, OvValue headers, OvValue json_body,
              OvValue params, OvValue named_params, OvValue body, OvValue ctype,
              OvValue timeout, OvValue* out) {
  const char* url_s = (ov_tagof(url) == OV_STRING) ? url.str->bytes : "";
  if (!url_s[0]) return fail("http requires a url", 18);

  // Positional body only applies to requests that carry one; GET/DELETE/HEAD
  // use the second positional slot for the query map instead.
  int is_getlike = !strcmp(method, "GET") || !strcmp(method, "DELETE") ||
                   !strcmp(method, "HEAD");
  OvValue qparams = ov_tagof(named_params) != OV_NIL ? named_params : params;

  const char* body_json = NULL;
  OvStr* body_s = NULL;
  const char* ctype_s = NULL;
  OvValue extra[4];
  int ne = 0;
  if (!is_getlike) {
    if (ov_tagof(json_body) != OV_NIL) {
      body_s = ov_json_stringify(json_body);
      body_json = body_s->bytes;
      ctype_s = "application/json";
      extra[ne++] = ov_str(body_s);
    } else if (ov_tagof(body) != OV_NIL) {
      if (ov_tagof(body) == OV_STRING) body_json = body.str->bytes;
      else {
        body_s = ov_json_stringify(body);
        body_json = body_s->bytes;
        ctype_s = "application/json";
        extra[ne++] = ov_str(body_s);
      }
    }
    if (ov_tagof(ctype) == OV_STRING && ctype.str->len) ctype_s = ctype.str->bytes;
  }
  const char* par_json = NULL;
  OvStr* par_s = NULL;
  if (ov_tagof(qparams) != OV_NIL) {
    par_s = ov_json_stringify(qparams);
    par_json = par_s->bytes;
    extra[ne++] = ov_str(par_s);
  }
  const char* hdr_json = NULL;
  OvStr* hdr_s = NULL;
  if (ov_tagof(headers) != OV_NIL) {
    hdr_s = ov_json_stringify(headers);
    hdr_json = hdr_s->bytes;
    extra[ne++] = ov_str(hdr_s);
  }
  double tmo = 30.0;
  if (ov_tagof(timeout) == OV_FLOAT) tmo = timeout.f;
  else if (ov_tagof(timeout) == OV_INT) tmo = (double)timeout.i;

  // The request below allocates (headers, buffers); a collection triggered by
  // those must still see the serialised payloads, whose only heap references
  // are the char* views above. Hold the heap quiet and keep the owners in
  // `extra` for the explicit collection points.
  ov_gc_begin_mutation();
  OvHttpResponse* r =
      ov_http_request(method, url_s, body_json, ctype_s, hdr_json,
                      par_json, tmo);
  ov_gc_end_mutation();
  (void)ne;
  (void)extra;
  if (!r) return fail("http request could not be allocated", 36);
  if (r->error) return fail_with(ov_str(r->error));

  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str(ov_str_new("status", 6)), ov_int(r->status));
  ov_map_set(m, ov_str(ov_str_new("ok", 2)),
             ov_bool(r->status >= 200 && r->status < 400));
  ov_map_set(m, ov_str(ov_str_new("body", 4)), ov_str(r->body));
  ov_map_set(m, ov_str(ov_str_new("headers", 7)), r->headers);
  ov_map_set(m, ov_str(ov_str_new("elapsed_ms", 10)), ov_float(r->elapsed_ms));
  *out = ov_map(m);
  return 0;
}

/* --------------------------------------------------------------- closures */
// A generated native function has C's exact calling convention, so wrapping it
// in a OvFunc only needs to record the arity; the call goes through a cast of
// the stored pointer. (The interpreter's closures carry a different, AST-based
// payload in `upvals`; the native backend never produces those.)
OvFunc* ov_h_make_closure(int arity, OvFnPtr fn, void* upvals, int num_upvals) {
  OvFunc* f = (OvFunc*)calloc(1, sizeof(OvFunc));
  if (!f) return NULL;
  f->call = fn;
  f->arity = arity;
  f->variadic = 0;
  f->name = ov_str_new("closure", 7);
  f->upvals = upvals;
  if (upvals && num_upvals > 0) {
    OvValue* slots = (OvValue*)upvals;
    for (int i = 0; i < num_upvals; i++) {
      ov_gc_register_root(&slots[i]);
    }
  }
  return f;
}

OvFunc* ov_h_make_func(int arity, OvFnPtr fn) {
  return ov_h_make_closure(arity, fn, NULL, 0);
}

OvValue ov_h_make_func_value(const char* name, int arity, OvFnPtr fn) {
  OvFunc* f = ov_h_make_func(arity, fn);
  if (!f) return ov_nil();
  if (name) f->name = ov_str_new(name, strlen(name));
  return ov_func(f);
}

OvValue ov_h_call(OvValue callee, OvValue args[], int n, OvValue** slot, int* argc) {
  if (ov_tagof(callee) != OV_FUNC || !callee.fn || !callee.fn->call) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "not callable");
    fail(tmp, strlen(tmp));
    return ov_nil();
  }
  OvFnPtr fn = callee.fn->call;
  if (slot) *slot = args;
  if (argc) *argc = n;
  return fn(callee.fn, args, n);
}

/* ------------------------------------------------------------------ misc */
double ov_h_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double as_double(OvValue v, int* threw) {
  OvValue n = ov_h_to_float(v, threw);
  return ov_tagof(n) == OV_FLOAT ? n.f : (double)n.i;
}

/* op: 'a' abs, 's' sqrt, 'f' floor, 'c' ceil, 'r' round */
OvValue ov_h_num1(OvValue v, int* threw, char op) {
  *threw = 0;
  if (op == 'a') {
    switch (ov_tagof(v)) {
      case OV_INT:   return ov_int(v.i < 0 ? -v.i : v.i);
      case OV_FLOAT: return ov_float(fabs(v.f));
      case OV_BOOL:  return ov_int(v.b);
      default: break;
    }
    // abs() of a numeric string still works, matching the interpreter.
    { OvValue n = ov_h_to_float(v, threw);
      if (*threw) return ov_nil();
      return ov_tagof(n) == OV_FLOAT ? ov_float(fabs(n.f)) : ov_int(n.i < 0 ? -n.i : n.i); }
  }
  double d = as_double(v, threw);
  if (*threw) return ov_nil();
  switch (op) {
    case 's': return ov_float(sqrt(d));
    case 'f': return ov_int((int64_t)floor(d));
    case 'c': return ov_int((int64_t)ceil(d));
    case 'r': return ov_int((int64_t)llround(d));
    default:  return ov_nil();
  }
}

/* --------------------------------------------------------------- gc */
OvValue ov_h_gc(OvValue mode) {
  const char* what = (ov_tagof(mode) == OV_STRING) ? ov_str_data(mode.str) : "";
  if (!strcmp(what, "stats")) {
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "live=%llu heap=%llu allocs=%llu",
             (unsigned long long)ov_gc_live_bytes(),
             (unsigned long long)ov_gc_heap_bytes(),
             (unsigned long long)ov_heap_alloc_count());
    return ov_str(ov_str_new(tmp, strlen(tmp)));
  }
  if (!strcmp(what, "reset")) { ov_heap_reset_stats(); return ov_nil(); }
  if (!strcmp(what, "disable")) { ov_gc_disable(1); return ov_nil(); }
  if (!strcmp(what, "enable"))  { ov_gc_disable(0); return ov_nil(); }
  ov_gc_collect();
  return ov_nil();
}

/* -------------------------------------------------------------- pad */
OvValue ov_h_pad(OvValue s, OvValue width, OvValue fill, OvValue side, int* threw) {
  *threw = 0;
  if (ov_tagof(s) != OV_STRING) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "pad() needs a String first argument, got %s",
             ov_type_name(s));
    *threw = 1;
    fail(tmp, strlen(tmp)); return ov_h_err_value();
  }
  if (ov_tagof(width) != OV_INT && ov_tagof(width) != OV_FLOAT) {
    *threw = 1;
    const char* m = "pad() needs a number for the width";
    fail(m, strlen(m)); return ov_h_err_value();
  }
  int64_t w = ov_tagof(width) == OV_INT ? width.i : (int64_t)width.f;
  int64_t f = ov_tagof(fill) == OV_INT ? fill.i : 0;
  // The optional fourth argument names the side; the default pads on the left,
  // exactly like the s.pad(width) method form. Interp::call_builtin renders it
  // with str_arg and compares to "right", which matches this string test for
  // every other value too (no non-"right" render can equal "right").
  int left = 1;
  if (ov_tagof(side) == OV_STRING && side.str->len == 5 &&
      memcmp(side.str->bytes, "right", 5) == 0)
    left = 0;
  return ov_str(ov_str_pad(s.str, w, f, left));
}

OvValue ov_h_sum(OvValue list, int* threw) {
  *threw = 0;
  if (ov_tagof(list) != OV_LIST) return ov_int(0);
  int all_int = 1;
  double d = 0;
  int64_t i = 0;
  for (uint32_t k = 0; k < list.list->len; k++) {
    OvValue n = ov_h_to_float(list.list->items[k], threw);
    if (*threw) return ov_nil();
    double dn = ov_tagof(n) == OV_FLOAT ? n.f : (double)n.i;
    if (ov_tagof(list.list->items[k]) == OV_FLOAT) all_int = 0;
    else if (ov_tagof(list.list->items[k]) != OV_INT) all_int = 0;
    d += dn;
    i += (int64_t)dn;
  }
  return all_int ? ov_int(i) : ov_float(d);
}

OvValue ov_h_join(OvValue sep, OvValue list) {
  if (ov_tagof(list) != OV_LIST) return ov_str(ov_str_new("", 0));

  /* Use string builder for O(n) join instead of repeated concatenation */
  OvStrBuilder* sb = ov_sb_new();
  int first = 1;
  for (uint32_t i = 0; i < list.list->len; i++) {
    if (!first) ov_sb_append(sb, sep.str->bytes, sep.str->len);
    OvStr* s = ov_render(list.list->items[i]);
    ov_sb_append(sb, s->bytes, s->len);
    first = 0;
  }
  OvStr* result = ov_sb_finish(sb);
  return ov_str(result);
}


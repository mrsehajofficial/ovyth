// Ovyth :: runtime/src/http_builtin.c
//
// The http.* builtin, in its own translation unit.
//
// It lives apart from ovrt_helpers.c on purpose. That object is linked
// into every program -- len(), sum(), join() and the rest are used
// everywhere -- and while it contained the http caller, every binary
// dragged in libcurl, libssl, libcrypto and libnghttp3 whether it made
// a request or not. An empty program linked with those took 15-18ms to
// start against 5ms without: ~12ms of TLS initialisation paid by
// arithmetic. Split out, the linker only pulls this member (and so the
// crypto stack) when the generated C actually calls ov_h_http, and
// --as-needed then drops the libraries for everything else.
#include <string.h>

#include "ovrt_helpers.h"

#include "ovrt.h"

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
  if (!url_s[0]) return ov_h_fail("http requires a url", 18);

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
  if (!r) return ov_h_fail("http request could not be allocated", 36);
  if (r->error) return ov_h_fail_with(ov_str(r->error));

  // The response map is built in a C local, and every key below allocates.
  // Bracket the build with the mutation guard so no collection runs while the
  // map is reachable from nothing: a collection there would free it out from
  // under these sets, and the caller would be handed a map built on freed
  // memory. The map's only reference from there on is *out, so the guard is
  // released before returning. Same rule ov_json_parse applies to a document.
  ov_gc_begin_mutation();
  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str(ov_str_new("status", 6)), ov_int(r->status));
  ov_map_set(m, ov_str(ov_str_new("ok", 2)),
             ov_bool(r->status >= 200 && r->status < 400));
  ov_map_set(m, ov_str(ov_str_new("body", 4)), ov_str(r->body));
  ov_map_set(m, ov_str(ov_str_new("headers", 7)), r->headers);
  ov_map_set(m, ov_str(ov_str_new("elapsed_ms", 10)), ov_float(r->elapsed_ms));
  ov_gc_end_mutation();
  *out = ov_map(m);
  return 0;
}

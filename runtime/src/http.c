/* Vayu runtime :: src/http.c
 *
 * HTTP client built on libcurl.
 *
 * Why libcurl for v0.1: TLS, redirects, proxy support, HTTP/2 and connection
 * reuse are all solved and battle-tested, and the PRD's rule is "build what
 * the project forces you to build" -- the project needs a working HTTPS POST
 * with JSON, not a TLS stack. The language sees one blocking `http.post()`;
 * the async layer in a later version wraps this with an event loop and a
 * multi handle rather than reimplementing the wire.
 *
 * The header takes JSON objects so the compiler stays trivial: the native map
 * the user wrote becomes a JSON string the runtime flattens. Errors that the
 * *transport* produced (DNS, TLS, timeout) come back as response.error rather
 * than a panic, so a Vayu program can `try` them like any other value.
 */
#include "vyrt.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
  char* buf;
  size_t len, cap;
} Sink;

static size_t sink_write(char* p, size_t sz, size_t nm, void* ud) {
  Sink* s = (Sink*)ud;
  size_t n = sz * nm;
  if (s->len + n + 1 > s->cap) {
    while (s->len + n + 1 > s->cap) s->cap = s->cap ? s->cap * 2 : 4096;
    char* nb = (char*)realloc(s->buf, s->cap);
    if (!nb) return 0;
    s->buf = nb;
  }
  memcpy(s->buf + s->len, p, n);
  s->len += n;
  s->buf[s->len] = '\0';
  return n;
}

typedef struct {
  VyHttpResponse* resp;
} HeaderSink;

static size_t header_cb(char* data, size_t sz, size_t nm, void* ud) {
  HeaderSink* h = (HeaderSink*)ud;
  size_t n = sz * nm;
  const char* colon = (const char*)memchr(data, ':', n);
  if (colon && h->resp) {
    size_t nlen = (size_t)(colon - data);
    while (nlen && (data[nlen - 1] == ' ')) nlen--;
    size_t vstart = (size_t)(colon - data) + 1;
    while (vstart < n && (data[vstart] == ' ' || data[vstart] == '\t')) vstart++;
    size_t vlen = n - vstart;
    while (vlen && (data[vstart + vlen - 1] == '\r' || data[vstart + vlen - 1] == '\n' ||
                    data[vstart + vlen - 1] == ' '))
      vlen--;
    char name[128], value[1024];
    if (nlen < sizeof(name) && vlen < sizeof(value)) {
      memcpy(name, data, nlen);
      name[nlen] = '\0';
      memcpy(value, data + vstart, vlen);
      value[vlen] = '\0';
      VyValue key = vy_str_val(name);
      VyMap* m = h->resp->headers.map;
      if (vy_map_has(m, key)) {
        /* repeated header (Set-Cookie): make it a list */
        VyValue prev = vy_map_get(m, key);
        if (vy_tagof(prev) == VY_LIST) vy_list_push(prev.list, vy_str_val(value));
        else {
          VyList* l = vy_list_new();
          vy_list_push(l, prev);
          vy_list_push(l, vy_str_val(value));
          vy_map_set(m, key, vy_list(l));
        }
      } else {
        vy_map_set(m, key, vy_str_val(value));
      }
    }
  }
  return n;
}

const char* vy_http_libcurl_version(void) { return curl_version(); }

/* URL-encode one component of a query string. */
static VyStr* url_encode(VyStr* s) {
  size_t extra = 0;
  for (uint32_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->bytes[i];
    if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != '~') extra++;
  }
  if (!extra) return vy_str_new(s->bytes, s->len);
  VyStr* r = vy_str_new(s->bytes, (size_t)s->len + extra * 2);
  size_t o = 0;
  static const char* hex = "0123456789ABCDEF";
  for (uint32_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->bytes[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      r->bytes[o++] = (char)c;
    } else {
      r->bytes[o++] = '%';
      r->bytes[o++] = hex[c >> 4];
      r->bytes[o++] = hex[c & 15];
    }
  }
  r->bytes[o] = '\0';
  r->len = (uint32_t)o;
  return r;
}

/* {"a":"b"} -> "a: b\0" -> curl slist. Values that are not strings use their
 * rendered form, which is what a header value is in practice. */
static struct curl_slist* build_headers(const char* headers_json) {
  if (!headers_json) return NULL;
  VyValue v = vy_json_parse(headers_json, strlen(headers_json));
  if (vy_tagof(v) != VY_MAP) return NULL;
  VyList* pairs = vy_map_pairs(v.map);
  struct curl_slist* list = NULL;
  for (uint32_t i = 0; i + 1 < pairs->len; i += 2) {
    VyValue k = pairs->items[i];
    VyValue val = pairs->items[i + 1];
    if (vy_tagof(k) != VY_STRING) continue;
    VyStr* vs = vy_render(val);
    VyStr* line = vy_str_concat(k.str, vy_str_new(": ", 2));
    line = vy_str_concat(line, vs);
    list = curl_slist_append(list, vy_str_data(line));
  }
  return list;
}

static VyStr* build_url(const char* url, const char* params_json) {
  if (!params_json) return vy_str_cstr(url);
  VyValue v = vy_json_parse(params_json, strlen(params_json));
  if (vy_tagof(v) != VY_MAP || v.map->len == 0) return vy_str_cstr(url);
  VyStr* out = vy_str_cstr(url);
  int first = strchr(vy_str_data(out), '?') == NULL;
  VyList* pairs = vy_map_pairs(v.map);
  for (uint32_t i = 0; i + 1 < pairs->len; i += 2) {
    VyValue k = pairs->items[i];
    VyValue val = pairs->items[i + 1];
    VyStr* ks = (vy_tagof(k) == VY_STRING) ? k.str : vy_render(k);
    VyStr* vs = vy_render(val);
    VyStr* ek = url_encode(ks);
    VyStr* ev = url_encode(vs);
    VyStr* sep = vy_str_new(first ? "?" : "&", 1);
    VyStr* chunk = vy_str_concat(sep, ek);
    chunk = vy_str_concat(chunk, vy_str_new("=", 1));
    chunk = vy_str_concat(chunk, ev);
    out = vy_str_concat(out, chunk);
    first = 0;
  }
  return out;
}

VyHttpResponse* vy_http_request(const char* method, const char* url,
                                const char* body, const char* content_type,
                                const char* headers_json, const char* params_json,
                                double timeout_s) {
  VyHttpResponse* r = (VyHttpResponse*)calloc(1, sizeof(VyHttpResponse));
  if (!r) return NULL;
  r->status = 0;
  r->body = vy_str_new("", 0);
  r->headers = vy_map(vy_map_new());
  r->error = NULL;
  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = '\0';

  CURL* h = curl_easy_init();
  if (!h) {
    r->error = vy_str_cstr("failed to initialise the HTTP client");
    return r;
  }

  VyStr* full_url = build_url(url, params_json);

  Sink sink = {NULL, 0, 0};
  HeaderSink hsink = {r};
  struct curl_slist* hdrs = build_headers(headers_json);
  /* libcurl carries the content type as a header, not a separate option */
  if (content_type && *content_type) {
    VyStr* line = vy_str_concat(vy_str_new("Content-Type: ", 14), vy_str_cstr(content_type));
    hdrs = curl_slist_append(hdrs, vy_str_data(line));
  }

  curl_easy_setopt(h, CURLOPT_URL, vy_str_data(full_url));
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink_write);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &hsink);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
  if (timeout_s > 0) {
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)(timeout_s * 1000.0));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_s * 1000.0));
  }
  if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);

  if (body && *body) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
             strcmp(method, "PATCH") == 0) {
    /* an explicit empty body still needs Content-Length: 0 */
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
  }

  CURLcode rc = curl_easy_perform(h);
  if (rc != CURLE_OK) {
    r->error = vy_str_cstr(errbuf[0] ? errbuf : curl_easy_strerror(rc));
    r->status = 0;
  } else {
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    r->status = (int64_t)code;
    double total = 0;
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total);
    r->elapsed_ms = total * 1000.0;
  }

  if (sink.buf) {
    r->body = vy_str_new(sink.buf, sink.len);
    free(sink.buf);
  }
  if (hdrs) curl_slist_free_all(hdrs);
  curl_easy_cleanup(h);
  return r;
}
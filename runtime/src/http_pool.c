/* Vayu runtime :: src/http_pool.c
 *
 * HTTP connection pool built on libcurl multi + easy handle reuse.
 *
 * Problem: every vy_http_request() calls curl_easy_init() + curl_easy_cleanup()
 * which means each AI API call pays TCP + TLS handshake overhead (50-200ms)
 * even when talking to the same server repeatedly.
 *
 * Solution: keep a pool of warm easy handles with persistent connections.
 * The pool is keyed by (scheme, host, port).  When a request goes to the same
 * server, the existing connection is reused -- no new TCP/TLS handshake.
 *
 * This is critical for agentic workloads where the loop is:
 *   LLM request → tool call → LLM request → tool call → ...
 * Each iteration POSTs to the same API endpoint.  Without pooling every
 * roundtrip pays TLS re-negotiation.
 *
 * API:
 *   VyHttpPool* vy_http_pool_new(int max_conns);
 *   void        vy_http_pool_free(VyHttpPool* p);
 *   VyHttpResponse* vy_http_pool_request(VyHttpPool* p, ...same args as vy_http_request...);
 *
 * Thread safety: not thread-safe -- use one pool per thread or add a mutex.
 */
#include "vyrt.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ----------------------------------------------------------------- pool */

#define VY_POOL_MAX_DEFAULT 8
#define VY_POOL_KEY_LEN     256

typedef struct PoolEntry {
  CURL* handle;
  char  key[VY_POOL_KEY_LEN];  /* "scheme://host:port" */
  int   in_use;
} PoolEntry;

struct VyHttpPool {
  PoolEntry* entries;
  int        cap;
  int        len;
};

VyHttpPool* vy_http_pool_new(int max_conns) {
  if (max_conns <= 0) max_conns = VY_POOL_MAX_DEFAULT;
  VyHttpPool* p = (VyHttpPool*)calloc(1, sizeof(VyHttpPool));
  if (!p) return NULL;
  p->entries = (PoolEntry*)calloc((size_t)max_conns, sizeof(PoolEntry));
  if (!p->entries) { free(p); return NULL; }
  p->cap = max_conns;
  p->len = 0;
  return p;
}

void vy_http_pool_free(VyHttpPool* p) {
  if (!p) return;
  for (int i = 0; i < p->len; i++)
    if (p->entries[i].handle) curl_easy_cleanup(p->entries[i].handle);
  free(p->entries);
  free(p);
}

/* Extract "scheme://host:port" from a URL for use as a pool key.
 * We only need enough to identify the connection target. */
static void url_key(const char* url, char* key, size_t klen) {
  /* Find end of "scheme://host:port" or "scheme://host/..." */
  const char* s = strstr(url, "://");
  if (!s) { strncpy(key, url, klen - 1); key[klen-1] = '\0'; return; }
  s += 3; /* skip :// */
  const char* slash = strchr(s, '/');
  size_t hostlen = slash ? (size_t)(slash - url) : strlen(url);
  if (hostlen >= klen) hostlen = klen - 1;
  memcpy(key, url, hostlen);
  key[hostlen] = '\0';
}

static CURL* pool_acquire(VyHttpPool* p, const char* key) {
  /* Look for an idle handle with a matching key. */
  for (int i = 0; i < p->len; i++) {
    if (!p->entries[i].in_use && strcmp(p->entries[i].key, key) == 0) {
      p->entries[i].in_use = 1;
      curl_easy_reset(p->entries[i].handle);
      return p->entries[i].handle;
    }
  }
  /* Allocate a new handle. */
  CURL* h = curl_easy_init();
  if (!h) return NULL;
  /* Store in pool if there is room. */
  if (p->len < p->cap) {
    p->entries[p->len].handle = h;
    strncpy(p->entries[p->len].key, key, VY_POOL_KEY_LEN - 1);
    p->entries[p->len].key[VY_POOL_KEY_LEN - 1] = '\0';
    p->entries[p->len].in_use = 1;
    p->len++;
  }
  return h;
}

static void pool_release(VyHttpPool* p, CURL* h) {
  for (int i = 0; i < p->len; i++) {
    if (p->entries[i].handle == h) {
      p->entries[i].in_use = 0;
      return;
    }
  }
  /* Not in pool (pool was full when allocated) -- clean up. */
  curl_easy_cleanup(h);
}

/* ----------------------------------------------------------- sink types */

typedef struct {
  char*  buf;
  size_t len, cap;
} HSink;

static size_t hsink_write(char* p, size_t sz, size_t nm, void* ud) {
  HSink* s = (HSink*)ud;
  size_t n = sz * nm;
  if (s->len + n + 1 > s->cap) {
    while (s->len + n + 1 > s->cap) s->cap = s->cap ? s->cap * 2 : 8192;
    char* nb = (char*)realloc(s->buf, s->cap);
    if (!nb) return 0;
    s->buf = nb;
  }
  memcpy(s->buf + s->len, p, n);
  s->len += n;
  s->buf[s->len] = '\0';
  return n;
}

static size_t hdr_write(char* data, size_t sz, size_t nm, void* ud) {
  VyHttpResponse* r = (VyHttpResponse*)ud;
  size_t n = sz * nm;
  const char* colon = (const char*)memchr(data, ':', n);
  if (colon && r) {
    size_t nlen = (size_t)(colon - data);
    while (nlen && data[nlen-1] == ' ') nlen--;
    size_t vs = (size_t)(colon - data) + 1;
    while (vs < n && (data[vs] == ' ' || data[vs] == '\t')) vs++;
    size_t vlen = n - vs;
    while (vlen && (data[vs+vlen-1] == '\r' || data[vs+vlen-1] == '\n' ||
                    data[vs+vlen-1] == ' ')) vlen--;
    char name[128], value[1024];
    if (nlen < sizeof(name) && vlen < sizeof(value)) {
      memcpy(name, data, nlen); name[nlen] = '\0';
      memcpy(value, data + vs, vlen); value[vlen] = '\0';
      VyValue key = vy_str_val(name);
      VyMap* m = r->headers.map;
      if (vy_map_has(m, key)) {
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

/* ----------------------------------------------- URL encode (shared) */

static char* pool_url_encode(CURL* curl, const char* s) {
  return curl_easy_escape(curl, s, (int)strlen(s));
}

/* ----------------------------------------------- pool request */

VyHttpResponse* vy_http_pool_request(VyHttpPool* pool,
                                     const char* method, const char* url,
                                     const char* body, const char* content_type,
                                     const char* headers_json,
                                     const char* params_json,
                                     double timeout_s) {
  VyHttpResponse* r = (VyHttpResponse*)calloc(1, sizeof(VyHttpResponse));
  if (!r) return NULL;
  r->status  = 0;
  r->body    = vy_str_new("", 0);
  r->headers = vy_map(vy_map_new());
  r->error   = NULL;

  char key[VY_POOL_KEY_LEN];
  url_key(url, key, sizeof(key));

  CURL* h = pool ? pool_acquire(pool, key) : curl_easy_init();
  if (!h) {
    r->error = vy_str_cstr("failed to get HTTP handle");
    return r;
  }

  /* Build full URL with query params. */
  char* full_url = (char*)url;
  char* dyn_url  = NULL;
  if (params_json) {
    VyValue pv = vy_json_parse(params_json, strlen(params_json));
    if (vy_tagof(pv) == VY_MAP && pv.map->len > 0) {
      VyList* pairs = vy_map_pairs(pv.map);
      size_t ulen = strlen(url);
      /* Estimate new URL length. */
      char* nb = (char*)malloc(ulen + 4096);
      if (nb) {
        memcpy(nb, url, ulen);
        nb[ulen] = '\0';
        int first = (strchr(url, '?') == NULL);
        for (uint32_t i = 0; i + 1 < pairs->len; i += 2) {
          VyValue k = pairs->items[i];
          VyValue v = pairs->items[i+1];
          if (vy_tagof(k) != VY_STRING) continue;
          char* ek = pool_url_encode(h, k.str->bytes);
          VyStr* vs = vy_render(v);
          char* ev  = pool_url_encode(h, vs->bytes);
          size_t cur = strlen(nb);
          snprintf(nb + cur, 4096, "%s%s=%s", first ? "?" : "&", ek ? ek : "", ev ? ev : "");
          if (ek) curl_free(ek);
          if (ev) curl_free(ev);
          first = 0;
        }
        full_url = nb;
        dyn_url  = nb;
      }
    }
  }

  /* Build headers. */
  struct curl_slist* hdrs = NULL;
  if (headers_json) {
    VyValue hv = vy_json_parse(headers_json, strlen(headers_json));
    if (vy_tagof(hv) == VY_MAP) {
      VyList* pairs = vy_map_pairs(hv.map);
      for (uint32_t i = 0; i + 1 < pairs->len; i += 2) {
        VyValue k = pairs->items[i];
        VyValue v = pairs->items[i+1];
        if (vy_tagof(k) != VY_STRING) continue;
        VyStr* vs = vy_render(v);
        VyStr* line = vy_str_concat(k.str, vy_str_new(": ", 2));
        line = vy_str_concat(line, vs);
        hdrs = curl_slist_append(hdrs, line->bytes);
      }
    }
  }
  if (content_type && *content_type) {
    char hbuf[256];
    snprintf(hbuf, sizeof(hbuf), "Content-Type: %s", content_type);
    hdrs = curl_slist_append(hdrs, hbuf);
  }

  HSink sink = {NULL, 0, 0};
  char errbuf[CURL_ERROR_SIZE]; errbuf[0] = '\0';

  curl_easy_setopt(h, CURLOPT_URL, full_url);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, hsink_write);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, hdr_write);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, r);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
  /* Keep connection alive for pool reuse. */
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPIDLE, 60L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPINTVL, 10L);
  if (timeout_s > 0) {
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)(timeout_s * 1000.0));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_s * 500.0));
  }
  if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
  if (body && *body) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 ||
             strcmp(method, "PATCH") == 0) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
  }

  CURLcode rc = curl_easy_perform(h);
  if (rc != CURLE_OK) {
    r->error  = vy_str_cstr(errbuf[0] ? errbuf : curl_easy_strerror(rc));
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
  if (dyn_url) free(dyn_url);

  if (pool) pool_release(pool, h);
  else curl_easy_cleanup(h);

  return r;
}

/* Global default pool -- used by the Vayu language http.* builtins when no
 * explicit pool is specified.  Lazily initialised on first use. */
static VyHttpPool* g_default_pool = NULL;

VyHttpPool* vy_http_default_pool(void) {
  if (!g_default_pool) g_default_pool = vy_http_pool_new(8);
  return g_default_pool;
}

void vy_http_pool_cleanup(void) {
  vy_http_pool_free(g_default_pool);
  g_default_pool = NULL;
}

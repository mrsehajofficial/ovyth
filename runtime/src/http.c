/* Ovyth runtime :: src/http.c
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
 * than a panic, so a Ovyth program can `try` them like any other value.
 *
 * `ov_http_request()` is now a thin adapter over the connection pool in
 * http_pool.c. Every AI API call in an agent loop targets the same host, so
 * the pooled easy handle keeps the TCP+TLS session warm instead of paying a
 * 50-200ms handshake on every turn. It also means this file no longer owns a
 * second copy of the response sink, the header parser and the query-string
 * encoder -- one implementation serves both the builtin and the explicit
 * pool API.
 */
#include "ovrt.h"

#include <curl/curl.h>

const char* ov_http_libcurl_version(void) { return curl_version(); }

OvHttpResponse* ov_http_request(const char* method, const char* url,
                                const char* body, const char* content_type,
                                const char* headers_json, const char* params_json,
                                double timeout_s) {
  /* The default pool is created lazily and freed by ov_http_pool_cleanup()
   * at runtime shutdown, so callers never manage its lifetime. */
  return ov_http_pool_request(ov_http_default_pool(),
                              method, url,
                              body, content_type,
                              headers_json, params_json,
                              timeout_s);
}

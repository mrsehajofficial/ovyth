/* Vayu :: benchmarks/ref_ai.c
 *
 * C reference implementation of the AI pipeline benchmark.
 * Matches benchmarks/bench_ai.vy case-for-case so the comparison
 * is honest: same algorithms, same inputs, same outputs.
 *
 * Build: clang -O3 -std=c11 benchmarks/ref_ai.c -o /tmp/ref_ai -lm && /tmp/ref_ai
 *
 * Each case prints: name<TAB>ms<TAB>description
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* --------------------------------------------------------------------- arena */

typedef struct { char* p; size_t used, cap; } Arena;

static void arena_init(Arena* a, size_t cap) {
  a->p    = (char*)malloc(cap);
  a->used = 0;
  a->cap  = cap;
}
static void* arena_alloc(Arena* a, size_t n) {
  n = (n + 7u) & ~7u;
  if (a->used + n > a->cap) {
    a->cap = a->cap * 2 + n;
    a->p   = (char*)realloc(a->p, a->cap);
  }
  void* r = a->p + a->used; a->used += n; return r;
}
static void arena_reset(Arena* a) { a->used = 0; }
static void arena_free(Arena* a)  { free(a->p); a->p = NULL; }

/* --------------------------------------------------------------------- mini JSON */

/* A trivial JSON parser that only extracts the "content" field from a
 * choices[0].message.content path -- the exact same access bench_ai.vy uses.
 * Not a full parser; just enough to match the workload. */

static const char* skip_ws(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

/* Returns pointer past the closing '"'; copies into buf (max buflen-1 chars). */
static const char* parse_str(const char* p, char* buf, size_t buflen) {
  if (*p != '"') return p;
  p++;
  size_t i = 0;
  while (*p && *p != '"') {
    if (*p == '\\') { p++; if (*p) { if (i+1 < buflen) buf[i++] = *p; p++; } continue; }
    if (i+1 < buflen) buf[i++] = *p;
    p++;
  }
  buf[i] = '\0';
  if (*p == '"') p++;
  return p;
}

/* Find "content":"..." in a completion JSON response, copy into out. */
static int find_content(const char* json, char* out, size_t outlen) {
  /* Fast scan for '"content":' -- not a real parser, just a realistic stand-in */
  const char* p = json;
  while (*p) {
    if (strncmp(p, "\"content\"", 9) == 0) {
      p += 9;
      p = skip_ws(p);
      if (*p == ':') { p++; p = skip_ws(p); }
      if (*p == '"') { parse_str(p, out, outlen); return 1; }
    }
    p++;
  }
  return 0;
}

/* --------------------------------------------------------------------- cases */

static volatile long long g_sink_i;
static volatile double    g_sink_f;

static const char* SAMPLE = "{\"id\":\"chatcmpl-abc123\",\"object\":\"chat.completion\",\"created\":1700000000,\"model\":\"gpt-4\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Arena allocators free all request-scoped memory in O(1) by releasing an entire block, eliminating per-object malloc/free overhead. This is critical for agent workloads where each request creates hundreds of temporary strings, JSON objects, and embedding inputs.\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":52,\"total_tokens\":94}}";

static const char* TOOL_JSON = "{\"status\":\"ok\",\"data\":{\"query\":\"arena allocator\",\"results\":[{\"title\":\"Arena Allocator Design\",\"score\":0.92},{\"title\":\"Memory Management\",\"score\":0.87}],\"total\":2}}";

/* 1. JSON parse: find content field N_JSON times */
static void bench_json_parse(int n) {
  char buf[512];
  for (int i = 0; i < n; i++) {
    find_content(SAMPLE, buf, sizeof(buf));
    g_sink_i = (long long)strlen(buf);
  }
}

/* 2. JSON access: same as parse (in C these are combined) */
static void bench_json_access(int n) {
  char buf[512];
  find_content(SAMPLE, buf, sizeof(buf));
  volatile int len = (int)strlen(buf);
  for (int i = 0; i < n; i++) {
    g_sink_i = len;
  }
}

/* 3. Context assembly: concatenate 8 chunk strings with numbering */
static const char* CHUNKS[] = {
  "Arena allocators free all request-scoped memory in O(1) by releasing an entire block.",
  "SIMD JSON scanning processes 16 bytes per instruction using SSE4.2 on x86 or NEON on ARM.",
  "Connection pooling eliminates 50-200ms of TLS handshake overhead per repeated API call.",
  "Pipeline fusion combines multiple data transformations into a single pass over the data.",
  "Zero-copy string slicing returns (pointer, length) into existing storage, avoiding allocation.",
  "io_uring provides async I/O with shared ring buffers, reducing syscall count significantly.",
  "Backpressure prevents upstream stages from overwhelming slower downstream stages with data.",
  "Adaptive I/O chooses zero-copy vs buffered based on payload size and hardware capabilities.",
};
#define N_CHUNKS 8

static void bench_context_build(int n) {
  char buf[8192];
  for (int iter = 0; iter < n; iter++) {
    size_t pos = 0;
    for (int i = 0; i < N_CHUNKS && pos < sizeof(buf)-256; i++) {
      int written = snprintf(buf + pos, sizeof(buf) - pos,
                             "[%d] %s\n", i+1, CHUNKS[i]);
      if (written > 0) pos += (size_t)written;
    }
    g_sink_i = (long long)pos;
  }
}

/* 4. Chunk pipeline: split by word, chunk into groups of 8 */
static const char* DOC = "This document discusses arena allocators and their role in high-performance AI agent systems where memory allocation overhead is critical for latency sensitive workloads processing thousands of requests per second with minimal garbage collection pauses";

static void bench_chunk_pipeline(int n, Arena* a) {
  long long total = 0;
  char* words[128];
  int nwords = 0;

  /* Tokenise once */
  char tmp[1024];
  strncpy(tmp, DOC, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
  char* tok = strtok(tmp, " ");
  while (tok && nwords < 128) { words[nwords++] = tok; tok = strtok(NULL, " "); }

  for (int iter = 0; iter < n; iter++) {
    arena_reset(a);
    int chunks = 0;
    for (int i = 0; i < nwords; i += 8) { (void)arena_alloc(a, 64); chunks++; }
    total += chunks;
  }
  g_sink_i = total;
}

/* 5. Hash map: open-addressing map of string -> int */
#define HM_CAP (1 << 15)  /* 32768 slots for 10000 entries */
struct kv { char k[32]; long long v; int used; };
static struct kv hm[HM_CAP];

static uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}

static void hm_put(const char* k, long long v) {
  uint32_t i = fnv1a(k) & (HM_CAP - 1);
  while (hm[i].used && strcmp(hm[i].k, k) != 0) i = (i+1) & (HM_CAP-1);
  strncpy(hm[i].k, k, 31); hm[i].k[31]='\0'; hm[i].v = v; hm[i].used = 1;
}
static long long hm_get(const char* k) {
  uint32_t i = fnv1a(k) & (HM_CAP - 1);
  while (hm[i].used && strcmp(hm[i].k, k) != 0) i = (i+1) & (HM_CAP-1);
  return hm[i].used ? hm[i].v : 0;
}

static void bench_hashmap(int n) {
  char key[32];
  memset(hm, 0, sizeof(hm));
  for (int i = 0; i < n; i++) {
    snprintf(key, sizeof(key), "tool_result_%d", i);
    hm_put(key, (long long)i * 2);
  }
  long long sum = 0;
  for (int i = 0; i < n; i++) {
    snprintf(key, sizeof(key), "tool_result_%d", i);
    sum += hm_get(key);
  }
  g_sink_i = sum;
}

/* 6. Multi-JSON parse: extract "score" field from tool results */
static void bench_multi_parse(int n) {
  char buf[64];
  double total = 0.0;
  for (int i = 0; i < n; i++) {
    /* Find "score":0.92 in TOOL_JSON */
    const char* p = strstr(TOOL_JSON, "\"score\":");
    if (p) {
      p += 8;
      total += strtod(p, NULL);
    }
  }
  g_sink_f = total;
  (void)buf;
}

/* 7. String scan: find '"' in haystack */
static const char* HAYSTACK = "The quick brown fox jumps over the lazy dog. Arena allocators free all request-scoped memory in O(1). SIMD scanning finds structural characters at 16 bytes per cycle.";

static void bench_string_scan(int n) {
  long long found = 0;
  for (int i = 0; i < n; i++) {
    const char* p = strchr(HAYSTACK, '"');
    if (p) found++;
  }
  g_sink_i = found;
}

/* ---------------------------------------------------------------------- main */

int main(void) {
  printf("=== C AI pipeline reference ===\n\n");
  double t, dt;

  Arena a;
  arena_init(&a, 64 * 1024);

  volatile int N_JSON   = 50000;
  volatile int N_ACCESS = 500000;
  volatile int N_CTX    = 100000;
  volatile int N_DOCS   = 500;
  volatile int N_MAP    = 10000;
  volatile int N_TOOL   = 20000;
  volatile int N_SCAN   = 1000000;

  t = now_ms(); bench_json_parse(N_JSON);   dt = now_ms() - t;
  printf("json_parse\t%.2fms\t%d parses\n", dt, N_JSON);

  t = now_ms(); bench_json_access(N_ACCESS); dt = now_ms() - t;
  printf("json_access\t%.2fms\t%d accesses\n", dt, N_ACCESS);

  t = now_ms(); bench_context_build(N_CTX);  dt = now_ms() - t;
  printf("context_build\t%.2fms\t%d assemblies\n", dt, N_CTX);

  t = now_ms(); bench_chunk_pipeline(N_DOCS, &a); dt = now_ms() - t;
  printf("chunk_pipeline\t%.2fms\t%d docs\n", dt, N_DOCS);

  t = now_ms(); bench_hashmap(N_MAP);       dt = now_ms() - t;
  printf("hash_map_str\t%.2fms\t%d inserts+reads\tsum=%lld\n", dt, N_MAP*2, g_sink_i);

  t = now_ms(); bench_multi_parse(N_TOOL);  dt = now_ms() - t;
  printf("multi_parse\t%.2fms\t%d tool_jsons\n", dt, N_TOOL);

  t = now_ms(); bench_string_scan(N_SCAN);  dt = now_ms() - t;
  printf("string_scan\t%.2fms\t%d scans\n", dt, N_SCAN);

  arena_free(&a);
  return 0;
}

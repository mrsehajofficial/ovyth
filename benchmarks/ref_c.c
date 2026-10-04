/* Reference implementation of the Vayu benchmark suite, in C.
 *
 * The comparison baseline for benchmarks/compare.sh (spec section 35). C is the
 * right control for "how much of Vayu's cost is the language runtime rather
 * than the algorithm": the algorithms here are the same ones as
 * benchmarks/cases/*.vy, on the same inputs, doing the same work.
 *
 * Timing is internal (clock_gettime around each case) so the numbers are not
 * dominated by process startup; the Vayu side reports whole-process wall clock
 * too, and the driver prints both so the difference is visible rather than
 * hidden.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* --- intloop --- */
static long long bench_intloop(long long n) {
  long long total = 0;
  for (long long i = 0; i < n; i++) total = total + i * 3 - 1;
  return total;
}

/* --- fib --- */
static long long fib(int k) { return k < 2 ? k : fib(k - 1) + fib(k - 2); }

/* --- strconcat: an amortising buffer, i.e. the *fast* string concat --- */
struct sb { char *b; size_t len, cap; };
static void sb_grow(struct sb *s, size_t need) {
  if (s->len + need <= s->cap) return;
  size_t cap = s->cap ? s->cap : 64;
  while (cap < s->len + need) cap *= 2;
  s->b = realloc(s->b, cap);
  s->cap = cap;
}
static void sb_put(struct sb *s, const char *p, size_t n) {
  sb_grow(s, n);
  memcpy(s->b + s->len, p, n);
  s->len += n;
}

/* --- listappend --- */
static long long bench_listappend(long long n, size_t *out_len) {
  long long *xs = malloc((size_t)n * sizeof(long long));
  long long s = 0;
  for (long long i = 0; i < n; i++) { xs[i] = i; s += i; }
  *out_len = (size_t)n;
  free(xs);
  return s;
}

/* --- mapops: a hash map keyed by decimal text --- */
struct kv { char *k; long long v; int used; };
static size_t hashs(const char *s) {
  size_t h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}
static void map_put(struct kv *t, size_t cap, const char *k, long long v) {
  size_t i = hashs(k) & (cap - 1);
  while (t[i].used) {
    if (strcmp(t[i].k, k) == 0) { t[i].v = v; return; }
    i = (i + 1) & (cap - 1);
  }
  t[i].used = 1; t[i].k = strdup(k); t[i].v = v;
}
static long long map_get(struct kv *t, size_t cap, const char *k) {
  size_t i = hashs(k) & (cap - 1);
  while (t[i].used) {
    if (strcmp(t[i].k, k) == 0) return t[i].v;
    i = (i + 1) & (cap - 1);
  }
  return 0;
}

/* Results are stored here so the optimiser cannot delete the work, and every
 * size below is read through a volatile at the call site: with a literal,
 * clang -O3 constant-folds bench_intloop/bench_listappend into a formula and
 * the reference reports a fictional 0.00ms. Timing is also taken in separate
 * statements -- inside printf() the clock read and the work are unordered
 * (argument evaluation order is unspecified). */
static volatile long long g_sink;

int main(void) {
  volatile long long n_intloop = 20000000;
  volatile long long n_list = 500000;
  double t, dt;
  long long r;
  size_t sz;

  t = now_ms();
  r = bench_intloop(n_intloop);
  dt = now_ms() - t;
  g_sink = r;
  printf("intloop %.2f %lld\n", dt, r);

  t = now_ms();
  r = fib(25);
  dt = now_ms() - t;
  g_sink = r;
  printf("fib %.2f %lld\n", dt, r);

  t = now_ms();
  { struct sb s = {0};
    for (int i = 0; i < 40000; i++) sb_put(&s, "hello world ", 12);
    sz = s.len;
    free(s.b); }
  dt = now_ms() - t;
  printf("strconcat %.2f %zu\n", dt, sz);

  t = now_ms();
  { size_t len; r = bench_listappend(n_list, &len); sz = len; }
  dt = now_ms() - t;
  g_sink = r;
  printf("listappend %.2f %zu %lld\n", dt, sz, r);

  t = now_ms();
  { size_t cap = 262144;
    struct kv *tab = calloc(cap, sizeof *tab);
    char buf[32];
    for (int i = 0; i < 100000; i++) {
      snprintf(buf, sizeof buf, "key%d", i);
      map_put(tab, cap, buf, i);
    }
    long long s = 0;
    for (int i = 0; i < 100000; i++) {
      snprintf(buf, sizeof buf, "key%d", i);
      s += map_get(tab, cap, buf);
    }
    sz = 100000; r = s;
    free(tab); }
  dt = now_ms() - t;
  g_sink = r;
  printf("mapops %.2f %zu %lld\n", dt, sz, r);

  return 0;
}
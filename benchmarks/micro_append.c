// Ovyth :: benchmarks/micro_append.c
//
// Is the append slow in the runtime, or is the codegen emitting something
// worse than the runtime?
//
// The language-level profile showed 500k appends costing 24-38ns per element
// while C -O3 does the same in 0.56ns, and the obvious suspects (the loop, the
// zeroing of a buffer about to be filled) were ruled out by ablation. This
// benchmark calls the runtime's own push in a plain C loop, so the codegen is
// out of the picture entirely: whatever this measures is the runtime.
//
//   cc -O2 benchmarks/micro_append.c build/libovrt.a -o /tmp/micro \
//      -lcurl -lssl -lcrypto -lz -lm -lpthread -ldl && /tmp/micro
//
// Three rows answer three different questions:
//   push, growing     -- what the language actually pays today
//   push, pre-sized   -- the same store with the growth removed
//   raw C             -- the floor for a bounds-checked store
#define _POSIX_C_SOURCE 200809L  // clock_gettime, and before any header
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ovrt.h"

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define N 500000

static void row(const char* what, double t0, double t1) {
  printf("  %-18s %7.2f ms   %6.1f ns/elem\n", what, (t1 - t0) * 1e3,
         (t1 - t0) * 1e9 / (double)N);
}

int main(void) {
  ov_runtime_init(0, NULL);

  /* 1. the runtime's push, growing -- exactly what codegen emits */
  double t0 = now();
  OvValue xs = ov_i64a_val(ov_i64a_new());
  for (int64_t i = 0; i < N; i++) ov_i64a_push(xs.i64a, i);
  double t1 = now();
  row("push, growing", t0, t1);
  printf("                     len=%u cap=%u\n", xs.i64a->len, xs.i64a->cap);

  /* 2. the same store, with the growth removed: the array is sized up front,
   *    so every push takes the inline fast path and nothing reallocs. */
  t0 = now();
  OvValue ys = ov_i64a_val(ov_i64a_new_cap(N));
  for (int64_t i = 0; i < N; i++) ov_i64a_push(ys.i64a, i);
  t1 = now();
  row("push, pre-sized", t0, t1);

  /* 3. the floor: a raw C array with the same access pattern. */
  t0 = now();
  int64_t* raw = (int64_t*)malloc((size_t)N * sizeof(int64_t));
  for (int64_t i = 0; i < N; i++) raw[i] = i;
  t1 = now();
  row("raw C", t0, t1);
  printf("                     checksum=%lld\n", (long long)raw[N - 1]);

  /* 4. and a C array that grows geometrically, to see what growth itself
   *    costs when it is not behind a runtime call. */
  t0 = now();
  uint32_t cap = 4;
  int64_t* g = (int64_t*)malloc(cap * sizeof(int64_t));
  for (int64_t i = 0; i < N; i++) {
    if ((uint32_t)i >= cap) {
      cap *= 2;
      g = (int64_t*)realloc(g, (size_t)cap * sizeof(int64_t));
    }
    g[i] = i;
  }
  t1 = now();
  row("raw C, growing", t0, t1);
  printf("                     checksum=%lld\n", (long long)g[N - 1]);

  /* 5. the shape codegen actually emits for `while i < N { xs.push(i); i = i + 1 }`:
   *    an OvValue built for the condition and tested through ov_truthy, with the
   *    unboxed int64 local. If this lands near the language-level number, the
   *    gap is the codegen's per-iteration plumbing rather than the runtime. */
  t0 = now();
  OvValue zs = ov_i64a_val(ov_i64a_new());
  int64_t i = 0;
  while (1) {
    { OvValue _t = ov_bool((i) < ((int64_t)N)); if (!ov_truthy(_t)) break; }
    (void)((ov_i64a_push(zs.i64a, i), zs));
    i = (i) + (1LL);
  }
  t1 = now();
  row("codegen shape", t0, t1);

  /* 6. the same, with a raw condition instead of a boxed one -- what the
   *    codegen *should* emit when both operands are proven ints. */
  t0 = now();
  OvValue ws = ov_i64a_val(ov_i64a_new());
  int64_t k = 0;
  while (1) {
    if (!((k) < ((int64_t)N))) break;
    (void)((ov_i64a_push(ws.i64a, k), ws));
    k = (k) + (1LL);
  }
  t1 = now();
  row("raw condition", t0, t1);

  /* 7. the codegen shape, with the array reached through a GC root the way
   *    generated code reaches a live local: `v_xs_4` is registered so the
   *    collector can see it, which may stop clang keeping the pointer in a
   *    register across the loop. */
  t0 = now();
  OvValue vs = ov_i64a_val(ov_i64a_new());
  ov_gc_register_root(&vs);
  int64_t m = 0;
  while (1) {
    { OvValue _t = ov_bool((m) < ((int64_t)N)); if (!ov_truthy(_t)) break; }
    (void)((ov_i64a_push(vs.i64a, m), vs));
    m = (m) + (1LL);
  }
  t1 = now();
  row("rooted", t0, t1);

  /* 8. and the same loop reaching the array through a plain local alias: the
   *    root still keeps the object alive, but the pointer itself is a register. */
  t0 = now();
  OvValue us = ov_i64a_val(ov_i64a_new());
  ov_gc_register_root(&us);
  OvInt64Array* alias = us.i64a;
  int64_t q = 0;
  while (1) {
    { OvValue _t = ov_bool((q) < ((int64_t)N)); if (!ov_truthy(_t)) break; }
    (void)((ov_i64a_push(alias, q), us));
    q = (q) + (1LL);
  }
  t1 = now();
  row("rooted + alias", t0, t1);

  ov_runtime_shutdown();
  return 0;
}

# Vayu AI Pipeline Benchmarks

> **Note:** These benchmarks measure the AI orchestration layer (JSON, context assembly, chunking, map operations) -- not LLM inference. LLM inference dominates at 100ms-10s; we optimize the 1-50ms overhead.

## Results (Linux x86-64, 7 Oct 2026, Vayu --release vs C -O3 vs Python)

Fresh output of `bash benchmarks/compare_ai.sh --release`:

```
=== Vayu vs C vs Python -- AI pipeline benchmark ===

case                 Vayu (ms)    C (ms)   Py (ms)        vs C   vs Python
------------------  ----------  --------  --------  ----------  ----------
json_parse                 0ms     122ms     581ms  0.0x faster  0.0x faster
json_access             3000ms       0ms     156ms           ?  19.2x slower
context_build           2000ms     145ms     465ms  13.8x slower  4.3x slower
chunk_pipeline             0ms       0ms       4ms           ?  0.0x faster
hash_map_str               0ms       5ms      13ms  0.0x faster  0.0x faster
multi_parse             1000ms       4ms     152ms  250.0x slower  6.6x slower
string_scan                0ms       0ms     262ms           ?  0.0x faster
```

> **Timer-granularity warning (read before quoting these):** `bench_ai.vy`
> reports whole seconds, so any case under ~1s prints `0ms` and cases near
> a second flip between `0/1000/2000ms` run to run (repeat runs gave
> `json_parse: 1000/0/0ms`, `context_build: 2000ms stable`,
> `multi_parse: 0ms` with `1000ms` on other runs). **A `0ms` row means
> "below the 1-second timer granularity", not "instant".** Honest readings
> from raw runs: `json_parse` ~600-1000ms vs Python ~580-600ms (~1.7x
> slower when it reports 1000ms); `json_access` 3000ms vs ~155-165ms
> (~19x slower, stable); `context_build` 1000-2000ms vs ~465-487ms
> (2.1-4.3x slower); `chunk_pipeline`/`hash_map_str`/`string_scan` below
> granularity; `multi_parse` flips 0-1000ms vs ~152-177ms (0-6.6x slower).

## Methodology caveat: NOT like-for-like

Unlike `compare.sh` (checked identical answers), the three AI
implementations do *different work* per case: `json_access` re-runs
`json.extract` on the string 500k times in Vayu, but Python does 500k
dict lookups after one parse and C reads back a cached length (~0ms --
an empty loop, not a parse). `chunk_pipeline` in C tokenises once then
bumps an arena counter; `multi_parse` in C is `strstr`+`strtod` while
Vayu/Python build full objects. So this table is "idiomatic pipeline
stages", not the same algorithm -- useful for hot spots, not speedups.
Like-for-like AI cases with checked outputs and a ms timer are open work.

## Key Optimizations Applied (v0.1.2)

### 1. Fast JSON Extraction (`json.extract`)
- Path syntax: `"choices[0].message.content"`
- Parses only needed fields without building full AST
- Direct `memcmp` key matching, no VyStr allocation per key
- Stack-allocated reusable buffer for temporary keys
- **Result: `json_access` 5000ms -> 3000ms** (~40% faster, was 30.9x slower
  than Python, now ~19x slower); `json_parse` ~1.7x slower than Python

### 2. String Builder for Join (`join()`)
- O(n) instead of O(n²) for repeated concatenation
- Uses `VyStrBuilder` to avoid per-element allocations
- **Result: no measurable change with the 1s timer** (`context_build`
  1000ms before, 1000-2000ms after -- the earlier "1000ms -> 0ms" was a
  timer-quantization artifact). Needs a ms timer to quantify.

### 3. Compile-time Path Specialization
- Compiler detects string literal paths and embeds them directly
- Avoids creating VyValue for path at runtime
- Reduces overhead in tight loops (part of the `json_access` win)

### 4. GC Mitigation
- Added `gc.disable()` / `gc.enable()` for controlled garbage collection
- Arena allocator for request-scoped allocations
- Reusable buffers in hot paths

### 5. Map Operations Optimized
- Pre-sized maps when size is known
- Reduced hash table resizes

## What Still Needs Work

- **Millisecond timer for `bench_ai.vy`**: the 1s `round()` granularity
  makes 4 of 7 cases unquotable. Print fractional ms and re-run before
  claiming AI wins.
- **Like-for-like AI cases**: align the three implementations to the same
  algorithm with checked outputs, as `compare.sh` does.
- **json_access**: ~19x slower than Python - nested field access still has
  GC overhead per extraction
- **multi_parse**: up to ~6.6x slower - could use arena allocation
- **Type inference**: Loop counters could use native int types
- **Arena allocation**: Could further reduce allocations in loops

## Notes
- Vayu numbers are whole-process wall clock (~4-5ms startup included)
- C and Python time their cases internally
- Build: C uses clang -O3 -march=native, Vayu uses --release (-O3)
- Machine: 4 cores, Linux x86-64; Python 3.14
- All 32 regression tests pass
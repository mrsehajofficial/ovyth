# Changelog

Notable changes to Vayu. Versions follow [semantic
versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## 0.1.4 (in progress)

### Specialized Array Types

Introduced three specialized array types for containers of known element type:

- **`VyInt64Array`** — contiguous `int64_t*` buffer with O(1) push/pop/get/set
- **`VyFloat64Array`** — contiguous `double*` buffer with the same fast paths
- **`VyStringArray`** — contiguous `VyStr**` buffer for strings

When a list literal contains only values of the same proven type (e.g. `[1, 2, 3]`),
the compiler emits a specialized array instead of a generic `VyList`. The types
are transparent: they render as `[1, 2, 3]`, iterate in `for` loops and
comprehensions, and support tuple unpacking — all through the same tag-guarded fast
paths used elsewhere.

- Runtime: `vy_i64a_new_cap`, `vy_f64a_new_cap`, `vy_stra_new_cap` with geometric
  growth (2x capacity); tag guards for push/get/pop/set in the hot path;
  fallback to `_slow` path for reallocation.
- Codegen: `ListLit` emits specialized arrays when all items are proven int/float;
  `emit_for` and `emit_listcomp` dispatch on all six tags; tuple destructuring
  handles `VY_I64A`/`VY_F64A`/`VY_STRA`; `vy_render` displays them as lists.

### Unboxed Numeric Locals

Locals proven int/float now emit as raw `int64_t`/`double` in generated C:

- No `VyValue` boxing/unboxing for proven int/float variables
- No GC root registration for these locals
- Fast arithmetic paths (add, sub, mul, div, mod, comparisons, bitwise) on raw locals
- Inlined builtins: `len`, `sum`, `abs`, `sqrt`, `floor`, `ceil`, `round`, `min`, `max`, `range`

### Test results

All 32 regression tests pass across both backends (interpreter + native).
Runtime self-test: 0 failures.

## 0.1.3 (benchmark re-check, 7 Oct 2026)

Re-ran all three benchmark suites on Linux x86-64 and corrected every
published number. No code changed -- this is a docs-and-numbers sync.

### Core suite (`compare.sh` / `run.sh`) -- ratios drifted, tables updated

Fresh best-of-3 wall clock (`intloop` varies 193-275ms run to run,
±20-40% desktop noise):

```
case         Vayu (wall)       C -O3      Python   result check
--------------------------------------------------------------------
intloop             237ms    42.14ms     5612ms   identical, py: identical
fib                   7ms     0.37ms       30ms   identical, py: identical
strconcat            14ms     0.81ms        9ms   identical, py: identical
listappend           34ms     0.11ms       86ms   identical, py: identical
mapops              187ms    64.99ms      156ms   identical, py: identical
```

- vs Python is now **2.5x-24x** (was 2.6x-26x): `fib` re-measures at
  4.3x, not 8x; `mapops` at 1.2x slower, not parity.
- vs C is now **2.9x-309x** (was 4.5x-378x): `fib` ~19x (was 12x),
  `listappend` ~309x (was 378x), `strconcat` ~17x (was 21x),
  `intloop` ~5.6x (was 4.5x), `mapops` ~2.9x (was 2.8x).
- `run.sh`: `intloop` best 275ms (was 193ms -- run-to-run noise, not a
  regression); `intloop` binary 26,952 -> 31,048 bytes and `json`
  35,168 -> 39,264 bytes from the v0.1.2 inline path specialization.
- Updated: `README.md` headline + table, `docs/PERFORMANCE.md` §§1-5,
  `benchmarks/RESULTS.md` §§1-3.

### AI suite (`compare_ai.sh --release`) -- two bad claims corrected

- **Corrected: "context assembly 1000ms -> 0ms (~100% faster)".**
  Fresh runs show `context_build` at 1000-2000ms vs Python ~465-487ms
  (2.1-4.3x slower). The `0ms` was the 1-second timer quantizing a
  sub-second case, not a speedup. `RESULTS_AI.md`, `PERFORMANCE.md` §7
  and the 0.1.2 entry below now say so.
- **Corrected: the HEAD `RESULTS_AI.md` table's `0ms` wins** for
  `json_parse`/`context_build`/`multi_parse`. Repeat runs of the same
  binary flip 0/1000/2000ms; honest readings are `json_parse` ~1.7x
  slower, `json_access` 3000ms stable (~19x slower, was 30.9x --
  the v0.1.2 `memcmp` + path-specialization win is real),
  `multi_parse` 0-6.6x slower.
- **Disclosed: the AI comparison is not like-for-like.** Vayu
  re-extracts from the JSON string, Python does dict lookups after one
  parse, C's `json_access` times a cached-length loop (~0ms). Documented
  in `RESULTS_AI.md` and `PERFORMANCE.md` §7; like-for-like AI cases
  with a millisecond timer are open work.
- Fresh AI table (7 Oct 2026): `json_parse` 0ms / 122ms / 581ms,
  `json_access` 3000ms / 0ms / 156ms, `context_build` 2000ms / 145ms /
  465ms, `chunk_pipeline` 0/0/4ms, `hash_map_str` 0/5/13ms,
  `multi_parse` 1000ms / 4ms / 152ms, `string_scan` 0/0/262ms --
  with the timer-granularity warning attached.

### Docs touched

- `benchmarks/RESULTS.md` -- fresh `compare.sh`/`run.sh` tables, variance
  note, binary-size growth explained.
- `benchmarks/RESULTS_AI.md` -- fresh table, timer warning, not-like-for-like
  caveat, corrected optimization results.
- `README.md` -- headline (2.5x-24x / 1.2x-19x AI / 2.9x-309x), expanded
  workload table, AI timer pointer, fresh vs-C ratios.
- `docs/PERFORMANCE.md` -- §§1-3, §5, §7 rewritten with fresh numbers.

---

## 0.1.2

### Performance Optimizations (AI/RAG Pipeline)

Significant performance improvements for AI orchestration workloads:

- **json.extract() optimization** (`runtime/src/json_extract.c`):
  - Eliminated per-key VyStr allocation during JSON field extraction
  - Direct pointer comparison (`memcmp`) for key matching instead of string allocation
  - Stack-allocated reusable buffer for temporary key parsing
  - **Result: ~40% faster** on repeated extraction (`json_access`
    5000ms → 3000ms for 500k extractions, re-measured 7 Oct 2026;
    single extraction ~1.7x slower than Python)

- **Compile-time path specialization** (`compiler/backend/codegen_c.cpp`):
  - Compiler detects string literal paths and embeds them directly in generated C
  - Avoids creating VyValue for path string at runtime
  - Reduces overhead in tight loops (part of the `json_access` win above)

- **String builder for join()** (`runtime/include/vy_sb.h`):
  - O(n) instead of O(n²) for repeated concatenation
  - Uses `VyStrBuilder` with geometric growth to avoid per-element allocations
  - **Result: no measurable change with the 1s `bench_ai.vy` timer**
    (`context_build` 1000ms before, 1000-2000ms after on re-check --
    the earlier "1000ms → 0ms / ~100% faster" was timer quantization,
    corrected 7 Oct 2026; needs a ms timer to quantify)

- **GC mitigation** (`runtime/src/vyrt_helpers.c`):
  - Added `gc.disable()` / `gc.enable()` for controlled garbage collection
  - Arena allocator for request-scoped allocations with O(1) teardown
  - Reusable buffers in hot paths

### Benchmarks (v0.1.2, re-measured 7 Oct 2026)

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

> `bench_ai.vy` reports whole seconds: `0ms` = below the 1s timer
> granularity (not "instant"), and near-second cases flip 0/1000/2000ms
> run to run. Honest readings: `json_parse` ~1.7x slower than Python when
> it reports 1000ms; `json_access` 3000ms stable (~19x slower); 
> `context_build` 1000-2000ms (2.1-4.3x slower); `multi_parse` flips
> 0-1000ms (0-6.6x slower). The three implementations also do different
> work per case (not like-for-like) -- see the Unreleased section above
> and `benchmarks/RESULTS_AI.md`.

### GC Runtime Stability (v0.1.1 fixes maintained)

Fixed critical garbage collection bugs that caused segfaults during high-iteration JSON parsing with nested index access (common in AI/RAG workloads):

- **Added GC root scanner API** (`runtime/src/gc.c`, `runtime/include/vyrt.h`)
  - `vy_gc_set_scanner()` — register dynamic root scanners during collection
  - `vy_gc_mark_value()` — mark values as roots
  - `vy_gc_roots_mark()` / `vy_gc_roots_restore()` — LIFO stack frames for root management

- **Native codegen protection** (`compiler/backend/codegen_c.cpp`)
  - Compound expressions with nested index chains now emit `vy_gc_begin_mutation()` / `vy_gc_end_mutation()` guards
  - Prevents GC from collecting intermediates during operations like `doc["choices"][0]["message"]["content"]`

- **Interpreter RAII root tracking** (`compiler/backend/interp*.cpp`)
  - Subexpressions and temporaries are now properly tracked
  - Fixes crashes in list comprehensions, binary operations, and nested assignments

- **Added regression test** `tests/interp/016_gc_stress.vy` — 2000 iterations with deep nesting passes

All 32 tests pass across both backends.

---

## 0.1.1

### GC Runtime Fixes

Fixed critical garbage collection bugs that caused segfaults during high-iteration JSON parsing with nested index access (common in AI/RAG workloads):

- **Added GC root scanner API** (`runtime/src/gc.c`, `runtime/include/vyrt.h`)
  - `vy_gc_set_scanner()` — register dynamic root scanners during collection
  - `vy_gc_mark_value()` — mark values as roots
  - `vy_gc_roots_mark()` / `vy_gc_roots_restore()` — LIFO stack frames for root management

- **Native codegen protection** (`compiler/backend/codegen_c.cpp`)
  - Compound expressions with nested index chains now emit `vy_gc_begin_mutation()` / `vy_gc_end_mutation()` guards
  - Prevents GC from collecting intermediates during operations like `doc["choices"][0]["message"]["content"]`

- **Interpreter RAII root tracking** (`compiler/backend/interp*.cpp`)
  - Subexpressions and temporaries are now properly tracked
  - Fixes crashes in list comprehensions, binary operations, and nested assignments

- **Added regression test** `tests/interp/016_gc_stress.vy` — 2000 iterations with deep nesting passes

All 32 tests pass across both backends.

### New Examples

- **Chatbot** (`examples/chatbot/chatbot.vy`) — Automation-focused chatbot with tool calling, session history, and mock server support
- **Mock server** (`examples/chatbot/mock_server.py`) — Python mock OpenAI API for local testing
- **RAG Pipeline** (`examples/rag/rag_pipeline.vy`) — RAG-style document retrieval and response generation
- **Tool Agent** (`examples/tool_agent/tool_agent.vy`) — Agent with tool-calling capabilities

### Benchmarks

- **AI Pipeline Benchmark** (`benchmarks/bench_ai.vy`) — Measures JSON parse, nested access, context assembly, chunking, hash maps, multi-parse, and string scanning
- **Reference implementations** (`benchmarks/ref_ai.c`, `benchmarks/ref_ai.py`) — C and Python equivalents for comparison
- **Comparison script** (`benchmarks/compare_ai.sh`) — Runs all three implementations and reports results

### Runtime Improvements

- **Fast JSON parser** (`runtime/src/json_fast.c`) — Reduced allocations during JSON string parsing
- **String builder** (`runtime/include/vy_sb.h`) — Reusable buffer for incremental string building
- **Arena allocator** (`runtime/src/arena.c`) — Request-scoped memory with O(1) teardown
- **HTTP connection pooling** (`runtime/src/http_pool.c`) — Reuse connections across requests

### Documentation Updates

- **README.md** — Updated with new features, examples, and benchmarks
- **CHANGELOG.md** — Added 0.1.1 section documenting all changes
- **docs/PERFORMANCE.md** — Updated with AI pipeline benchmarks and GC improvements

---

## 0.1.0

First release.

### Language

- Dynamic values with inference: `Int`, `Float`, `Bool`, `null`, `String`,
  `List`, `Map`, tuples.
- `if` / `else if` / `else`, `while`, `for ... in`, `break`, `continue`.
- `function` declarations with named arguments, and `function(x) { ... }`
  literals.
- String interpolation (`"hi {name}, n={1 + 2}"`); invalid braces stay literal,
  so inline JSON is safe.
- `try` / `catch` with catchable runtime errors, plus `throw` and `assert`.
- List comprehensions, slices, and destructuring.

### Standard library

- **Strings** — `upper` `lower` `trim` `split` `join` `contains` `replace`
  `indexof` `repeat` `startswith` `endswith` `pad` `chars` `bytes`.
- **Lists** — `push` `pop` `insert` `remove` `sort` `reverse` `contains`
  `indexof` `extend` `length`.
- **Maps** — `get` `set` `has` `delete` `keys` `values` `items` `count`
  `merge`.
- **Math** — `abs` `sqrt` `floor` `ceil` `round` `min` `max` `sum` `pow`.
- **JSON** — `json.parse` `json.stringify` `json.valid`.
- **HTTP** — `http.get` `http.post` `http.put` `http.delete`, with `json=` and
  `headers=` named arguments.
- **Environment** — `env` (throws when unset), `env_or` (falls back).
- **Misc** — `type` `str` `int` `float` `bool` `range` `len` `time.clock`
  `time.now` `gc` `input` `exit`.

### Toolchain

- `vyc prog.vy` — compile to a native executable; the runtime is linked in, so
  the result needs nothing but libc.
- `vyc run prog.vy` — tree-walking interpreter for a fast edit/run loop.
- `vyc init <dir>` — scaffold a project (`hello`, `http`, `cli`
  templates) with a `main.vy`, `Makefile`, `README.md` and `.gitignore`.
- `vyc check` / `ast` / `tokens` / `fmt` — diagnostics and canonical
  formatting.
- `--release` (`-O3`, LTO, strip) and `--target=native` (`-march=native`).
- `make install` and `./install.sh` — install `vyc`, `libvyrt.a` and the
  headers into a prefix so `vyc` works from anywhere.

### Runtime

- Tracing garbage collector with `gc()` / `gc("stats")` / `gc("reset")`.
- `http.*` is linked lazily: a program that never makes a request does not
  carry libcurl.
- A compiled `print("hi")` binary is about 18 KB and starts in about 5 ms.

### Known limitations

- Closures do not capture their enclosing scope (rejected at compile time
  rather than miscompiled).
- Values are boxed, so tight numeric loops are much slower than C.
- One file per program — no modules, imports, or file I/O yet.
- No async or concurrency.
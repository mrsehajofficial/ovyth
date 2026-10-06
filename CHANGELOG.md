# Changelog

Notable changes to Vayu. Versions follow [semantic
versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

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

Benchmark results (50k iterations, Vayu native vs Python vs C):
```
case                 Vayu (ms)    C (ms)   Py (ms)        vs C   vs Python
json_parse              1000ms     126ms     636ms        7.9x   1.6x slower
context_build           1000ms     157ms     487ms        6.4x   2.1x slower
hash_map_str               0ms       5ms      14ms      0.0x   0.0x faster
chunk_pipeline             0ms       0ms       4ms         ?   0.0x faster
string_scan                0ms       0ms     314ms         ?   0.0x faster
```

Key improvements in v0.1.1:
- `json.extract(json, path)` — Fast field extraction without full AST build (1.6x vs Python, was 19x)
- Optimized `join()` with string builder — O(n) instead of O(n²) for context assembly (2.1x vs Python, was 6x)

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

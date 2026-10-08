# How fast is Vayu?

The short, honest answer: **Vayu is 2.5x-24x faster than Python on loops, recursion and list building, within 1.2x-19x of Python on AI pipeline operations, and 2.9x-309x slower than C.** It starts in ~5 ms and ships binaries of 18-40 KB.

Every number on this page was measured on this machine (4 cores, Linux x86-64, clang/gcc, Python 3.14) with the scripts in `benchmarks/`. Nothing is estimated, and Rust/Go columns are left out entirely because those compilers are not installed here — an omitted number is better than an invented one.

Reproduce everything yourself:

```bash
bash benchmarks/run.sh        # Vayu's own suite
bash benchmarks/compare.sh    # Vayu vs C vs Python, same algorithms
bash benchmarks/compare_ai.sh # AI pipeline benchmark (JSON, context building, etc.)
```

---

## 1. The headline numbers

`benchmarks/compare.sh` runs the *same algorithm on the same input* in Vayu, C (`-O3 -march=native`), and Python — and **checks that all three compute the identical answer** before it reports a timing. A comparison where the programs compute different things proves nothing.

```
case         Vayu (wall)       C -O3      Python   result check
--------------------------------------------------------------------
intloop             237ms    42.14ms     5612ms   identical, py: identical
fib                   7ms     0.37ms       30ms   identical, py: identical
strconcat            14ms     0.81ms        9ms   identical, py: identical
listappend           34ms     0.11ms       86ms   identical, py: identical
mapops              187ms    64.99ms      156ms   identical, py: identical
```

Measured 7 Oct 2026 (`bash benchmarks/compare.sh`, best-of-3 wall clock;
desktop variance ±20-40% -- `intloop` has measured 193-275ms across recent
runs).

| case | Vayu vs Python | Vayu vs C | what the case measures |
|---|---|---|---|
| `intloop` | **23.7x faster** | 5.6x slower | 20M-iteration arithmetic loop |
| `fib` | **4.3x faster** | 19x slower | recursive calls, `fib(25)` |
| `strconcat` | 1.6x **slower** | 17x slower | 40k string appends |
| `listappend` | **2.5x faster** | 309x slower | 500k `push`es + sum |
| `mapops` | 1.2x slower | 2.9x slower | 100k map inserts + 100k reads |

How to read that honestly:

- **Where Vayu wins against Python** (loops, recursion, list building), it wins because it is compiled native code, not an interpreter.
- **`mapops` is now within 1.2x of Python** (187ms vs 156ms) after the two runtime defects below were fixed; the C margin that remains is the hash table, not the language model.
- **`strconcat` is still 1.6x slower than Python**, and the reason is specific: CPython reuses the buffer for `s += x` and interns short strings, while Vayu still allocates one `VyStr` per append. It went 29ms -> 14ms; finishing it needs small-string interning, not a new backend.
- **The C column is the real scoreboard.** Vayu is now ~5.6x behind on arithmetic loops instead of 19x. What remains is the value representation — see §4.

Note on variance: timings move ±20-40% run to run on a desktop machine (this suite measured `intloop` anywhere from 193ms to 275ms across recent runs, `fib` 4-7ms). Treat the tables as orders of magnitude, not as precise constants.

---

## 2. Vayu's own suite

`benchmarks/run.sh` times each native binary as a *whole process* — startup and runtime init included. Excluding startup would be the kind of benchmark cheating this project refuses to do.

```
case           result
---------------------------------------------
startup        best=    5ms avg=    6ms  binary=   18736 bytes
intloop        best=  275ms avg=  300ms  binary=   31048 bytes
fib            best=    6ms avg=    6ms  binary=   18736 bytes
strconcat      best=   14ms avg=   15ms  binary=   27016 bytes
listappend     best=   34ms avg=   36ms  binary=   39312 bytes
mapops         best=  189ms avg=  192ms  binary=   31056 bytes
json           best=   11ms avg=   11ms  binary=   39264 bytes
chatbot        best=   11ms avg=   11ms  binary=   31040 bytes
```

Measured 7 Oct 2026 (`bash benchmarks/run.sh`, 5 reps best-of). `intloop`
varies 193-275ms run to run (desktop noise, not a regression); its binary
grew 26,952 -> 31,048 bytes and `json` 35,168 -> 39,264 bytes since the
last snapshot because the v0.1.2 compile-time path specialization emits
larger per-site inline blocks.

Two numbers here are genuinely good regardless of what C does:

- **~5 ms cold start.** There is no VM to boot and libcurl is only linked into binaries that actually call `http.*`.
- **18 KB for `print("hi")`.** The runtime is linked normally (not `--whole-archive`) with `-ffunction-sections` + `--gc-sections`, so dead code — JSON, HTTP, half the stdlib — is dropped from binaries that never touch it.

---

## 3. What has already been won

From `benchmarks/RESULTS.md` (measurements, not claims):

| change | before | after |
|---|---|---|
| binary size for `print("hi")` | 254,848 B | **18,736 B** (13.6x smaller) |
| `strconcat` 40k appends | 32,581 ms | **14 ms** (~2,300x faster) |
| `mapops` 100k inserts + reads | infinite hang | **189 ms** |
| `intloop` 20M iterations | 819 ms | **~240-275 ms** (3x, varies run to run) |
| `vy_str_concat` | 2 allocations, 3 copies | **1 allocation, 2 copies** |
| scalar arithmetic (`a + b`) | call into `libvyrt.a` + ~7 tag tests | **inlined in the header**, one tag test |
| `xs.push(x)` | out-of-line method dispatch per push | **tag guard + direct `vy_list_push`** |
| `str(i)` | `snprintf` + render `Buf`, 2 allocations | **digit loop into stack buffer**, 1 allocation |
| hash-map inserts | grew only at 100% load | **grows at 0.7 load**; 100k int keys 176ms -> 89ms |
| GC mark of int-keyed containers | 200k calls per collection | **tag compare**, no calls |
| **JSON field extraction** | full AST build (19x slower than Python) | **fast path extraction** (~1.7x slower; repeated access ~19x -- see §7) |
| **Context assembly (join)** | O(n²) repeated concat (6x slower) | **O(n) string builder** (2.1-4.3x slower, 1s timer variance -- see §7) |
| **unboxed numeric locals** | proof-of-concept | **proven int/float** locals emitted as raw `int64_t`/`double`, no GC registration |
| **specialized arrays** | generic `VyList` for all arrays | `VyInt64Array` / `VyFloat64Array` / `VyStringArray` with contiguous buffers and tag-guarded fast paths |

Writing the benchmarks exposed six real bugs that reading the code did not: a GC that never computed its live set (collector ran on every allocation past 8 MB), heap accounting against the wrong "last allocation" slot, a `[[noreturn]]` parser-error function that actually returned and spun forever, a hash map that only grew when 100% full, `sweep()` subtracting garbage from an already-rebuilt live total, and a GC mark loop paying a function call per primitive element.

---

## 4. Why the gap with C exists

Every Vayu value is currently a **16-byte tagged `VyValue`**, passed by value. Several optimizations have closed the gap:

| consequence | status |
|---|---|
| `a + b` was a **runtime call** into `libvyrt.a` with ~7 tag tests first | **fixed** — the fast path is `static inline` in `vyrt.h` |
| no language-level constant folding; `-O3` could not cross the call boundary | **fixed** — literals fold at emit time |
| per-operation allocation (`str(i)`, string appends, `"key" + str(i)` keys) | **mostly fixed** — stack-buffer rendering, string sharing, pinned literals |
| the compiler does **not specialise int/float**, so no value ever lives in a register | **partially done** — proven int/float locals are emitted as raw `int64_t`/`double` with no VyValue boxing; however, list elements and function parameters still go through the boxed path |

The remaining gap is that values flowing through lists, maps, and function parameters are still boxed. Loop variables that the type-inference pass can prove as int/float now emit as raw locals — that optimization is implemented and deployed, but it is scope-local, and the remaining hot paths still pass through the boxed representation. This is the work tracked under stage P3 in the performance spec.

---

## 5. Where Vayu sits today, in one table

| workload | verdict |
|---|---|
| scripting / automation (I/O, JSON, HTTP) | **fast enough** — bounded by the network, not the language |
| replacing Python scripts | **usually faster** (2.5x-24x on loops/recursion/lists); hash maps within 1.2x, string building 1.6x behind CPython |
| AI pipeline operations | **mixed, see §7** — single extraction ~1.7x, repeated access ~19x, context build 2.1-4.3x vs Python; sub-second cases below the 1s timer |
| startup-sensitive CLI tools | **good** — ~5 ms, 18 KB binaries |
| tight numeric loops | **2.9x-19x behind C on arithmetic/recursion, up to 309x on list churn** — was 19x-29x; the remaining cause is boxed values (P3) |
| string-heavy or map-heavy hot loops | **`mapops` within 1.2x of CPython**, `strconcat` still loses to in-place append |

---

## 6. Benchmark integrity rules

The suite follows the project's performance spec (sections 34–37):

- same algorithm, same input, same output in every language;
- answers are **checked for equality** before a time is reported;
- whole-process wall clock for Vayu, internal timers for C/Python — and the ~5 ms startup difference is disclosed in the output, not hidden;
- the C reference takes its timings in separate statements with volatile loop bounds, so `-O3` cannot constant-fold the work into a fictional `0.00ms`;
- missing toolchains (Rust, Go) produce **omitted columns, never estimates**.

---

## 7. AI Pipeline Benchmarks

Vayu is designed for automation and AI tooling workloads. The `benchmarks/bench_ai.vy` suite measures exactly the operations that dominate AI agent pipelines — not integer arithmetic.

> **Read this section before quoting a number.** Re-checked 7 Oct 2026
> (`bash benchmarks/compare_ai.sh --release`): `bench_ai.vy` reports whole
> seconds, so sub-second cases print `0ms` ("below granularity", not
> "instant") and cases near a second flip 0/1000/2000ms run to run. And the
> three implementations do *different work* per case (Vayu re-extracts from
> the JSON string; Python does dict lookups after one parse; C's
> `json_access` reads back a cached length -- an empty loop). Full analysis
> in `benchmarks/RESULTS_AI.md`. A ms timer plus like-for-like cases with
> checked outputs are open work.

### Benchmark Cases (fresh output, 7 Oct 2026)

```
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

| Case | What it measures | Honest reading (raw runs, reps) |
|------|------------------|----------------------------------|
| `json_parse` (50k extracts) | Extract field from LLM response via path | ~600-1000ms vs Python ~580-600ms → **~1.7x slower** when it reports 1000ms; `0ms` rows are timer artifacts |
| `json_access` (500k extracts) | Repeated nested extraction | 3000ms stable vs ~155-165ms → **~19x slower**; C's ~0ms is a cached-length loop, not a parse |
| `context_build` (100k joins) | Join chunks with numbering (RAG hot path) | 1000-2000ms vs ~465-487ms → **2.1-4.3x slower**; C ~145-160ms |
| `hash_map_str` | Insert + read string-keyed map | below 1s timer on all three -- too fast to quote |
| `chunk_pipeline` | Split document into chunks | below 1s timer -- too fast to quote |
| `multi_parse` (20k parses) | Parse small tool-result JSONs | flips 0-1000ms vs ~152-177ms → **0-6.6x slower**, needs finer timer |
| `string_scan` (1M scans) | Find character in string | below 1s timer in Vayu/C vs ~262-314ms Python -- too fast to quote |

Benchmark iteration counts: 50k (`json_parse`), 500k (`json_access`),
100k (`context_build`), 500 docs, 10k+10k map ops, 20k parses, 1M scans.
Outputs agree across implementations on result length/sums, but the
algorithms differ per language (see caveat above) -- this is *not* the
checked like-for-like comparison that `compare.sh` does.

### Key Optimizations

**`json.extract(json, path)`** — Fast field extraction without building full AST:
```vayu
// Extract nested field without parsing entire JSON tree
content = json.extract(response, "choices[0].message.content")
```
- Path syntax: dot notation with array indices (`key` or `array[index]`)
- Recursive support for nested objects and arrays
- Parses only what's needed, skipping irrelevant parts
- v0.1.2: `memcmp` key matching with no per-key `VyStr` allocation, plus
  compile-time path specialization for string literals
- Result: single extraction **~1.7x slower than Python**; `json_access`
  (500k repeated extracts) 5000ms -> 3000ms, still **~19x slower**

**Optimized `join()` with string builder**:
- Changed from O(n²) repeated concatenation to O(n) using `VyStrBuilder`
- Avoids per-element allocations in hot loops
- Result: context assembly **2.1-4.3x slower than Python** (was 6x slower);
  exact figure varies run to run under the 1s timer -- needs a ms timer to
  pin down. The earlier "1000ms -> 0ms" reading was a quantization
  artifact, corrected on re-check.

### How to Run

```bash
# Run Vayu benchmark directly
./build/vyc benchmarks/bench_ai.vy --release -o /tmp/bench_ai && /tmp/bench_ai

# Compare all three implementations
bash benchmarks/compare_ai.sh --release
```

### Context

These benchmarks measure the AI orchestration layer (JSON parsing, context assembly, chunking, map operations) — not LLM inference. LLM inference dominates at 100ms-10s; we optimize the 1-50ms overhead.

For real AI workloads where LLM calls dominate, Vayu's orchestration overhead is negligible. The goal is to make Vayu fast enough that it doesn't add perceptible latency to your pipeline.

---

## 8. Known Limitations

- **Partial unboxing**: Only variables proven as int/float at the type level are emitted as raw `int64_t`/`double`. Values flowing through lists, maps, and function parameters remain boxed, so hot paths that iterate over collections still pay the boxing cost.
- **GC overhead**: Every allocation goes through the garbage collector, adding ~1-2μs per object
- **No escape analysis**: Cannot prove temporaries don't escape, so must GC-track everything
- **String interning**: Short strings not interned like CPython, causing extra allocations

The partial unboxing work (§4) is the first step toward full specialization — closing the remaining gap needs the type-inference pass to reach every value, not just locals. For now, Vayu targets scripting/automation workloads where the overhead is acceptable compared to the benefits of compiled native execution.

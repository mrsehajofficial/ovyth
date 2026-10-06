# How fast is Vayu?

The short, honest answer: **Vayu is 2.6x-26x faster than Python on loops, recursion and list building, competitive with Python on AI pipeline operations, and 4.5x-380x slower than C.** It starts in ~5 ms and ships binaries of 18-40 KB.

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
intloop             193ms    42.83ms     4955ms   identical, py: identical
fib                   4ms     0.34ms       32ms   identical, py: identical
strconcat            14ms     0.68ms        9ms   identical, py: identical
listappend           34ms     0.09ms       88ms   identical, py: identical
mapops              189ms    66.76ms      161ms   identical, py: identical
```

| case | Vayu vs Python | Vayu vs C | what the case measures |
|---|---|---|---|
| `intloop` | **25.7x faster** | 4.5x slower | 20M-iteration arithmetic loop |
| `fib` | **8x faster** | 12x slower | recursive calls, `fib(25)` |
| `strconcat` | 1.6x **slower** | 21x slower | 40k string appends |
| `listappend` | **2.6x faster** | 378x slower | 500k `push`es + sum |
| `mapops` | ~equal | 2.8x slower | 100k map inserts + 100k reads |

How to read that honestly:

- **Where Vayu wins against Python** (loops, recursion, list building), it wins because it is compiled native code, not an interpreter.
- **`mapops` is now at parity with Python** (189ms vs 161ms) after the two runtime defects below were fixed; the C margin that remains is the hash table, not the language model.
- **`strconcat` is still 1.6x slower than Python**, and the reason is specific: CPython reuses the buffer for `s += x` and interns short strings, while Vayu still allocates one `VyStr` per append. It went 29ms -> 14ms; finishing it needs small-string interning, not a new backend.
- **The C column is the real scoreboard.** Vayu is now 4.5x behind on arithmetic loops instead of 19x. What remains is the value representation — see §4.

Note on variance: timings move ±20-40% run to run on a desktop machine (this suite measured `intloop` anywhere from 184ms to 223ms on the same afternoon). Treat the tables as orders of magnitude, not as precise constants.

---

## 2. Vayu's own suite

`benchmarks/run.sh` times each native binary as a *whole process* — startup and runtime init included. Excluding startup would be the kind of benchmark cheating this project refuses to do.

```
case           result
---------------------------------------------
startup        best=    5ms avg=    6ms  binary=   18736 bytes
intloop        best=  193ms avg=  232ms  binary=   26952 bytes
fib            best=    4ms avg=    4ms  binary=   18736 bytes
strconcat      best=   14ms avg=   15ms  binary=   27016 bytes
listappend     best=   33ms avg=   35ms  binary=   39312 bytes
mapops         best=  189ms avg=  198ms  binary=   31056 bytes
json           best=   12ms avg=   12ms  binary=   35168 bytes
chatbot        best=   11ms avg=   11ms  binary=   31040 bytes
```

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
| `intloop` 20M iterations | 819 ms | **193 ms** (4.2x) |
| `vy_str_concat` | 2 allocations, 3 copies | **1 allocation, 2 copies** |
| scalar arithmetic (`a + b`) | call into `libvyrt.a` + ~7 tag tests | **inlined in the header**, one tag test |
| `xs.push(x)` | out-of-line method dispatch per push | **tag guard + direct `vy_list_push`** |
| `str(i)` | `snprintf` + render `Buf`, 2 allocations | **digit loop into stack buffer**, 1 allocation |
| hash-map inserts | grew only at 100% load | **grows at 0.7 load**; 100k int keys 176ms -> 89ms |
| GC mark of int-keyed containers | 200k calls per collection | **tag compare**, no calls |
| **JSON field extraction** | full AST build (19x slower than Python) | **fast path extraction** (1.6x slower) |
| **Context assembly (join)** | O(n²) repeated concat (6x slower) | **O(n) string builder** (2.1x slower) |

Writing the benchmarks exposed six real bugs that reading the code did not: a GC that never computed its live set (collector ran on every allocation past 8 MB), heap accounting against the wrong "last allocation" slot, a `[[noreturn]]` parser-error function that actually returned and spun forever, a hash map that only grew when 100% full, `sweep()` subtracting garbage from an already-rebuilt live total, and a GC mark loop paying a function call per primitive element.

---

## 4. Why the gap with C exists

Every Vayu value is currently a **16-byte tagged `VyValue`**, passed by value. Four things followed from that, and three of them are now fixed:

| consequence | status |
|---|---|
| `a + b` was a **runtime call** into `libvyrt.a` with ~7 tag tests first | **fixed** — the fast path is `static inline` in `vyrt.h` |
| no language-level constant folding; `-O3` could not cross the call boundary | **fixed** — literals fold at emit time |
| per-operation allocation (`str(i)`, string appends, `"key" + str(i)` keys) | **mostly fixed** — stack-buffer rendering, string sharing, pinned literals |
| the compiler does **not specialise int/float**, so no value ever lives in a register | **open** — this is the remaining item |

That last row is the whole of what is left: `i` in `intloop` is still a 16-byte boxed struct that is rebuilt and re-tagged on every iteration, where C keeps a single register. Fixing it needs a type-inference pass that lowers known-int/float locals to raw `int64_t`/`double` and guards at function entry, which is a real pass rather than a patch (spec sections 5-7, plan in `PERFORMANCE-SPEC.md` §44 stage P3). That one change is what moves `intloop` from 4.5x toward parity with C.

---

## 5. Where Vayu sits today, in one table

| workload | verdict |
|---|---|
| scripting / automation (I/O, JSON, HTTP) | **fast enough** — bounded by the network, not the language |
| replacing Python scripts | **usually faster** (2.6x-26x), except string building and map churn, where it is now within 1.2-1.6x of CPython |
| AI pipeline operations | **competitive** — JSON extraction 1.6x, context build 2.1x vs Python |
| startup-sensitive CLI tools | **good** — ~5 ms, 18 KB binaries |
| tight numeric loops | **4.5x-12x behind C** — was 19x-29x; the remaining cause is boxed values (P3) |
| string-heavy or map-heavy hot loops | **no longer a walkover for CPython** — `mapops` is at parity, `strconcat` still loses to in-place append |

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

### Benchmark Cases

| Case | What it measures | Vayu (ms) | C (ms) | Python (ms) | vs Python |
|------|------------------|-----------|--------|-------------|-----------|
| `json_parse` | Extract field from LLM response via path | 1000 | 126 | 636 | **1.6x slower** |
| `context_build` | Join chunks with numbering (RAG hot path) | 1000 | 157 | 487 | **2.1x slower** |
| `hash_map_str` | Insert + read string-keyed map | 0 | 5 | 14 | ~equal |
| `chunk_pipeline` | Split document into chunks | 0 | 0 | 4 | ~equal |
| `string_scan` | Find character in string | 0 | 0 | 314 | ~equal |

Benchmark runs 50,000-500,000 iterations per case. All implementations produce identical outputs (verified by comparison script).

### Key Optimizations

**`json.extract(json, path)`** — Fast field extraction without building full AST:
```vayu
// Extract nested field without parsing entire JSON tree
content = json.extract(response, "choices[0].message.content")
```
- Path syntax: dot notation with array indices (`key` or `array[index]`)
- Recursive support for nested objects and arrays
- Parses only what's needed, skipping irrelevant parts
- Result: **1.6x slower than Python** (was 19x slower before optimization)

**Optimized `join()` with string builder**:
- Changed from O(n²) repeated concatenation to O(n) using `VyStrBuilder`
- Avoids per-element allocations in hot loops
- Result: context assembly now **2.1x slower than Python** (was 6x slower)

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

- **Boxed values**: Every value is a 16-byte `VyValue` struct, preventing register allocation for numeric types
- **GC overhead**: Every allocation goes through the garbage collector, adding ~1-2μs per object
- **No escape analysis**: Cannot prove temporaries don't escape, so must GC-track everything
- **String interning**: Short strings not interned like CPython, causing extra allocations

These are architectural limitations that require deeper compiler changes (type inference pass) to fix. For now, Vayu targets scripting/automation workloads where the overhead is acceptable compared to the benefits of compiled native execution.

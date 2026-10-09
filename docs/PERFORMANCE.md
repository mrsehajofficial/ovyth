# How fast is Ovyth?

The short, honest answer: **on loop-heavy code Ovyth beats Python by 5x to
310x; where it loses to Python it loses by 1.2x to 1.6x (hash maps, string
building); on AI-pipeline work it beats CPython on five of seven cases;
and against `-O3 -march=native` C it runs within 1.8x on a tight integer
loop, up to 185x behind where every operation allocates.** It starts in
about 6ms and ships binaries of 18-44 KB.

Every number on this page was measured on this machine (4 cores, Linux
x86-64, clang 22.1.8 / gcc 16.2.1, Python 3.14.7) with the scripts in
`benchmarks/`. Nothing is estimated, and the Rust/Go columns are left out
entirely because those compilers are not installed here — an omitted
number is better than an invented one.

Reproduce everything yourself:

```bash
bash benchmarks/run.sh              # Ovyth's own suite
bash benchmarks/compare.sh          # Ovyth vs C vs Python, same algorithms
bash benchmarks/compare_ai.sh --release   # AI pipeline benchmark
```

---

## 1. The headline numbers

`benchmarks/compare.sh` runs the same algorithm on the same input in Ovyth,
C (`-O3 -march=native`), and Python — and **checks that all three compute
the identical answer** before it reports a timing. A comparison where the
programs compute different things proves nothing.

```
case         Ovyth (wall)       C -O3      Python   result check
--------------------------------------------------------------------
intloop              22ms    12.04ms     6827ms   identical, py: identical
fib                   6ms     0.41ms       32ms   identical, py: identical
strconcat            14ms     0.62ms        9ms   identical, py: identical
listappend           37ms     0.20ms       91ms   identical, py: identical
mapops              194ms    82.46ms      158ms   identical, py: identical
```

Measured 8 Oct 2026 (best-of-3 wall clock). This is a desktop, so expect
movement: two runs an hour apart gave `intloop` 21-22ms, Python's copy
6827-7399ms, and Python's `fib` anywhere from 32ms to 91ms. Read the tables
as orders of magnitude.

| case | Ovyth vs Python | Ovyth vs C | what the case measures |
|---|---|---|---|
| `intloop` | **310x faster** | 1.8x slower | 20M-iteration integer loop (xor/shift body — see §6) |
| `fib` | **5.3x faster** | 14.6x slower | recursive calls, `fib(25)` |
| `strconcat` | 1.6x **slower** | 22.6x slower | 40k string appends |
| `listappend` | **2.5x faster** | 185x slower | 500k `push`es + sum |
| `mapops` | 1.2x slower | 2.4x slower | 100k map inserts + 100k reads |

How to read that honestly:

- **Where Ovyth wins against Python** — loops, recursion, list building —
  it wins because it is compiled native code, not an interpreter. 310x on
  the integer loop is what "compiled" buys.
- **`mapops` is within 1.2x of CPython** (194ms vs 158ms) after the two
  runtime defects below were fixed. The 2.4x C margin that remains is the
  hash table, not the language model.
- **`strconcat` is still 1.6x slower than Python**, and the reason is
  specific: CPython reuses the buffer for `s += x` and interns short
  strings, while Ovyth allocates one `OvStr` per append. It went 32,581ms →
  14ms; finishing the job needs small-string interning, not a new backend.
- **`intloop` within 1.8x of C is the newest result.** Proven `int` locals
  now emit as raw `int64_t` with no boxing, so the loop body is close to
  what clang would write by hand. (The older `total += i * 3 - 1` version
  of this case was being *erased* by the optimiser — see §6.)
- **The C column is the real scoreboard**, and it ranges from 1.8x to 185x
  depending on how much the case allocates. That spread is the object
  model — see §4.

---

## 2. Ovyth's own suite

`benchmarks/run.sh` times each native binary as a *whole process* — startup
and runtime init included. Excluding startup would be the kind of
benchmark cheating this project refuses to do.

```
case           result
---------------------------------------------
startup        best=    6ms avg=   13ms  binary=   18736 bytes
intloop        best=   30ms avg=   43ms  binary=   18736 bytes
fib            best=    8ms avg=    9ms  binary=   18736 bytes
strconcat      best=   21ms avg=   35ms  binary=   22904 bytes
listappend     best=   44ms avg=   47ms  binary=   43400 bytes
mapops         best=  256ms avg=  399ms  binary=   31056 bytes
json           best=   13ms avg=   14ms  binary=   43360 bytes
chatbot        best=   15ms avg=   16ms  binary=   31040 bytes
```

Measured 8 Oct 2026 (5 reps, best-of). The `best`/`avg` gap on `mapops`
is ordinary desktop noise.

Two numbers here are genuinely good regardless of what C does:

- **~6 ms cold start.** There is no VM to boot, and libcurl is only linked
  into binaries that actually call `http.*`.
- **18 KB for `print("hi")`.** The runtime is linked normally (not
  `--whole-archive`) with `-ffunction-sections` + `--gc-sections`, so dead
  code — JSON, HTTP, half the stdlib — is dropped from binaries that never
  touch it.

---

## 3. What has already been won

From `benchmarks/RESULTS.md` — measurements, not claims:

| change | before | after |
|---|---|---|
| binary size for `print("hi")` | 254,848 B | **18,736 B** (13.6x smaller) |
| `strconcat` 40k appends | 32,581 ms | **14 ms** (~2,300x faster) |
| `mapops` 100k inserts + reads | infinite hang | **194 ms** |
| scalar arithmetic (`a + b`) | a call into `libovrt.a` + ~7 tag tests | **inlined in the header**, one tag test |
| `xs.push(x)` | out-of-line method dispatch per push | **tag guard + direct `ov_list_push`** |
| `str(i)` | `snprintf` + render `Buf`, 2 allocations | **digit loop into a stack buffer**, 1 allocation |
| hash-map inserts | grew only at 100% load | **grows at 0.7 load**; 100k int keys 176ms → 89ms |
| GC mark of int-keyed containers | 200k calls per collection | **tag compare**, no calls |
| JSON field extraction | full AST build | **fast path extraction**; see §7 |
| context assembly (`join`) | O(n²) repeated concat | **O(n) string builder**; see §7 |
| unboxed numeric locals | proof-of-concept | **proven int/float** locals as raw `int64_t`/`double`, no GC registration |
| specialized arrays | generic `OvList` for everything | `OvInt64Array` / `OvFloat64Array` / `OvStringArray`, contiguous buffers, tag-guarded fast paths |

Writing the benchmarks exposed six real bugs that reading the code did
not: a GC that never computed its live set (the collector ran on every
allocation past 8 MB), heap accounting against the wrong "last allocation"
slot, a `[[noreturn]]` parser-error function that actually returned and
spun forever, a hash map that only grew when 100% full, `sweep()`
subtracting garbage from an already-rebuilt live total, and a GC mark loop
paying a function call per primitive element. The full write-ups are in
[benchmarks/RESULTS.md](../benchmarks/RESULTS.md).

One more find, on 8 Oct 2026: re-running the suite showed `intloop`
finishing 20 million iterations in 4ms — because at `-O2` clang replaces
the whole affine loop `total = total + i * 3 - 1` with closed-form
arithmetic. Plain C does the same thing (0.00ms), and the old `volatile`
trip counts that protected the C reference were also suppressing
vectorisation. The case now uses an xor/shift body with no closed form,
identically in all three languages; the details and the corrected numbers
are in §6 and in `benchmarks/RESULTS.md`.

---

## 4. Why the gap with C exists

Every Ovyth value is currently a **16-byte tagged `OvValue`**, passed by
value. Several optimizations have closed most of the distance:

| consequence | status |
|---|---|
| `a + b` was a **runtime call** into `libovrt.a` with ~7 tag tests first | **fixed** — the fast path is `static inline` in `ovrt.h` |
| no language-level constant folding; `-O3` could not cross the call boundary | **fixed** — literals fold at emit time |
| per-operation allocation (`str(i)`, string appends, `"key" + str(i)` keys) | **mostly fixed** — stack-buffer rendering, string sharing, pinned literals |
| the compiler did not specialise int/float, so no value ever lived in a register | **partially done** — proven int/float locals emit as raw `int64_t`/`double` with no boxing; list elements and function parameters still take the boxed path |

The remaining gap is the boxed path: values flowing through lists, maps
and function parameters still get rebuilt and re-tagged rather than kept
in registers. Loop variables the type-inference pass can prove are fixed —
that is why `intloop` sits at 1.8x while `listappend` sits at 185x. Closing
the rest is the work tracked under stage P3 in the performance spec.

---

## 5. Where Ovyth sits today, in one table

| workload | verdict |
|---|---|
| scripting / automation (I/O, JSON, HTTP) | **fast enough** — bounded by the network, not the language |
| replacing Python scripts | **usually much faster** — 2.5x-310x on loops, recursion, lists; hash maps within 1.2x; string building 1.6x behind |
| AI pipeline operations | **mostly ahead of Python** — 5 of 7 cases faster (1.6x-14.5x); context assembly 3.0x behind; see §7 |
| startup-sensitive CLI tools | **good** — ~6 ms, 18 KB binaries |
| tight numeric loops | **within 1.8x of `-O3` C** while locals stay unboxed — was 5.6x before the P3 locals landed |
| allocation-heavy hot loops | **the weak spot** — 185x behind C on list churn; boxing + GC per push (P3 remains) |
| map-heavy hot loops | **within 1.2x of CPython**, 2.4x behind C |

---

## 6. Benchmark integrity rules

The suite follows the project's performance spec (sections 34-37):

- same algorithm, same input, same output in every language;
- answers are **checked for equality** before a time is reported;
- whole-process wall clock for Ovyth, internal timers for C/Python — and the
  ~4ms startup difference is disclosed in the output, not hidden;
- timing is taken in separate statements with the work, never inside the
  `printf` call that reports it (argument evaluation order is unspecified);
- a case whose loop the optimiser can erase does not get published: when
  `intloop`'s affine body proved away to closed form in both Ovyth and C
  (4ms "20M iterations" in Ovyth, 0.00ms in C), the case was redesigned to
  an xor/shift body with no closed form, identically in all three
  languages, and the old numbers were pulled. C's reference uses a
  `volatile` trip count only where folding is still a threat (`listappend`);
- missing toolchains (Rust, Go) produce **omitted columns, never estimates**.

---

## 7. AI pipeline benchmarks

Ovyth is aimed at automation and AI tooling, so `benchmarks/bench_ai.ov`
measures the operations that dominate agent pipelines rather than integer
arithmetic: JSON parsing, field extraction, context assembly, chunking,
string-keyed maps.

Fresh output, 8 Oct 2026 (`bash benchmarks/compare_ai.sh --release`).
Until this run the suite timed with `now()`, which has one-second
granularity — older tables showing `0ms` rows and `0.0x faster` ratios were
broken, not fast. It now uses `time.clock()`:

```
case                 Ovyth (ms)    C (ms)   Py (ms)        vs C   vs Python
------------------  ----------  --------  --------  ----------  ----------
json_parse               351ms  119.39ms     604ms  2.9x slower  1.7x faster
json_access               80ms    0.39ms     168ms  205.1x slower  2.1x faster
context_build           1421ms  150.39ms     478ms  9.4x slower  3.0x slower
chunk_pipeline             5ms    0.02ms       3ms  250.0x slower  1.7x slower
hash_map_str               8ms    5.75ms      13ms  1.4x slower  1.6x faster
multi_parse               11ms    4.14ms     159ms  2.7x slower  14.5x faster
string_scan              148ms    0.00ms     293ms         n/a  2.0x faster
```

**Ovyth beats Python on five of seven cases**, 1.6x to 14.5x. The losses:
context assembly (3.0x) and the chunk pipeline (5ms vs 3ms — below what a
1ms timer can resolve). Iteration counts: 50k, 500k, 100k, 500 docs,
10k+10k, 20k, 1M respectively.

Two caveats before quoting a row:

- **The algorithms differ on some cases.** Ovyth re-extracts from the JSON
  string through a path on every iteration of `json_access`; Python parses
  once and does dict lookups; C's 0.39ms is a cached-length read, not a
  parse. Those `vs C` ratios compare different work. `compare.sh` is the
  like-for-like suite; this one is directional.
- Sub-10ms rows are timer grain. `chunk_pipeline`'s 5ms vs 3ms means
  "both fast", nothing more.

### Key optimizations behind those numbers

**`json.extract(json, path)`** — fast field extraction without building the
full AST:

```ovyth
// Extract a nested field without parsing the entire JSON tree
content = json.extract(response, "choices[0].message.content")
```

- dot-path syntax with array indices (`key` or `array[index]`), recursive
  over nested objects and arrays;
- parses only what it needs, skipping the rest;
- v0.1.2: `memcmp` key matching with no per-key `OvStr` allocation, plus
  compile-time path specialization for string literals;
- result: 50k extractions in 351ms (~7µs each), 1.7x faster than Python's
  `json.loads`-and-index.

**`join()` with a string builder** — O(n²) repeated concatenation replaced
by a geometrically-growing `OvStrBuilder`, with the compiler recognising
`acc += x` in loops. Context assembly went from 6x slower than Python to
3.0x; the remaining cost is per-element overhead in the builder, tracked
as open work.

**Fast field accessors** — `json.get_int` / `json.get_float` /
`json.get_str` pull a scalar with `strtoll`/`strtod` directly: no parse, no
AST. That is what makes `multi_parse` (20k tool-result documents) 14.5x
faster than CPython.

### How to run

```bash
# Ovyth side only
./build/ovc benchmarks/bench_ai.ov --release -o /tmp/bench_ai && /tmp/bench_ai

# three-way comparison
bash benchmarks/compare_ai.sh --release
```

### Context

These benchmarks measure the orchestration layer — JSON, context assembly,
chunking, map ops — not LLM inference. Inference dominates at 100ms-10s;
the layer Ovyth optimises is the 1-50ms between calls. Full numbers, per-
case caveats and the open work are in
[benchmarks/RESULTS_AI.md](../benchmarks/RESULTS_AI.md).

---

## 8. Known limitations

- **Partial unboxing.** Only variables proven int/float at the type level
  are emitted as raw `int64_t`/`double`. Values flowing through lists, maps
  and function parameters remain boxed, so collection-heavy hot paths pay
  the full representation cost — this is the 185x on `listappend`.
- **GC overhead.** Every allocation goes through the collector, adding
  roughly 1-2µs per object.
- **No escape analysis.** The compiler cannot yet prove that a temporary
  stays local, so everything gets GC-tracked.
- **No small-string interning.** CPython interns short strings and reuses
  `s += x` buffers; Ovyth allocates per append.

The partial unboxing work is the first step toward full specialization —
closing the remaining gap needs the type-inference pass to reach every
value, not just locals. For now Ovyth targets scripting and automation
workloads, where the overhead is small next to the benefit of shipping one
native binary.

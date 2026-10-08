# Vayu AI pipeline benchmarks

These measure the orchestration layer around an LLM: JSON parsing, context
assembly, chunking, string-keyed maps. Not inference. A model call costs
100ms-10s on its own; the work Vayu can actually change is the tens of
milliseconds of plumbing between calls. If your pipeline is dominated by
model time, nothing here will move your latency.

Machine: 4 cores, Linux x86-64, clang 22.1.8, Python 3.14.7. Measured
8 Oct 2026 with `bash benchmarks/compare_ai.sh --release`.

## Results

```
case                 Vayu (ms)    C (ms)   Py (ms)        vs C   vs Python
------------------  ----------  --------  --------  ----------  ----------
json_parse               351ms  119.39ms     604ms  2.9x slower  1.7x faster
json_access               80ms    0.39ms     168ms  205.1x slower  2.1x faster
context_build           1421ms  150.39ms     478ms  9.4x slower  3.0x slower
chunk_pipeline             5ms    0.02ms       3ms  250.0x slower  1.7x slower
hash_map_str               8ms    5.75ms      13ms  1.4x slower  1.6x faster
multi_parse               11ms    4.14ms     159ms  2.7x slower  14.5x faster
string_scan              148ms    0.00ms     293ms         n/a  2.0x faster
```

All three sides time each case internally with their own clocks, so
startup is excluded everywhere. C is built `clang -O3 -march=native`;
Vayu is built `--release`.

**Vayu beats Python on five of the seven cases**, by 1.6x to 14.5x. The two
losses: context assembly (1421ms vs 478ms, 3.0x) and the chunk pipeline
(5ms vs 3ms -- below the point where a 1ms timer says anything useful).

## Caveats worth knowing before you quote anything

* Rows under ~10ms are timer grain: Vayu rounds to whole milliseconds, so
  `chunk_pipeline`'s 5ms vs 3ms is noise, not a measurement.
* The three implementations do **different work** on some cases. Vayu's
  `json_access` re-extracts from the raw JSON string through a path on every
  iteration; Python parses once and does dict lookups; C's 0.39ms is a
  cached-length read, not a parse at all. The `vs C` ratios on that row
  compare different algorithms. `compare.sh` is the like-for-like suite;
  this one is directional.
* Outputs agree across implementations on lengths and sums, but they are
  not the formally checked equality that `compare.sh` performs.
* Iteration counts: 50k (`json_parse`), 500k (`json_access`), 100k
  (`context_build`), 500 documents (`chunk_pipeline`), 10k inserts + 10k
  reads (`hash_map_str`), 20k parses (`multi_parse`), 1M scans
  (`string_scan`).

### A note on the timer

Until 8 Oct 2026 this suite called `now()`, which returns epoch **seconds**
as an Int -- every reading was quantised to 1000ms steps, so sub-second
cases printed `0ms` and half the table was nonsense (`0.0x faster`). The
suite now uses `time.clock()`, the monotonic microsecond float, and C prints
two decimals. If you see older RESULTS_AI output quoting `0ms` rows or
whole-second timings, that is the bug it came from.

## What has been optimized

| change | what it does | where it landed |
|---|---|---|
| `json.extract(json, path)` | pull a nested field without building the full AST — `"choices[0].message.content"` syntax, `memcmp` key matching, no per-key `VyStr` allocation, compile-time path specialization for literal paths | v0.1.2 |
| `join()` string builder | geometric growth instead of O(n²) re-concatenation; the compiler recognises `acc += x` in loops and lowers it to the builder | v0.1.3 |
| fast field accessors | `json.get_int` / `json.get_float` / `json.get_str` extract a scalar with `strtoll`/`strtod` directly — no parse, no AST | v0.1.3 |
| unboxed numeric locals (P3) | proven int/float locals emit as raw `int64_t`/`double` — no boxing, no GC registration | partially landed |
| specialized array types | `VyInt64Array` / `VyFloat64Array` / `VyStringArray` with contiguous buffers and 2x geometric growth | landed |
| append fast path | inline `vy_list_push` when capacity is available | landed |

Where that leaves the suite: about 7µs per path extraction (351ms / 50k),
0.55µs per small tool-document parse (11ms / 20k — 14.5x faster than
CPython's `json.loads`), and 20k hash-map inserts plus reads in 8ms.

## Remaining work

1. **Context assembly is the real loss** (3.0x vs Python). The builder is
   O(n) now; what is left is per-element overhead inside the builder — the
   fusion work tracked in the performance spec.
2. **Array specialization in codegen** — lower proven `Array<Int>` onto
   `VyInt64Array` everywhere, not just literal-initialized lists.
3. **Escape analysis** — stack-allocate short-lived temporaries instead of
   GC-tracking them.
4. **SIMD** over the specialized arrays, once (2) lands.

## How to run

```bash
bash benchmarks/compare_ai.sh --release          # full three-way comparison
./build/vyc benchmarks/bench_ai.vy --release -o /tmp/bench_ai && /tmp/bench_ai
```

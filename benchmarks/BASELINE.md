# Ovyth performance baseline

Measured 12 Oct 2026 on this machine: Intel i7-8665U @ 1.90GHz, 4 cores,
Linux 7.2.3, clang 22.1.8, CPU features `fma avx2 bmi2`.

Reproduce every number below:

```bash
make && make test            # correctness first
bash benchmarks/profile.sh   # the four-dimension table, this file
bash benchmarks/compare.sh   # the Ovyth vs C vs Python table
```

`profile.sh` reports four things per case because "how fast?" is the wrong
first question. The four:

| column | what it is |
|---|---|
| `median` / `best` | whole-process wall clock, 5 reps, **startup included** |
| `rss KB` | peak resident set, measured by a `fork`+`getrusage` wrapper outside the process |
| `allocs` `gc` `resize` | from `--stats`: objects allocated, collections run, collection growths |
| `binary` | bytes in the stripped executable |

Correctness gates the table: a case whose output is not stable across three
runs is reported as `UNSTABLE OUTPUT` rather than as a number. `compare.sh`
refuses to print a timing at all unless Ovyth's answer equals C's.

---

## 1. The baseline table

```
case           median     best    rss KB   allocs     gc resize    binary
--------------------------------------------------------------------------------------------
startup            6ms       6ms      2276        1      2      0     22912
intloop            32ms      32ms      2184        1      2      0     22912
fib                10ms      10ms      2084        1      2      0     22912
strconcat          20ms      20ms      6988        4      2      0     27016
listappend         45ms      44ms     14448        3      3     17     43488
mapops            238ms     236ms     36256  400005      4      0     35232
json               32ms      21ms      6060       90      2      0     43440
chatbot            16ms      16ms      6032       12      2      0     31120
```

Process floor: an empty program is **5ms** and one allocation. Everything
above is therefore case work plus ~5ms of start.

## 2. Against C, for the same algorithms

| case | Ovyth | C -O3 | ratio |
|---|---|---|---|
| intloop | 27ms | 16.45ms | **1.6x** |
| mapops | 234ms | 94.09ms | **2.5x** |
| fib | 9ms | 0.50ms | 18x |
| strconcat | 20ms | 0.80ms | 25x |
| listappend | 46ms | 0.25ms | 184x |

C and Python time their cases internally; Ovyth's number is the whole
process, so it additionally carries ~5ms of start. That explains part of the
small cases and none of the large ratios.

## 3. What the measurements say, before changing anything

**The map is not the problem.** `mapops` looks like an outlier against the
other Ovyth cases (238ms, 5x the next slowest), so that was the first place
to look — and it is the one case that is already healthy: 2.5x of C, which is
within reach of a boxed dynamic value. Its 400,005 allocations turned out to
be the *benchmark's own string building*, not the map:

```
m[i] = i          100k int-keyed inserts + lookups     90ms    3 allocs
k = "key"+str(i) 100k string keys                     169ms  200003 allocs
"key"+str(i)      string building alone, no map        84ms  200002 allocs
```

So of mapops' 238ms, roughly 168ms is `"key" + str(i)` and ~70ms is string
hashing. `ov_str_concat` is already a single allocation built in place, and
the map already grows geometrically at a 0.7 load factor. Nothing to fix
here yet.

**The list append path is the real gap.** 184x of C. Split by measurement:

```
process floor                                            5ms
500k pushes alone                                      35ms   (~70ns each)
+ sum(xs) over the same list                            +13ms
```

Each push is a box (`ov_int`, 16 bytes), a tag check and a bounds-checked
store — about 10 instructions, which should not cost 70ns. The rest is the
collector: `next_gc = live*2 + 8MB` collects three times over a growing 8MB
list, and each collection marks every element.

**`range` shows the fix already exists.** `for x in range(500000)` with a sum
runs in **21ms with 2 allocations and 90 bytes of heap** — the codegen
specialises it into a native C loop and never builds a collection. Compared
with 45ms and 3 GC cycles for the same work done with `push`, the cost is the
boxed representation, not the loop.

**Strings and JSON are healthy for this workload.** `strconcat` is 20ms for
200k concatenations (one allocation each, which is the semantics of immutable
strings, not waste) and `json` is 16ms with 90 allocations.

## 4. P1: int collections — done, and measured

`xs = []` followed only by `xs.push(<int>)` now becomes an `Int64Array`
instead of a boxed list. The evidence that motivated it: `range`-based
iteration is 2x faster with zero allocations because it skips the boxed
representation, while `xs.push(i)` materialises 500k boxed values.

```
case        before                    after
--------------------------------------------------------------------
listappend  41ms / 15076 KB RSS      24ms /  9316 KB RSS
            17 list resizes          17 i64a resizes
vs C        184x                      82x
```

41% faster, 37% less memory, half the distance to C — from removing the box
per element and the collector's scan of the elements, not from touching the
loop. `allocs` stayed at 3 and GC cycles dropped 3 -> 2.

### The representation must not leak

Specialized arrays were "NOT GC-tracked; must be freed explicitly" — which
made them unsafe for a collection that escapes its function (`return xs`
returned a freed array). They now carry an `OvHeader` like every other
object and are collected, so an array stays reachable as long as anything
points at it.

Leaks found and fixed on the way, each one a place the representation could
be observed:

- `type()` printed `?` for a specialized array (the native backend emits a
  different tag than the interpreter's list).
- `json.stringify()` fell through its switch and produced garbage.
- `sum()` returned 0, because it only handled `OV_LIST`.
- `pop()` on an empty array returned `0` where the interpreter returns `nil`.
- A `push` with a non-int argument was truncated into the array; sema now
  retires the collection on the first such push, so it stays an ordinary
  boxed list with the value's type intact.

Flow-insensitivity was the load-bearing part: a `i = 2.5` *after* a `push(i)`
in source but *before* it on a loop's back edge must still retire the
collection, so a pre-pass scans the whole body before checking it.

## 5. Where the next change should go

By the measurements, in order:

1. **P1, function boundaries (not done).** An int local is unboxed inside one
   function, but crossing a call boxes again. `fib` at 18x of C is where that
   shows up — ~4ns per call is already good, but the argument and return are
   boxed on every hop.
2. **String-heavy loops (P3).** Only once a benchmark whose bottleneck is
   *confirmed* to be the runtime rather than the algorithm exists.
3. **The `sum()` walk.** It costs 13ms over 500k elements even with the array
   in hand, because it boxes each element to test its type.
2. **GC scan cost for large collections (P2).** Three collections over a
   growing 8MB set is O(live) per cycle. Measured next, only if (1) does not
   already remove most of it — a specialized int array has no pointers to
   scan, so (1) may make this moot.
3. **String-heavy loops (P3).** Only after a case whose bottleneck is
   *confirmed* to be the runtime rather than the algorithm. Today's string
   numbers are dominated by a benchmark that builds 200k throwaway keys.

Not on the list yet: a new GC, a JIT, an LLVM backend, or assembly. `fib` at
~4ns per call through a boxed dynamic call, and `intloop` at 1.6x of C, do
not justify them.

## 6. Reproducing a change

Per the change-one-thing rule: edit, then

```bash
make && make test                       # correctness, both backends
bash benchmarks/profile.sh --reps 9     # more reps, medians are noisier
bash benchmarks/compare.sh              # the C comparison, checked
```

Keep the change only if the medians improve, `make test` stays green
(including `tests/interp/`, which runs every case on both backends), and no
other case regresses by more than noise.

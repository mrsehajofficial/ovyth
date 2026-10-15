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
startup            1ms       1ms      2192        1      2      0     22944
intloop            6ms       6ms      2084        1      2      0     22944
fib                1ms       1ms      2308        1      2      0     22944
strconcat          2ms       2ms      2952        4      2      0     31144
listappend         3ms       3ms      5932        3      2     17     31160
mapops            70ms      70ms     32836   400005      4      0     35264
json               1ms       1ms      2312       90      2      0     43472
chatbot            1ms       1ms      2212       12      2      0     31152
```
Re-measured 15 Oct 2026 after the root-pair change (section 5). Every
number before that date is in `benchmarks/RESULTS.md`; `fib` is the one
that moved, 2ms -> 1ms, because the per-call root bookkeeping is gone.

Process floor: an empty program is **1ms** and one allocation.

## 2. Against C, for the same algorithms

| case | Ovyth | C -O3 | ratio |
|---|---|---|---|
| intloop | 6ms | 3.47ms | **1.7x** |
| mapops | 70ms | 22.72ms | **3.1x** |
| fib | 1ms | 0.12ms | 8x (startup floor; the call work is at parity — §5) |
| strconcat | 2ms | 0.16ms | 12x |
| listappend | 3ms | 0.06ms | 50x |

Every case's whole-process number carries the ~1ms process floor, and the
C/Python columns time the work internally. For sub-millisecond cases the
ratio measures the floor, not the language — which is why the §5
experiment ran fib(35) by hand instead of trusting this table.

## 2b. The finding that changed everything

Every number above was **4-10x worse before this section existed**, and the
cause was not the runtime: it was the binary's dependencies. An arithmetic
program loaded libcurl, libssl, libcrypto and libnghttp3 because
`ovrt_helpers.o` — linked into every program for `len()`, `sum()` and
`join()` — contained the `http` caller. An *empty* C program linked with those
takes **15-18ms to start against 5ms without**: ~12ms of TLS initialisation
paid by arithmetic, and by every tool invocation an agent spawns.

`ov_h_http` now lives in `runtime/src/http_builtin.c`, its own translation
unit, so the linker only pulls it — and so the crypto stack — when the
generated C actually calls it. `--as-needed`, which the driver already passed,
then drops the libraries for everything else. `ldd` on an arithmetic Ovyth
binary is now libc and libm.

```
before:  libcurl libssl libcrypto libnghttp3 libngtcp2 ... (12 libraries)
after:   libm, libc
```

## 2c. What that ruled out

Each of these was measured and found not to be the cause, which is why the
library split was the change made rather than any of them. The method is in
`benchmarks/micro_append.c`, which times the runtime's own push in a plain C
loop with the codegen out of the picture:

```
push, growing      17.8 ns/elem   runtime, exactly what codegen emits
push, pre-sized    11.6 ns/elem   the same store, growth removed
raw C               6.0 ns/elem   the floor
raw C, growing     12.3 ns/elem   growth itself, in raw C
codegen shape      14.7 ns/elem   the emitted loop, hand-written in C
```

The runtime's append is **1.45x** an equivalent C loop with the same growth
policy — it was never the problem. Neither was the loop condition (a boxed
`ov_bool` folds away), the root registration, or the zeroing of a buffer about
to be filled (measured, reverted).

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

## 5. What the measurements changed on 15 Oct 2026

### The finding that mattered: the root bookkeeping, not the boxing

Section 4 above ranked "P1, function boundaries" as the next change,
reasoning that a boxed 16-byte argument and return per call was `fib`'s
9-16x gap to C. That was measured, and it was **wrong**. Boxing is free at
-O3: `ov_int`/`ov_sub`/`ov_add` inline to a tag test or two, and raw
`int64_t` boundaries moved `fib(32)` from 27ms to 25-28ms — inside noise.
Three measurements, in order, on this machine, median of 9 whole-process
runs:

```
hand-written C, fib(35)                                 31-33 ms
C + ov_gc_roots_mark/restore per call                  107 ms
Ovyth fib(35), before                                 103-113 ms
Ovyth fib(35), raw boundaries + no root pair             31 ms
```

The gap was never the value representation. It was the pair of
`ov_gc_roots_mark()` / `ov_gc_roots_restore()` calls every function
emitted: two out-of-line calls into `libovrt.a` (the archive is built
without LTO, so they cannot inline) plus a global load and store per
invocation — about 2.5ns a call, 76ms of `fib(35)`'s 107ms.

The fix is in the codegen: the pair exists to pop a frame's root
registrations on return, so a function that registers nothing needs it
never. Raw `int64_t` parameters are not heap references, and a function
with no rooted local has nothing to pop. Such functions now emit neither
the mark nor the restore. `fib` is at parity with the same function in C,
and the rest of the suite is unchanged — every allocation count in the
table above reproduces exactly.

Two changes were measured and **rejected** on the way:

* raw `int64_t` parameters and returns alone: no measurable effect
  (27ms -> 25-28ms on `fib(32)`, inside noise), kept only because it is
  what makes the root pair omissible and it fixes the loop-variable
  collision below;
* `-flto` on the runtime archive: 3-6ms on `fib(35)`, and it breaks every
  non-release link ("plugin needed to handle lto object"), so no.

### The hidden bug the work exposed

`for n in [1, 2]` inside a function with a parameter named `n` assigned
the raw loop element straight into the parameter's `OvValue` slot — a
clang type error, so the program would not compile at all. The raw
loop-variable path reused an outer slot instead of declaring a fresh
local (the boxed path always declared one). Loop variables now always get
their own C local, matching the interpreter's scoping. Regression:
`tests/interp/028_fn_boundary.ov`.

### What replaced the old "next change" list

1. **The remaining `fib` overhead is the recursion itself** — 31ms against
   31-33ms for C leaves nothing measurable to take.
2. **`mapops`' 400k allocations** remain the benchmark's own string
   building (`"key" + str(i)`), not the map — unchanged conclusion, now
   with one more measurement behind it: hash-map inserts alone are 90ms
   with 3 allocations.
3. **Function-call-boundary boxing**: measured, not the bottleneck.
   Anything that reopens it needs a benchmark whose result contradicts
   the numbers above.

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

# Vayu benchmark results

Everything below was measured on 8 Oct 2026 on this machine: 4 cores,
Linux x86-64, clang 22.1.8 / gcc 16.2.1, Python 3.14.7. Reproduce it all
with `bash benchmarks/run.sh`, `bash benchmarks/compare.sh` and
`bash benchmarks/compare_ai.sh --release`.

Every timing in the comparison table is **checked**: the driver refuses to
print a result unless Vayu's answer equals C's and Python's. Three programs
computing three different things would prove nothing (spec section 35, and
section 37 on not cheating).

---

## 1. Where Vayu stands

```
=== Vayu vs C vs Python -- same algorithms, same outputs ===

case         Vayu (wall)       C -O3      Python   result check
--------------------------------------------------------------------
intloop              22ms    12.04ms     6827ms   identical, py: identical
fib                   6ms     0.41ms       32ms   identical, py: identical
strconcat            14ms     0.62ms        9ms   identical, py: identical
listappend           37ms     0.20ms       91ms   identical, py: identical
mapops              194ms    82.46ms      158ms   identical, py: identical

Result columns are checked, not assumed: Vayu's answer must match C's.
Rust and Go are not installed on this machine, so they are omitted too.
Vayu numbers are whole-process wall clock; C/Python time their cases
internally, so Vayu's column additionally carries ~4ms of process start.
```

Best-of-3 wall clock. This is a desktop, so numbers move between runs: an
hour apart, `intloop` measured 21-22ms while Python's copy measured
6827-7399ms, and Python's `fib` ranged 32-91ms. Treat the tables as orders
of magnitude, not constants.

How to read them honestly:

* **Against Python, Vayu wins wherever the code does real work:** 310x on
  the integer loop, 5x on recursion, 2.5x on list building. That is the
  expected result for compiled native code over an interpreter, and it is
  the honest headline.
* **The two cases that do not win are `mapops` and `strconcat`.** Hash maps
  land within 1.2x of CPython (194ms vs 158ms) after the runtime defects
  below were fixed; string building is 1.6x behind (14ms vs 9ms), because
  CPython reuses the buffer for `s += x` and interns short strings while
  Vayu still allocates one `VyStr` per append. Small-string interning is
  what closes that one -- no new backend required.
* **Against C the gap ranges from 1.8x to 185x, and the spread is the
  story.** On the integer loop Vayu now runs within 1.8x of `-O3
  -march=native` clang (22ms including startup vs 12.04ms timed internally),
  because proven `int` locals emit as raw `int64_t` and never touch the
  boxed path. On list churn it is 185x behind, because every `push` still
  allocates through the collector. The gap is the object model, not the
  algorithms:

  | source of the gap | what it costs | status |
  |---|---|---|
  | every value is a 16-byte tagged `VyValue` passed by value | values flowing through lists, maps and function parameters never live in a register | **the one remaining item** -- needs the P3 type-inference pass to reach every value |
  | `listappend` / `mapops` allocate per push and per key concat | GC traffic per operation; the C versions allocate nothing | **reduced** -- stack-buffer digit rendering, pinned literals, geometric growth, 0.7 load-factor rehashing |
  | `strconcat` | linear now, but each append still allocates a rendered fragment | **reduced** -- render stopped copying; interning would finish it |

## 2. What the optimisations actually bought

| change | before | after | why |
|---|---|---|---|
| binary size for `print("hi")` | 254,848 B | **18,736 B** (13.6x) | link the runtime normally instead of `--whole-archive`, plus `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` + `--as-needed`; a program that never calls `http.*` no longer links libcurl (spec 4, 29) |
| `strconcat` (40k appends) | 32,581 ms | **14 ms** (~2,300x) | the compiler recognises `acc = acc + x` inside a loop and lowers it to a growable string builder — O(n) instead of O(n²) (spec 12). Later: render stopped copying strings, so appending an existing string allocates nothing |
| `vy_str_concat` | 2 allocations, 3 copies | **1 allocation, 2 copies** | builds directly in the tracked allocation instead of via a scratch buffer |
| arithmetic operators | a real call into `libvyrt.a` plus ~7 tag tests per `a + b` | **`static inline` in `vyrt.h`**, one tag test (spec 44, P0) | plus proven int/float locals emitted as raw `int64_t`/`double` |
| `fib(25)` | 10 ms | **6 ms** | the same inlining, plus emit-time constant folding and per-site pinned string literals (P1) |
| `str(i)` | `snprintf` + render `Buf` = a format parse and 2 allocations | **digit loop into a 64-byte stack buffer, 1 allocation**; `nil`/`true`/`false` are pinned singletons (P2) | |
| `listappend` | 58 ms | **37-44 ms** | `xs.push(x)` no longer goes through the out-of-line method dispatcher — the emitted code is a tag guard plus a direct `vy_list_push` (P2). Proven numeric list literals use specialized arrays |
| `mapops` | **infinite hang** | **194 ms** | see below — first a correctness bug, then two performance defects |
| hash-map inserts | grew only at 100% load | **grows at 0.7**; 100k ordered int keys 176ms -> 89ms | (P2b) |
| GC mark of int-keyed containers | 200k calls per collection | **tag compare**, no calls | (P2b) |
| startup of `print("hi")` | ~5 ms | **~5-6 ms** | already trivial; libcurl is no longer initialised by programs that do not use it (spec 30) |

### The loop the optimiser deleted

Re-running the suite on 8 Oct 2026, `intloop` reported **4ms for twenty
million iterations**. That is not possible, and it was not: at `-O2` and
above, clang recognises the affine recurrence

    total = total + i * 3 - 1

and replaces the entire loop with closed-form arithmetic — the emitted
binary literally contained `movabs $599999950000000`, the answer, where the
loop used to be. Plain C does the same thing: compile that loop yourself at
`-O3` and it reports 0.00ms. The C reference had only avoided it with a
`volatile` trip count, which also blocks vectorisation — so C was measured
at a handicap while Vayu was measured doing nothing at all.

The fix, applied identically in all three languages, is an xor/shift body:
`total + (i ^ (i >> 3)) - 1`. Same shape — one int, one counter, one
comparison — but no closed form exists, so every implementation has to run
the loop for real. Measured: C 12.04ms, Vayu 22ms (startup included),
Python 6827ms, all agreeing on `202067735460992`. The comment in
`benchmarks/cases/intloop.vy` records why the naive body must not come
back, because it looks completely harmless.

That is spec 36 and 37 working as intended: a number that is too good is
a bug report, not a headline.

### The six real bugs the benchmarks exposed

Found by writing benchmarks, not by reading code (spec 36).

1. **The collector never computed the live set.** `vy_gc_collect_ex`
   reset `live_bytes` to 0 and nothing ever added to it, so `next_gc`
   collapsed to its 8 MB floor and the collector then ran on *every*
   allocation once the heap passed 8 MB — quadratic death. Fixed by
   accumulating each survivor's size in `mark_value`.
2. **Heap accounting used a shared "last allocation" slot.**
   `vy_heap_note_realloc` computed its delta against `t_cur_alloc`, which
   had just been set by an unrelated string allocation, so the heap total
   inflated without any memory being allocated. Fixed by passing the
   object's own previous size.
3. **`Parser::error` was `[[noreturn]]` but returned.** A parse error left
   `cur_` on the offending token and the recursive-descent loop spun
   forever — an unterminated string consumed memory until the process
   died. Fixed by unwinding via `ParseAbort`, which is what `[[noreturn]]`
   always intended.
4. **The hash map only grew when it was 100% full.** `map_insert_slot`
   returns NULL only when no free slot exists, so every table filled to
   load factor ~1.0 and, with a well-avalanched hash, the trailing
   insertions walked four- and five-figure probe chains each. The comment
   above the function already claimed a 0.75 load factor that the code
   never enforced. `vy_map_set` now rehashes at 0.7: 100k ordered int keys
   went from 176ms to 89ms.
5. **`sweep()` subtracted garbage from an already-rebuilt live total.**
   `collect` zeroes `live_bytes` and rebuilds it from the marked
   survivors, so subtracting the freed bytes double-counted the garbage.
   `live_bytes` came out too low, and `next_gc` is `live*2 + 8MB`, so the
   collector ran more often than intended. Removed; `heap_bytes` is a
   running total and keeps its subtraction.
6. **`mark_loop` paid a function call per primitive element.** It called
   `mark_value` for every list item and map key/value, and `mark_value`
   returned immediately for nil/bool/int/float. Marking a 100k-entry map
   of ints cost 200k calls *per collection*. The tag enum orders the
   container kinds at `VY_STRING`, so a tag compare now replaces the call.

Bug 4 was found by decomposing `mapops` into four 6-line micro-programs
(key construction only; inserts with int keys; a constant-key loop; a bare
arithmetic loop) rather than by guessing at the whole case, and bugs 5-6 by
running `gprof` on the emitted C. That is the method the spec demands
(measure before optimising) and it is recorded because it will recur.

---

## 3. Vayu's own suite

```
=== Vayu benchmark suite (native, 5 reps, best-of) ===

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

(ms = wall clock for the whole process, including startup)
```

Measured 8 Oct 2026 (`bash benchmarks/run.sh`). Whole-process timing is
 deliberate: excluding startup would be exactly the kind of cheating the
spec forbids (section 37). The gap between `best` and `avg` on `mapops`
is desktop noise, not a bimodal distribution.

Two numbers are good regardless of what C does: **~6ms cold start** (no VM
to boot; libcurl is only linked into binaries that call `http.*`) and
**18 KB for a hello-world binary** — dead-code stripping at link time drops
JSON, HTTP and the half of the stdlib the program never touches.

---

## 4. Not yet done

Stated plainly rather than implied:

* **§5/§6 static typing with native representation.** Proven int/float
  locals emit as raw `int64_t`/`double` (no boxing, no GC registration) and
  list literals of proven numeric types use specialized arrays — that is
  what put `intloop` within 1.8x of C. But values flowing through lists,
  maps and function parameters are still boxed; the full type-inference
  pass remains the largest remaining gap.
* **§8/§19 function inlining.** The emitter still does not inline user
  functions. `-O3` on the generated C folds what it can see within a
  function, and since P0 the call body is all inline ops, so the win left
  here is smaller than it was.
* **Small-string interning** would close the last Python loss
  (`strconcat`, 14ms vs 9ms).
* **§35 Rust and Go columns.** Not installed on this machine; omitted
  rather than estimated.
* **§38 PGO**, and **§25/§26 async and concurrency.** Not started.
* Closures do not capture their environment (the native `VyFnPtr` has no
  upvalue slot); a closure that reads an enclosing local is rejected with
  a clear error rather than miscompiled.

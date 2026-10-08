# Vayu benchmark results

> **Note:** the C reference has since been fixed — timings were previously
> taken inside `printf()` with constant arguments, so `intloop`/`listappend`
> reported a constant-folded fictional `0.00ms` for C. Fresh, honest numbers
> (C: `intloop` 43.6ms, `listappend` 0.09ms) are in
> [docs/PERFORMANCE.md](../docs/PERFORMANCE.md).

Machine: 4 cores, Linux x86-64, clang 22.1.8 / gcc 16.2.1, Python 3.14.7.
Reproduce with `benchmarks/run.sh` and `benchmarks/compare.sh`.

Every result below is **checked**: the comparison driver requires Vayu's
computed answer to equal C's and Python's before reporting a timing. A
comparison where the programs compute different things proves nothing
(spec section 35, and section 37 on not cheating).

---

## 1. Where Vayu stands against other implementations

```
=== Vayu vs C vs Python -- same algorithms, same outputs ===

case         Vayu (wall)       C -O3      Python   result check
--------------------------------------------------------------------
intloop             237ms    42.14ms     5612ms   identical, py: identical
fib                   7ms     0.37ms       30ms   identical, py: identical
strconcat            14ms     0.81ms        9ms   identical, py: identical
listappend           34ms     0.11ms       86ms   identical, py: identical
mapops              187ms    64.99ms      156ms   identical, py: identical

Result columns are checked, not assumed: Vayu's answer must match C's.
Rust and Go are not installed on this machine, so they are omitted too.
Vayu numbers are whole-process wall clock; C/Python time their cases
internally, so Vayu's column additionally carries ~4ms of process start.
```

Measured 7 Oct 2026 (`bash benchmarks/compare.sh`, best-of-3 wall clock).
Timings move ±20-40% run to run on a desktop machine (`intloop` measured
193-275ms across recent runs), so treat these as orders of magnitude.

Reading it honestly:

* **Vayu beats Python by 2.5x-24x** on loop, recursion, and list throughput.
  That is the expected result for a compiled language over an interpreter, and
  it is the honest headline.
* **`mapops` is now within 1.2x of CPython** (187ms vs 156ms) after the two
  runtime defects below were fixed; the C margin that remains is the hash
  table, not the language model.
* **`strconcat` is the last case that loses to Python** (14ms vs 9ms): CPython
  reuses the buffer for `s += x` and interns short strings, Vayu still
  allocates one `VyStr` per append.
* **Vayu is 2.9x-309x slower than C.** That gap is the *real* number, and it
  is not a defect in the algorithms -- it is the cost of the dynamic object
  model the compiler currently emits:

  | source of gap | what it costs | status |
  |---|---|---|
  | every value is a 16-byte tagged `VyValue` passed by value | no register allocation; each local is rebuilt and re-tagged per iteration | **the one remaining item** -- needs the P3 type-inference pass |
  | `mapops` builds `"key" + str(i)` per iteration | allocates 2 strings per operation; C's version allocates none | **reduced** -- `str(i)` is a digit loop into a stack buffer; the concat still allocates |
  | `strconcat` | linear, but each append still allocates a rendered fragment | **reduced** -- render no longer copies, so an append of an existing string allocates nothing |

## 2. What the optimisations actually bought

| change | before | after | why |
|---|---|---|---|
| binary size for `print("hi")` | 254,848 B | **18,736 B** (13.6x) | link the runtime normally instead of `--whole-archive`, plus `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` + `--as-needed`. A program that never calls `http.*` no longer links libcurl (spec 4, 29) |
| `strconcat` (40k appends) | 32,581 ms | **14 ms** (~2,300x) | compiler recognises `acc = acc + x` inside a loop and lowers it to a growable string builder, making it O(n) instead of O(n^2) (spec 12). Later: render stopped copying strings, so appending an existing string allocates nothing |
| `vy_str_concat` | 2 allocations, 3 copies | **1 allocation, 2 copies** | builds directly in the tracked allocation instead of via a scratch buffer |
| `intloop` (20M iterations) | 819 ms | **~240-275 ms** (3x) | every arithmetic operator was a call into `libvyrt.a` with ~7 tag tests before `int+int`. The fast paths for `vy_add/sub/mul/div/mod`, unary ops, bitwise ops, `vy_eq/cmp/truthy/is` and `vy_list_push/get` now live as `static inline` in `vyrt.h` (spec §44 P0). Proven int/float locals now emit as raw `int64_t`/`double` |
| `fib(25)` | 10 ms | **6-7 ms** | same inlining, plus emit-time constant folding and per-site pinned string literals (spec §44 P1) |
| `strconcat` / `mapops` key building | `str(i)` = `snprintf` + render `Buf` + copy = 2 allocations and a format parse | **digit loop into a 64-byte stack buffer, 1 allocation**; `nil`/`true`/`false` are pinned singletons; render no longer copies strings (spec §44 P2) |
| `listappend` | 58 ms | **33 ms** | `xs.push(x)` no longer calls the out-of-line method dispatcher per push: the emitted code is `if (vy_tagof(base) == VY_LIST) vy_list_push(...)` with the unchanged dispatch as the fallthrough (spec §44 P2). List literals of proven int/float now use specialized arrays |
| `mapops` (100k inserts + 100k reads) | **infinite hang** | **187-189 ms** | see below -- this was a correctness bug, then two performance defects |
| `print("hi")` startup | ~5 ms | ~4-6 ms | unchanged; runtime init is already trivial, and libcurl is no longer initialised for programs that do not use it (spec 30) |

### The six real bugs the benchmarks exposed

These were found by writing benchmarks, not by reading code (spec 36).

1. **The collector never computed the live set.** `vy_gc_collect_ex` reset
   `live_bytes` to 0 and nothing ever added to it, so `next_gc` collapsed to
   its 8 MB floor and the collector then ran on *every* allocation once the
   heap passed 8 MB -- quadratic death. Fixed by accumulating each survivor's
   size in `mark_value`.

2. **Heap accounting used a shared "last allocation" slot.** `vy_heap_note_realloc`
   computed its delta against `t_cur_alloc`, which had just been set by an
   unrelated string allocation. The heap total inflated without any memory
   being allocated. Fixed by passing the object's own previous size.

3. **`Parser::error` was `[[noreturn]]` but returned.** A parse error therefore
   left `cur_` on the offending token and the recursive-descent loop spun
   forever -- an unterminated string consumed memory until the process died.
   Fixed by unwinding via `ParseAbort`, which is what `[[noreturn]]` always
   intended.

4. **The hash map only grew when it was 100% full.** `map_insert_slot` returns
   NULL only when no free slot exists, so every table filled to load factor
   ~1.0 and, with a well-avalanched hash, the trailing insertions walked four-
   and five-figure probe chains each. The comment above the function already
   claimed a 0.75 load factor that the code never enforced. `vy_map_set` now
   rehashes at 0.7: 100k ordered int keys went from 176 ms to 89 ms.

5. **`sweep()` subtracted garbage from an already-rebuilt live total.**
   `collect` zeroes `live_bytes` and rebuilds it from the marked survivors, so
   subtracting the freed bytes double-counted the garbage. `live_bytes` came
   out too low, and `next_gc` is `live*2 + 8MB`, so the collector ran more
   often than intended. Removed; `heap_bytes` is a running total and keeps its
   subtraction.

6. **`mark_loop` paid a function call per primitive element.** It called
   `mark_value` for every list item and map key/value, and `mark_value`
   returned immediately for nil/bool/int/float. Marking a 100k-entry map of
   ints cost 200k calls *per collection*. The tag enum orders the container
   kinds at `VY_STRING`, so a tag compare now replaces the call.

Bug 4 was found by decomposing `mapops` into four 6-line micro-programs
(key construction only; inserts with int keys; constant-key loop; bare
arithmetic loop) rather than by guessing at the whole case, and bugs 5-6 by
running `gprof` on the emitted C. That is the method the spec demands
(measure before optimising) and it is recorded because it will recur.

## 3. Vayu's own suite

```
=== Vayu benchmark suite (native, 5 reps, best-of) ===

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

(ms = wall clock for the whole process, including startup)
```

Measured 7 Oct 2026 (`bash benchmarks/run.sh`). Note `intloop`'s binary
grew 26,952 -> 31,048 bytes and the `json` case binary 35,168 -> 39,264
bytes since the last snapshot: the v0.1.2 compile-time path
specialization in `codegen_c.cpp` emits larger per-site inline blocks.
`intloop` itself varies 193-275ms run to run (±20-40% desktop variance),
so the slower best-of here is noise, not a regression from HEAD.

## 4. Not yet done

Stated plainly rather than implied:

* **§5/§6 static typing with native representation.** Proven int/float locals are now emitted as raw `int64_t`/`double` (no boxing, no GC registration), and list literals of proven numeric types use specialized array types. However, values flowing through lists, maps, and function parameters are still boxed — the full type-inference pass to reach every value remains the largest remaining gap.
* **§8/§19 function inlining.** The emitter still does not inline user functions; `-O3` on the generated C folds what it can see, and the call body is now all inline ops, so the win available here is smaller than it was.
* **Small-string interning** would close the last Python loss (`strconcat`, 14ms vs 9ms).
* **§35 Rust and Go columns.** Not installed on this machine; omitted rather than estimated.
* **§38 PGO**, and **§25/§26 async and concurrency.** Not started.
* Closures do not capture their environment (the native `VyFnPtr` has no upvalue slot); a closure that reads an enclosing local is rejected with a clear error rather than miscompiled.

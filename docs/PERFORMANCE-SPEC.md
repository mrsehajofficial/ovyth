# Vayu maximum performance engineering specification

This is the document the project measures itself against. It says what
performance means for Vayu, what principles the compiler and runtime must
follow to get there, and — in section 44 — what to actually do first,
with the measured results of each stage as it landed. Sections 1–43 are
the principles; section 44 is the scorecard.

---

## 1. Primary goal

Vayu must be designed as a high-performance native compiled language. The
priority order is fixed:

1. Correctness
2. Predictable performance
3. Low memory overhead
4. Low runtime overhead
5. Fast compilation where practical
6. Developer ergonomics

Two things the goal is *not*. It is not to imitate Python's internal
behaviour at the cost of runtime speed, and it is not to claim that Vayu
will beat every existing language. The goal is simpler and more testable:
remove unnecessary overhead wherever technically possible, and let the
compiler generate highly optimized native machine code. When we cannot
demonstrate the result with a measurement, we do not claim it (see
section 40).

---

## 2. Keep the existing C++ compiler

The compiler may stay implemented in C++. There is no reason to rewrite
it merely because the implementation language is C++, and C++ is only the
implementation language — Vayu is the target language.

The architecture stays as it is: source goes through lexer, parser, AST,
semantic analysis, an IR, an optimizer, and then to native code. Replace a
component only when there is a demonstrated technical reason, not a
theoretical one.

---

## 3. Native code must be the normal execution path

Production Vayu programs execute as native machine code: program.vy →
compiler → optimized IR → machine code → executable.

The interpreter exists for debugging, development, a REPL, tests and rapid
prototyping. What it must never do is silently become the way production
programs run. If a program is supposed to be compiled, it is compiled.

---

## 4. Avoid a heavy runtime

The runtime must be as small as reasonably possible. A program that just
does `x = 10` and `print(x)` should not require a large mandatory runtime
to execute. Where the linker can prove a runtime component is unused —
JSON, HTTP, half the stdlib — it should be dropped from the binary.

The shape we want is: small program, small executable, small runtime
footprint. The shape we refuse is: small program, large mandatory runtime.

---

## 5. Avoid Python's object model

Do not represent every value as a heavyweight generic object. Wrapping
every primitive in

```
Object
 ├── type information
 ├── reference information
 ├── metadata
 └── payload
```
is exactly the cost Vayu exists not to pay. Prefer native representations:
an int is a native integer, a float a native floating-point value, a bool
a native boolean, a byte a native byte.

So `a = 10`, `b = 20`, `c = a + b` should compile to something close to
the corresponding machine instructions, with no unnecessary runtime
dispatch in between.

---

## 6. Static typing with inference

Vayu provides static typing without annotation noise:

```vy
x = 10
name = "Vayu"
active = true
```

The compiler infers int, string, bool. Explicit types remain available
(`x: Int = 10`) when the programmer wants to pin one down. Static type
information is what gives the optimizer its leverage, so dynamic typing is
never the default execution model.

---

## 7. Dynamic features must be explicit

If Vayu eventually supports genuinely dynamic values, they must be opt-in.
The normal path stays statically typed; anything dynamic is written
dynamically. Avoid `everything → dynamic Value` as a representation — it
hands the optimizer nothing to work with. Normal code should be statically
typed so it can be optimized aggressively, and dynamic code should say so
at the source.

---

## 8. Zero-cost abstractions

A language feature should cost nothing at runtime once the compiler can
prove the abstraction has no observable effect. A small function like

```vy
function add(a, b) {
    return a + b
}
```

should be inlined when inlining is beneficial. What must not happen is
that a statically-known call gets lowered into a runtime lookup → dynamic
dispatch → generic invocation chain. When the target is known, the call is
a call (or nothing at all).

---

## 9. Minimize heap allocation

Heap allocation is expensive next to stack and register operations, so
nothing gets allocated on the heap by default. The preference order is:
register, then stack, then heap only when necessary.

Over time the compiler should grow escape analysis, allocation elimination,
scalar replacement, stack allocation and lifetime analysis where practical.

---

## 10. Make stack allocation the natural choice

Local values that do not escape their scope should be eligible for stack
allocation. A function like

```vy
function calculate() {
    x = 10
    y = 20
    return x + y
}
```

should involve no heap allocation at all — ideally the generated code is a
few register operations and a return. The heap is for values that outlive
the frame, not for everything that happens to exist for a moment.

---

## 11. Minimize memory copies

Avoid the pattern copy → modify → copy → return. Prefer references, views
and slices wherever that is safe, and give strings, byte buffers, arrays
and slices representations that can share underlying memory. Taking a view
of a buffer must not copy the buffer.

---

## 12. Design strings carefully

Strings will be one of the most frequently used types, so their design
decides a lot of real-world performance. Naive concatenation — where
`a + b + c + d` creates a temporary allocation and copy per step — is not
acceptable as the final design. The compiler and runtime must be able to
optimize concatenation, through some combination of capacity-aware
buffers, proper builders, compiler recognition of append patterns, move
semantics, slices and copy elision. Which strategies win is decided by
benchmarks, not by taste (the `strconcat` case in `benchmarks/` exists for
exactly this purpose).

---

## 13. Move semantics and ownership

Passing a large value must not implicitly mean "allocate, copy the whole
buffer, pass the copy". Investigate an ownership/move model so the language
can say clearly what happens to `data` when it is handed to `process(data)`:
who owns it, whether it is borrowed or moved, what mutability applies.

The warning that comes with this section: do not copy Rust's entire
ownership system blindly. Design only what Vayu actually needs — and note
that this section stays parked until the runtime model in sections 14–15
is resolved against what the runtime really is (see section 44, P4).

---

## 14. Avoid reference counting everywhere

Do not put a reference counter on every value by default. Refcounting
injects increments, decrements, atomic operations and cache traffic into
otherwise simple operations. Use ownership and lifetime analysis where
possible, and reserve reference counting for the places where shared
ownership genuinely requires it.

---

## 15. Garbage collection should not be mandatory

A garbage collector must not be introduced merely because it makes the
implementation easier. It costs memory overhead, pauses, CPU overhead and
unpredictable latency. Investigate the alternatives first: stack
allocation, ownership, borrowing, explicit allocation, and targeted
reference counting. If a GC turns out to be useful for specific dynamic
features, keep it optional or isolated where it can be paid for only by
the programs that use those features.

(Honesty note: today the runtime *is* a mark-sweep collector plus
refcounted strings. Section 44 P4 commits to making the document match
that reality — adopt the model, then optimise it — rather than pretending
sections 13–15 are already satisfied.)

---

## 16. Make memory layout predictable

Data structures should have predictable layouts with minimal pointer
indirection. Contiguous memory beats pointer-chasing wherever it fits:

```
[ item ][ item ][ item ][ item ]
```

not `pointer → item` three times over. Prefer the first shape.

---

## 17. Cache locality matters

Algorithmic complexity is not enough. A theoretically efficient algorithm
is still slow if it misses cache every iteration, so compiler and
stdlib work has to consider cache locality, contiguous memory, data
layout, branch prediction, memory bandwidth and pointer chasing. Hot data
gets stored efficiently, not merely computed efficiently.

---

## 18. Bounds checking should be optimizable

Safe arrays keep their bounds checks — but a check the compiler can prove
redundant should be removable. In a loop like

```
for i in 0..array.length {
    process(array[i])
}
```

the index is provably within bounds on every iteration, and the optimizer
should eventually learn to see that. Safety must not automatically mean
paying for the same check twice.

---

## 19. The compiler optimization pipeline

The compiler should have a serious optimization pipeline behind the AST:
constant folding and propagation, dead-code elimination, inlining,
devirtualization, escape analysis, allocation elimination, loop
optimization and vectorization, before LLVM takes over for machine-level
optimization.

Not every stage has to be implemented by hand. Where LLVM already provides
an excellent optimization, use it — but only after the emitted code gives
LLVM something it can actually work with (functions that call out of line
to do `a + b` cannot be optimized across the boundary; see section 44 P0).

---

## 20. LLVM should be used properly

LLVM is the native optimization and backend infrastructure, not merely a
way to emit basic machine code. Release builds use aggressive optimization
levels — inlining, dead-code elimination, vectorization, LTO,
target-specific optimization — where appropriate, exposed as something
equivalent to `vyc build --release program.vy`.

---

## 21. Target the actual CPU

Vayu should allow target-specific optimization — `vyc build --release
--target=native program.vy` — so the backend can use the CPU features of
the machine it is compiling on: SSE/AVX/AVX2/AVX-512, AES, BMI, FMA where
supported. The constraint is symmetric: never emit instructions that make
the binary incompatible with its declared target.

---

## 22. SIMD and vectorization

CPU-heavy operations should be vectorizable, and LLVM can do that
automatic vectorization well. The preferred path is high-level Vayu →
optimized IR → LLVM vectorization → SIMD machine code. Manual SIMD and
intrinsics are introduced only for proven hotspots, never as the default
way to write a loop.

---

## 23. Function calls should be cheap

When the compiler knows what `add(1, 2)` refers to, it emits a direct call
or inlines it. Turning a known call into a hash lookup of the function
name → a runtime object → a dynamic invocation is forbidden unless the
program explicitly asked for dynamic behaviour.

---

## 24. Generics should be compile-time friendly

If generics are eventually added, they must not force every generic
operation through dynamic dispatch. Where practical, a generic function is
specialized at compile time for the actual types, producing optimized
native code per instantiation. Equally important: do not implement
generics until the core language actually needs them.

---

## 25. Async must have low overhead

Eventually Vayu should support asynchronous programming, and it must not
be designed around heavyweight task objects or expensive task creation.
The targets are cheap tasks, cheap suspension, cheap wake-up and an
efficient scheduler. It must not be built before the synchronous native
execution path is stable.

---

## 26. Concurrency must not penalize single-threaded programs

A program that never uses concurrency should not pay for one. No
thread pool, no scheduler, no synchronization infrastructure initialised
for programs that will never touch it; runtime components come up only
when they are needed, where practical.

---

## 27. Avoid hidden work

Language constructs must not silently do heap allocation, copying, locking,
reflection, network access or dynamic dispatch when their semantics don't
require it. The programmer should be able to reason about the cost of the
code they are reading. Expensive operations stay visible.

---

## 28. Compile-time computation

Where safe and useful, let the compiler evaluate constant expressions:
`x = 100 * 20 + 50` can become `x = 2050` at compile time rather than
paying for the arithmetic at runtime. The same principle can eventually
extend to more sophisticated compile-time computation.

---

## 29. Dead code must disappear

Unused functions and unused runtime components must not survive into the
final binary when dead-code elimination, linker garbage collection and LTO
can remove them. (This is section 4 in practice: an 18 KB hello-world is
the acceptance test.)

---

## 30. Runtime initialization must be minimal

`print("Hello")` must not initialise the HTTP subsystem, the database
subsystem, an AI subsystem or an async scheduler. The runtime initialises
only what the program requires.

---

## 31. Error handling must be predictable

Expected, ordinary failures should not travel through an extremely
expensive mechanism. A result-style model (`Result<T, Error>`) can be
considered for operations where failure is expected, and exceptions must
not become the universal control-flow mechanism. If exceptions are
eventually supported, the non-exception path is the one that gets
optimized.

---

## 32. The standard library must be native-performance-oriented

The standard library must not be a friendly high-level layer that quietly
adds large overhead on top of native operations. The important primitives —
strings, arrays, maps, files, bytes, math, I/O, HTTP, JSON — are all
benchmarked. Where a mature system library is faster than anything we
would write, use it.

---

## 33. Don't reinvent high-performance libraries without a reason

If a mature native library is significantly faster and more reliable,
integrate it. Do not hand-write cryptography, compression, BLAS, TLS or
cpu primitives just so the project can say Vayu implemented them. The
objective is performance, not a maximum amount of handwritten code.

---

## 34. Benchmark everything

Keep a permanent benchmark suite. At minimum it covers integer arithmetic,
floating-point arithmetic, function calls, loops, arrays, maps, strings,
string concatenation, memory allocation and copying, file I/O, JSON, HTTP,
concurrency, startup time and binary size — recording latency,
throughput, memory, allocation counts and CPU use. Never optimize based
solely on intuition.

---

## 35. Compare against real implementations

Compare Vayu against Python, C, C++, Rust and Go when appropriate — using
equivalent algorithms, equivalent inputs, equivalent outputs and
equivalent compiler settings. Toy programs and mismatched workloads are
not comparisons. If a toolchain is not installed, the column is omitted,
not estimated.

---

## 36. Profile before optimizing

Every optimization cycle runs the same loop: benchmark → profile → find
the bottleneck → optimize → benchmark again → keep it only if the
improvement is real. Code that is not actually a bottleneck does not get
optimized.

---

## 37. Avoid benchmark cheating

Do not: remove work from a benchmark; use different algorithms without
documenting it; selectively exclude initialization; ignore memory
allocation; ignore compilation or runtime startup where it matters;
compare debug Vayu against optimized C++; or compare different workloads.
Benchmarks must represent real programs. (The `intloop` story in section
44 is what this looks like when it is enforced against ourselves.)

---

## 38. Have multiple performance modes

Support, eventually: Debug, Release, Release + native CPU, Release + LTO,
and PGO — as `vyc build program.vy`, `vyc build --release program.vy`,
`vyc build --release --target=native program.vy`, and later
`vyc profile` / `vyc build --pgo`.

---

## 39. Don't sacrifice correctness for micro-optimizations

Never introduce undefined behaviour to gain a few percent. The shape to
aim for is: safe by default, an explicit escape hatch where unsafe is
truly needed, and optimized compiler-generated code doing the heavy
lifting. Safety and performance are not mutually exclusive.

---

## 40. Don't optimize for "looks fast"

"C++ is fast, therefore this must be fast" and "LLVM is used, therefore
Vayu is automatically fast" are both false. Actual performance comes from
the whole stack together: language semantics, type system, memory model,
data structures, compiler IR, optimizer, runtime, and the generated
machine code. Any decision that cannot be measured goes back on the
shelf.

---

## 41. The target pipeline

The intended final architecture looks like this:

```
              VAYU SOURCE
                   |
                   v
        Lexer → Parser → AST → Semantic Analysis
                   |
                   v
                Vayu IR
              /         \
     Optimization      Analysis
              \         /
                   v
             Optimized IR
                   |
                   v
                 LLVM  →  native machine code (CPU-specific or generic)
                   |
                   v
            small runtime → native executable
```

The final program contains only what it actually needs.

One correction this document owes the reader (see section 44, P4): today's
backend emits C and hands it to clang — which *is* LLVM, so sections 19–22
are partly free already, but only within a single generated function. The
IR → Optimizer → LLVM IR leg of the diagram above is the target
architecture, not yet the implemented one; `codegen_llvm.cpp` is a stub.
Section 44 tracks closing that gap.

---

## 42. Performance philosophy

Four sentences to argue from:

- Do expensive work only when necessary.
- If the compiler can prove something is unnecessary, remove it.
- If the compiler cannot prove it, measure it before optimizing it.
- Do not inherit overhead merely because another language does it that way.

Vayu takes its simplicity from Python's example. It does not take
Python's runtime architecture.

---

## 43. Development priority

Do not attempt everything above at once. The order is:

1. **Correct compiler** — lexer, parser, AST, semantic analysis, IR.
2. **Native execution** — backend, native executable, basic optimizer.
3. **Efficient primitives** — integers, floats, strings, arrays, maps,
   functions, memory.
4. **Runtime efficiency** — allocation, ownership, copy elimination,
   runtime minimisation.
5. **Compiler optimization** — inlining, constant folding, DCE, loop
   optimization, vectorization, LTO.
6. **Concurrency** — tasks, async I/O, scheduler, channels.
7. **Higher-level libraries** — HTTP, JSON, files, database.

Only after those foundations are strong does Vayu start adding specialised
AI/RAG/agent capabilities.

---

## 44. Execution order: where the milliseconds actually are

Grounded in the code as it exists today (the C backend; measured numbers
in `docs/PERFORMANCE.md` and `benchmarks/RESULTS.md`). Every stage below
lands only if `make test` stays green and `benchmarks/run.sh` plus
`benchmarks/compare.sh` are re-run with the new numbers recorded. This
section exists because sections 1–43 say what to believe; they do not say
what to do first.

### Stage P0 — make arithmetic inlineable

*Status: LANDED and measured. Highest return, no language change.*

The hot path for `a + b` used to be: `codegen_c.cpp` emits a real call to
`vy_add`, which lives out-of-line in `value.c` and starts with about seven
tag/branch tests (three for string, list, `is_num` twice, both-int) before
reaching the int+int case — and `libvyrt.a` was built without LTO, so even
a `--release` LTO build could never inline across the archive boundary.

What was done:

- `vy_add/sub/mul/div/mod`, `vy_neg/vy_pos/vy_not`, the bitwise ops,
  `vy_eq`, `vy_cmp`, `vy_truthy` and `vy_is` all grew `static inline` fast
  paths in `vyrt.h`: both tags `VY_INT` → `vy_int(a.i <op> b.i)`, floats
  likewise, otherwise a call to the unchanged `*_slow` body in `value.c`,
  so string coercion, list concat and nil handling are byte-for-byte the
  same as before.
- `vy_list_push`/`vy_list_get` got the same treatment (fast path in the
  header, `*_slow` fallback in `vyrt.c`).
- `-flto` on `libvyrt.a` was deliberately *not* added: `vyc` links the
  emitted C with clang while the runtime archive is built by g++, so a
  bitcode archive would not be readable in every configuration. With the
  hot path in the header, LTO had nothing left to buy here anyway.

Measured: `intloop` 819ms → 184ms (4.4x; the target had been ≤150ms),
`fib` 10ms → 4ms (target met). Vayu vs Python on `intloop` went from 8.1x
faster to 27x.

### Stage P1 — optimise what the emitter can see

*Status: LANDED (first two items) and measured.*

- **Emit-time constant folding.** `fold_const_binop`/`fold_const_unary` in
  `codegen_c.cpp` fold int/float literals on Unary and Binary nodes, so
  `2 * 3 + n` emits `6 + n` instead of three runtime calls.
- **Per-site pinned string literals.** A literal becomes a block-local
  `static` initialised once through `vy_str_lit()` and pinned with
  `VY_HDR_PIN`, so `sweep()` keeps it alive and a loop body that references
  `"hello world "` stops allocating it 40,000 times.
- **Emitting small user functions as `static inline`**: still open. `fib`
  is 243k calls and the win is smaller now that the call body is all
  inline ops.
- **Keeping values in `VyValue` locals rather than re-materialising
  temporaries**: the emitter already does this for most shapes; no further
  change needed.

Measured: `fib` 10ms → 4ms — within 12x of C's 0.34ms rather than the
≤2x target; the remainder is the call itself plus boxed arithmetic, which
is P3's work. Startup unchanged at ~3–4ms.

### Stage P2 — kill per-operation allocation

*Status: LANDED (all of it) and measured.*

- **Render to a stack buffer for scalars.** `vy_render()` used to assemble
  every scalar through a growable `Buf` (malloc 256 → render → copy into a
  fresh `VyStr` → free) even for `str(42)`. Ints and floats now render
  into a 64-byte stack buffer with one allocation, and the int path uses a
  hand-rolled digit loop instead of `snprintf("%lld")`, which alone cost
  ~150ns of format parsing per call. `nil`/`true`/`false` are pinned
  singletons.
- **`vy_render` no longer copies strings.** Every `VyStr` in the runtime is
  immutable by construction (an audit of every `bytes[...] =` write showed
  they all target a freshly allocated block), so render returns the same
  object. That removed a per-append allocation from `strconcat`.
- **Pinned string literals (P1)** removed the literal side of the same
  problem.
- **`xs.push(x)`.** The emitter used to call `vy_h_value_method`, an
  out-of-line dispatch with a `strcmp` chain over the method name, once per
  push. The call site now emits a tag guard —
  `if (vy_tagof(base) == VY_LIST) vy_list_push(...)` — and falls through to
  the unchanged dispatch for any other base, so error behaviour is
  identical. Only emitted for arity ≤ 1, so every argument expression is
  still evaluated.
- **`str_build` geometric growth**: superseded. The builder already grew
  geometrically, and the per-append allocation it targeted is gone via
  string sharing above.

Measured: `mapops` 982ms → 191ms (5.1x; target was ≤300ms). `strconcat`
29ms → 13ms, and it went from losing to CPython's in-place append to
beating it (13ms vs 16–27ms measured under load). `listappend` 58ms →
33–35ms.

### Stage P2b — the runtime bugs a micro-benchmark decomposition found

*Method first, because it will recur:* instead of guessing why `mapops`
was still slow, three throwaway programs isolated the parts — key
construction only; map inserts with int keys and no allocation; a
constant-key loop; a bare arithmetic loop. The decomposition said inserts
were 600ns each with zero allocations, so the cost was not allocation at
all, and `gprof` then named the functions.

1. **The hash map only grew when it was 100% full.** `map_insert_slot`
   returns NULL only when no free slot exists, so every table filled to
   load factor ~1.0 and, with a well-avalanched hash, the trailing
   insertions walked four- and five-figure probe chains each. The comment
   above the function already claimed a 0.75 load factor the code never
   enforced. `vy_map_set` now rehashes at 0.7: 100k ordered int keys went
   from 176ms to 89ms, and `mapops` went from 5x slower than CPython to
   roughly parity.
2. **`sweep()` subtracted freed bytes from `live_bytes` *after* `collect`
   had already zeroed and rebuilt it from the marked survivors**, so
   `live_bytes` came out too low — and `next_gc` is `live*2 + 8MB`, so the
   collector ran more often than the growth policy intended. Removed;
   `heap_bytes` was and is a running total, so it keeps its subtraction.
3. **`mark_loop` called `mark_value` for every list item and map
   key/value**, and `mark_value` returned immediately for nil/bool/int/
   float. Marking a 100k-entry map of ints cost 200k calls *per
   collection*. The tag enum orders the container kinds at `VY_STRING`, so
   a tag compare now replaces the call: `b2_insonly` (100k inserts, no
   lookup) went 63ms → 51ms, `listappend` 39ms → 33ms.

The general lesson, recorded because it will recur: the first three
findings were invisible in the source and obvious in three 6-line
programs.

### Stage P3 — unboxed specialisation

*Status: PARTIALLY LANDED.*

Against sections 5, 6 and 7 — the "next level":

- Sema tracks `is_proven_int`/`is_proven_float` for literal initialisations.
- Codegen emits raw `int64_t`/`double` locals instead of boxed `VyValue`
  for proven types, and skips GC registration for them.
- Fast arithmetic paths on raw locals: add/sub/mul/div/mod, comparisons,
  bitwise.
- Specialized array types — `VyInt64Array`, `VyFloat64Array`,
  `VyStringArray` — with contiguous buffers and tag-guarded fast paths;
  list literals of proven int/float emit them.

What remains: values flowing through lists, maps and function parameters
are still boxed. Full type-inference that reaches every value is the
larger remaining task. It is also where the payoff is: with unboxed
locals, `intloop` now runs within 1.8x of `-O3` C (22ms vs 12.04ms,
8 Oct 2026), while `listappend` — fully boxed — is 185x behind. Verify
against both targets: keep `intloop` near C, and no behavioural change
anywhere (both backends must agree per `tests/interp.sh`).

### Benchmark integrity note (8 Oct 2026) — a case the optimiser deleted

Re-running the suite for the docs rewrite, `intloop` reported 4ms for 20
million iterations: impossible, and not true. At `-O2` and above, clang
replaces the affine recurrence `total = total + i * 3 - 1` with its
closed-form value, in plain C as well as in generated Vayu code — the
binary contained `movabs $599999950000000` where the loop used to be. The
C reference had only been protected by `volatile` trip counts, which also
block vectorisation, so the old table had Vayu measured doing nothing
while C was measured at a handicap.

The case now uses `total + (i ^ (i >> 3)) - 1` — same shape, no closed
form — identically in all three languages, and the corrected numbers
(C 12.04ms, Vayu 22ms, Python 6827ms, all agreeing on the same answer)
are what the documentation quotes. This is sections 36–37 applied to our
own numbers: a result that is too good is a bug report, not a headline.

### Stage P4 — make the document match the machine

Against section 40:

- The pipeline diagram in section 41 shows Vayu IR → Optimizer → LLVM IR,
  but `codegen_llvm.cpp` is a 13-line stub and `compiler/optimizer/` is
  empty. Section 41 now dates that diagram as the target architecture;
  the real backend today is emitted C → clang (which *is* LLVM, so
  sections 19–22 are partially free already — within one function; P0 is
  what buys back the cross-call part).
- Resolve sections 14/15 against reality: the runtime *is* a mark-sweep
  GC plus refcounted strings. Commit to that model, then optimise it
  (generational collection, bump allocation) instead of speculating about
  ownership systems — section 13 stays parked.
- Add the benchmark cases section 34 already demands: float arithmetic,
  sorting, closure calls — and make every case report allocation counts,
  not just time.
- Profiling tooling: `perf record` on a benchmark binary before any new
  optimisation lands. Section 36 without a tool is a wish.

---

## Final requirement

Do not claim Vayu is "high performance" because the compiler is written in
C++, because LLVM is used, because the language is compiled, or because
the binary is native. None of those are performance. Performance is
demonstrated experimentally, and the project answers these questions
continuously:

- How fast is it?
- How much memory does it use?
- How many allocations occur?
- Where is the bottleneck?
- What changed after the optimization?
- Did the optimization actually improve real workloads?

The objective all of this adds up to: Python-like productivity, native
compilation, low runtime overhead, an efficient memory model, aggressive
compiler optimization, efficient concurrency — and a high-performance
Vayu that can show its work.

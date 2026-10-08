Vayu Maximum Performance Engineering Specification

1. Primary Goal

Vayu must be designed as a high-performance native compiled language.

The priority order is:

1. Correctness
2. Predictable performance
3. Low memory overhead
4. Low runtime overhead
5. Fast compilation where practical
6. Developer ergonomics

Vayu must not sacrifice runtime performance merely to imitate Python's internal behavior.

The goal is NOT to claim that Vayu will be faster than every existing language.

The goal is to remove unnecessary overhead wherever technically possible and allow the compiler to generate highly optimized native machine code.

---

2. Keep the Existing C++ Compiler

The compiler may remain implemented in C++.

Do NOT rewrite the compiler simply because it is written in C++.

C++ is the implementation language.

Vayu is the target language.

The existing architecture should remain:

Vayu source
    ↓
Lexer
    ↓
Parser
    ↓
AST
    ↓
Semantic analysis
    ↓
Vayu IR
    ↓
Optimizer
    ↓
LLVM IR
    ↓
Native machine code

Only replace components when there is a demonstrated technical reason.

---

3. Native Code Must Be the Normal Execution Path

Production Vayu programs should execute as native machine code.

Target:

program.vy
     ↓
Vayu compiler
     ↓
optimized LLVM IR
     ↓
native machine code
     ↓
executable

The interpreter may exist for:

- debugging
- development
- REPL
- testing
- rapid prototyping

But production execution must not silently fall back to the interpreter.

---

4. Avoid a Heavy Runtime

The runtime must be as small as reasonably possible.

Do not require a large runtime merely to execute:

x = 10
print(x)

The compiler should eliminate unused runtime components during linking where possible.

Prefer:

small program
 ↓
small executable
 ↓
small runtime footprint

rather than:

small program
 ↓
large mandatory runtime

---

5. Avoid Python's Object Model

Do NOT represent every value as a heavyweight generic object.

Avoid a mandatory representation such as:

Object
 ├── type information
 ├── reference information
 ├── metadata
 └── payload

for every primitive value.

Prefer native representations:

int    → native integer
float  → native floating point
bool   → native boolean
byte   → native byte

A simple integer operation should compile to something close to the corresponding machine instruction.

Example:

a = 10
b = 20
c = a + b

should have no unnecessary runtime dispatch.

---

6. Static Typing With Inference

Vayu should provide static typing while keeping syntax simple.

Example:

x = 10
name = "Vayu"
active = true

The compiler should infer:

x      → integer
name   → string
active → boolean

Explicit types should remain available:

x: int = 10

Static information gives the optimizer much more information.

Do not make dynamic typing the default execution model.

---

7. Dynamic Features Must Be Explicit

If Vayu eventually supports dynamic values, they should not infect the entire program.

Avoid:

everything → dynamic Value

Prefer:

normal code → statically typed
dynamic code → explicitly dynamic

This allows the compiler to optimize normal code aggressively.

---

8. Zero-Cost Abstractions

A language feature should not impose runtime cost when the compiler can prove that the abstraction has no observable effect.

Example:

fn add(a: int, b: int) -> int {
    return a + b
}

The compiler should be able to inline this when beneficial.

Do not automatically turn every function call into:

runtime lookup
 → dynamic dispatch
 → generic invocation

when the target is statically known.

---

9. Minimize Heap Allocation

Heap allocation is expensive compared with simple stack/register operations.

Do not allocate everything on the heap.

Prefer:

register
 ↓
stack
 ↓
heap only when necessary

The compiler should eventually perform:

- escape analysis
- allocation elimination
- scalar replacement
- stack allocation
- lifetime analysis

where practical.

---

10. Make Stack Allocation the Natural Choice

Local values that do not escape their scope should be eligible for stack allocation.

Example:

fn calculate() {
    x = 10
    y = 20
    return x + y
}

There should be no unnecessary heap allocation.

Ideally the generated code becomes essentially:

register operations
return

---

11. Minimize Memory Copies

Avoid unnecessary:

copy
 → modify
 → copy
 → return

operations.

Prefer references/views/slices where safe.

Provide efficient representations for:

string
bytes
arrays
buffers
slices

For example:

buffer
  ↓
slice/view

should not automatically copy the underlying memory.

---

12. Design Strings Carefully

Strings will be one of the most frequently used data types.

Do not implement naive concatenation such that:

a + b + c + d

creates many temporary allocations and copies.

The compiler/runtime should be capable of optimizing concatenation.

Possible strategies include:

- capacity-aware buffers
- efficient builders
- compiler optimization
- move semantics
- slices
- copy elision

The final design should be determined by benchmarks.

---

13. Move Semantics and Ownership

Investigate an ownership/move system rather than automatically copying large values.

Example:

data = create_large_buffer()
process(data)

Passing "data" should not automatically mean:

allocate
copy entire buffer
pass copy

The language should have a clear model for:

- ownership
- borrowing
- moving
- immutable references
- mutable references

However:

«Do not copy Rust's entire ownership system blindly.»

Design only what Vayu actually needs.

---

14. Avoid Reference Counting Everywhere

Do not automatically put a reference counter on every value.

Reference counting can introduce:

increment
decrement
atomic operations
cache traffic

into otherwise simple operations.

Use ownership/lifetime analysis where possible.

Use reference counting only where shared ownership genuinely requires it.

---

15. Garbage Collection Should Not Be Mandatory

Do not introduce a garbage collector merely because it makes implementation easier.

A GC can introduce:

- memory overhead
- pauses
- CPU overhead
- unpredictable latency

Investigate alternatives first.

Possible model:

stack allocation
+
ownership
+
borrowing
+
explicit allocation
+
targeted reference counting

If a garbage collector is eventually useful for specific dynamic features, keep it optional or isolated where possible.

---

16. Make Memory Layout Predictable

Data structures should have predictable layouts.

Avoid unnecessary pointer indirection.

Prefer contiguous memory where appropriate:

array
[ item ][ item ][ item ][ item ]

rather than:

pointer → item
pointer → item
pointer → item

Contiguous memory improves cache locality.

---

17. Cache Locality Matters

Algorithmic complexity is not enough.

A theoretically efficient algorithm can still be slow because of cache misses.

Compiler and standard-library implementations should consider:

- cache locality
- contiguous memory
- data layout
- branch prediction
- memory bandwidth
- pointer chasing

Hot data should be stored efficiently.

---

18. Bounds Checking Should Be Optimizable

Safe arrays should normally have bounds checks.

But if the compiler can prove that an index is valid, it should be able to eliminate redundant checks.

Example:

for i in 0..array.length {
    process(array[i])
}

The optimizer should eventually recognize that the index is within bounds.

Safety must not automatically mean unnecessary checks on every operation.

---

19. Compiler Optimization Pipeline

The compiler should have a serious optimization pipeline.

Potential stages:

AST
 ↓
Semantic analysis
 ↓
Vayu IR
 ↓
Constant folding
 ↓
Constant propagation
 ↓
Dead code elimination
 ↓
Inlining
 ↓
Devirtualization
 ↓
Escape analysis
 ↓
Allocation elimination
 ↓
Loop optimization
 ↓
Vectorization
 ↓
LLVM
 ↓
Machine optimization

Not every optimization must be implemented manually.

Use LLVM where it already provides excellent optimization.

---

20. LLVM Should Be Used Properly

LLVM should be treated as the native optimization/backend infrastructure, not merely as a way to emit basic machine code.

Use appropriate optimization levels for release builds.

Support something equivalent to:

vyc build --release program.vy

Release compilation can enable:

aggressive optimization
inlining
dead-code elimination
vectorization
LTO
target-specific optimization

where appropriate.

---

21. Target the Actual CPU

Vayu should eventually allow target-specific optimization.

For example:

vyc build --release --target=native program.vy

This allows the compiler/backend to use CPU features available on the target machine.

Potentially:

SSE
AVX
AVX2
AVX-512
AES
BMI
FMA

where supported.

Do not enable instructions that make the binary incompatible with the declared target.

---

22. SIMD and Vectorization

CPU-heavy operations should be capable of vectorization.

LLVM can perform automatic vectorization.

Do not manually write SIMD code everywhere.

Use:

high-level Vayu
 ↓
optimized IR
 ↓
LLVM vectorization
 ↓
SIMD machine code

Manual SIMD/intrinsics should only be introduced for proven hotspots.

---

23. Function Calls Should Be Cheap

Avoid unnecessary runtime dispatch.

When the compiler knows:

add(1, 2)

it should generate a direct call or inline it.

Do not use:

function name
 ↓
hash lookup
 ↓
runtime object
 ↓
dynamic call

unless dynamic behavior is explicitly required.

---

24. Generics Should Be Compile-Time Friendly

If generics are eventually added, avoid automatically forcing all generic operations through dynamic dispatch.

Where practical:

generic function
 ↓
specialization
 ↓
optimized native implementation

This can produce code specialized for the actual types.

Do not implement generics until the core language needs them.

---

25. Async Must Have Low Overhead

Eventually Vayu should support asynchronous programming.

Do not design async around heavyweight objects and expensive task creation.

Aim for:

cheap task
cheap suspension
cheap wake-up
efficient scheduler

But do not implement async before the synchronous native execution path is stable.

---

26. Concurrency Should Not Penalize Single-Threaded Programs

A program that never uses concurrency should not pay significant concurrency-runtime overhead.

Avoid:

every program
 ↓
thread pool
 ↓
scheduler
 ↓
synchronization infrastructure

when unnecessary.

Runtime components should be initialized only when needed where practical.

---

27. Avoid Hidden Work

Vayu should make expensive operations visible.

Avoid language constructs that silently perform:

heap allocation
copying
locking
reflection
network access
dynamic dispatch

unless their semantics require them.

The programmer should be able to reason about the cost of code.

---

28. Compile-Time Computation

Where safe and useful, allow the compiler to evaluate constant expressions.

Example:

x = 100 * 20 + 50

should not necessarily require runtime arithmetic.

Compile:

x = 2050

The same principle can eventually apply to more sophisticated compile-time computations.

---

29. Dead Code Must Disappear

Unused functions and unused runtime components should not unnecessarily remain in the final binary.

Use:

dead-code elimination
linker garbage collection
LTO

where appropriate.

---

30. Runtime Initialization Must Be Minimal

Do not make startup expensive.

A program such as:

print("Hello")

should not initialize:

HTTP subsystem
database subsystem
AI subsystem
async scheduler

unless needed.

Initialize only what is required.

---

31. Error Handling Must Be Predictable

Normal expected failures should not require extremely expensive mechanisms.

A result-based model can be considered:

Result<T, Error>

for operations where failure is expected.

Do not use exceptions as the universal control-flow mechanism.

If exceptions are eventually supported, optimize the common non-exception path.

---

32. Standard Library Must Be Native-Performance-Oriented

Do not write a high-level standard library that secretly introduces large overhead on top of native operations.

For important primitives:

strings
arrays
maps
files
bytes
math
I/O
HTTP
JSON

the implementation should be benchmarked.

Use optimized system libraries where appropriate rather than reinventing everything.

---

33. Do Not Reinvent High-Performance Libraries Without a Reason

If a mature native library is significantly faster and reliable, integrate it.

Do not implement your own:

cryptography
compression
BLAS
TLS
CPU primitives

just to say Vayu implemented them.

The objective is performance, not maximum amount of handwritten code.

---

34. Benchmark Everything

Create a permanent benchmark suite.

At minimum:

integer arithmetic
floating-point arithmetic
function calls
loops
arrays
maps
strings
string concatenation
memory allocation
memory copying
file I/O
JSON
HTTP
concurrency
startup time
binary size

Record:

latency
throughput
memory
allocations
CPU usage

Never optimize based solely on intuition.

---

35. Compare Against Real Implementations

Compare Vayu against:

Python
C
C++
Rust
Go

when appropriate.

Do not compare toy programs.

Use equivalent algorithms and equivalent compiler optimization settings.

For example:

same input
same algorithm
same output
same workload

Then measure.

---

36. Use Profiling Before Optimization

Every optimization cycle should be:

Benchmark
   ↓
Profile
   ↓
Find bottleneck
   ↓
Optimize
   ↓
Benchmark again
   ↓
Keep only if improvement is real

Do not optimize code that isn't actually a bottleneck.

---

37. Avoid Benchmark Cheating

Do not:

- remove work from the benchmark
- use different algorithms without documenting it
- exclude initialization selectively
- ignore memory allocation
- ignore compilation/runtime startup when it matters
- compare debug Vayu against optimized C++
- compare different workloads

Benchmarks must represent real programs.

---

38. Have Multiple Performance Modes

Eventually support:

Debug
Release
Release + Native CPU
Release + LTO
PGO

Example:

vyc build program.vy
vyc build --release program.vy
vyc build --release --target=native program.vy

Later:

vyc profile program.vy
vyc build --pgo program.vy

---

39. Don't Sacrifice Correctness for Micro-Optimizations

Never introduce undefined behavior merely to gain a few percent.

Bad:

unsafe memory access everywhere

Good:

safe default
+
explicit unsafe escape hatch
+
optimized compiler-generated code

Safety and performance are not mutually exclusive.

---

40. Do Not Optimize for "Looks Fast"

Avoid decisions such as:

«"C++ is fast, therefore this must be fast."»

or:

«"LLVM is used, therefore Vayu is automatically fast."»

Neither is true.

Actual performance depends on:

language semantics
+
type system
+
memory model
+
data structures
+
compiler IR
+
optimizer
+
runtime
+
generated machine code

---

41. The Ultimate Vayu Performance Pipeline

The intended final architecture should approach:

                  VAYU SOURCE
                       │
                       ▼
                    Lexer
                       │
                       ▼
                    Parser
                       │
                       ▼
                      AST
                       │
                       ▼
              Semantic Analysis
                       │
                       ▼
                   Vayu IR
                       │
             ┌─────────┴─────────┐
             │                   │
        Optimization        Analysis
             │                   │
             └─────────┬─────────┘
                       ▼
                 Optimized IR
                       │
                       ▼
                     LLVM
                       │
              ┌────────┴────────┐
              │                 │
          CPU target        Generic target
              │                 │
              ▼                 ▼
       Native machine code
              │
              ▼
        Small runtime
              │
              ▼
        Native executable

The final program should contain only what it actually needs.

---

42. Performance Philosophy

The fundamental philosophy of Vayu should be:

«Do expensive work only when necessary.»

And:

«If the compiler can prove something is unnecessary, remove it.»

And:

«If the compiler cannot prove it, measure it before optimizing it.»

And:

«Do not inherit overhead merely because another language does it that way.»

Vayu should take inspiration from Python's simplicity, but it should not inherit Python's runtime architecture.

---

43. Development Priority

Do NOT attempt to implement everything above immediately.

Use this order:

Stage 1 — Correct compiler

lexer
parser
AST
semantic analysis
IR

Stage 2 — Native execution

LLVM backend
native executable
basic optimizer

Stage 3 — Efficient primitives

integers
floats
strings
arrays
maps
functions
memory

Stage 4 — Runtime efficiency

allocation
ownership
copy elimination
runtime minimization

Stage 5 — Compiler optimization

inlining
constant folding
DCE
loop optimization
vectorization
LTO

Stage 6 — Concurrency

tasks
async I/O
scheduler
channels

Stage 7 — Higher-level libraries

HTTP
JSON
files
database

Only after these foundations are strong should Vayu begin adding specialized AI/RAG/agent capabilities.

---

44. Execution Order: Where the Milliseconds Actually Are

Grounded in the code as it exists today (C backend, measured numbers in
docs/PERFORMANCE.md). Every stage below lands only if `make test` stays
green and `benchmarks/run.sh` + `benchmarks/compare.sh` are re-run and the
new numbers recorded. This section exists because stages 1-43 say what to
believe, not what to do first.

Stage P0 -- make arithmetic inlineable (highest return, no language change)

  Status: LANDED and measured.

  The hot path for `a + b` used to be:
    codegen_c.cpp emits vy_add(a, b)          -- a real CALL
    vy_add lives out-of-line in value.c
    ~7 tag/branch tests (3x string, list, is_num x2, both_int)
    before reaching the int+int path
    libvyrt.a is built WITHOUT -flto (Makefile grep: 0 matches), so even a
    --release LTO build can never inline across the archive boundary.

  What was done:
  - vy_add/vy_sub/vy_mul/vy_div/vy_mod, vy_neg/vy_pos/vy_not, the bitwise
    ops, vy_eq, vy_cmp, vy_truthy and vy_is now have `static inline` fast
    paths in vyrt.h: both tags VY_INT -> vy_int(a.i <op> b.i); float likewise;
    otherwise call the corresponding `*_slow` body in value.c, so string
    coercion, list concat and nil handling are byte-for-byte unchanged.
  - vy_list_push/vy_list_get got the same treatment (fast path in vyrt.h,
    `*_slow` fallback in vyrt.c).
  - -flto on libvyrt.a was deliberately NOT added: vyc links the emitted C
    with clang while the runtime archive is built by g++, so a bitcode
    archive would not even be readable in every configuration. With the hot
    path in the header, LTO has nothing left to buy here; adding it would
    only introduce a toolchain coupling.

  Measured: intloop 819ms -> 184ms (4.4x, target was <=150ms); fib 10ms ->
  4ms (target met). Vayu vs Python on intloop went from 8.1x faster to 27x.

Stage P1 -- optimise what the emitter can see (spec 28, 8, 23)

  Status: LANDED (first two items) and measured.

  - emit-time constant folding: fold_const_binop/fold_const_unary in
    codegen_c.cpp fold int/float literals on Unary and Binary nodes, so
    `2 * 3 + n` emits `6 + n` instead of three runtime calls.
  - per-site pinned string literals: a literal now becomes a block-local
    `static` initialised once through vy_str_lit() and pinned with
    VY_HDR_PIN, so sweep() keeps it alive and a loop body referencing
    `"hello world "` stops allocating it 40k times.
  - emit small user functions as `static inline`: still open. fib is 243k
    calls; the win is smaller now that the call body is all inline ops.
  - keeping values in VyValue locals rather than re-materialising temporaries:
    the emitter already does this for most shapes; no further change.

  Measured: fib 10ms -> 4ms (within 12x of C's 0.34ms, not the <=2x target --
  remainder is the call itself plus boxed arithmetic, i.e. work P3 owns);
  startup unchanged at ~3-4ms.

Stage P2 -- kill per-operation allocation (spec 9, 11, 12, 16)

  Status: LANDED (all of it) and measured.

  - render-to-stack buffer for scalars: vy_render() assembled every scalar
    through a growable Buf (malloc 256 -> render -> copy into a fresh VyStr
    -> free) even for `str(42)`. Ints and floats are now rendered into a
    64-byte stack buffer with one allocation, and the int path uses a
    hand-rolled digit loop instead of snprintf("%lld"), which alone cost
    ~150ns of format parsing per call. nil/true/false are pinned singletons.
  - strings are no longer copied by vy_render: every VyStr in the runtime is
    immutable by construction (an audit of every `bytes[...] =` write shows
    they all target a freshly allocated block), so render returns the same
    object. This removed a per-append allocation from strconcat.
  - per-site pinned string literals (P1) removed the literal side of the
    same problem.
  - `xs.push(x)`: the emitter used to call vy_h_value_method, an out-of-line
    dispatch with a strcmp chain over the method name, once per push. The
    call site now emits a tag guard -- `if (vy_tagof(base) == VY_LIST)
    vy_list_push(...)` -- and falls through to the unchanged dispatch for
    any other base, so error behaviour is identical. Only emitted for
    arity <= 1, so every argument expression is still evaluated.
  - str_build geometric growth: superseded. The builder already grows
    geometrically, and the per-append allocation it was meant to remove is
    gone via string sharing above.

  Measured: mapops 982ms -> 191ms (5.1x; target was <=300ms). strconcat
  29ms -> 13ms and it now beats CPython's in-place append (13ms vs 16-27ms
  measured under load) instead of losing to it. listappend 58ms -> 33-35ms.

Stage P2b -- the runtime bugs a micro-benchmark decomposition found

  Method: instead of guessing why mapops was still slow, three throwaway
  programs isolated the parts (key construction only; map inserts with int
  keys and no allocation; a constant-key loop; a bare arithmetic loop). The
  decomposition said inserts were 600ns each with zero allocations -- so the
  cost was not allocation at all, and gprof then named the functions.

  1. The hash map only grew when it was 100% full. map_insert_slot returns
     NULL only when no free slot exists, so every table filled to load
     factor ~1.0 and, with a well-avalanched hash, the trailing insertions
     walked four- and five-figure probe chains each. The comment above
     map_insert_slot already claimed a 0.75 load factor that the code never
     enforced. vy_map_set now rehashes at 0.7; 100k ordered int keys went
     from 176ms to 89ms, and the whole mapops case from 5x slower than
     CPython to roughly parity/faster.

  2. sweep() subtracted freed bytes from live_bytes *after* collect had
     already zeroed and rebuilt it from the marked survivors, so live_bytes
     came out too low -- and next_gc is live*2 + 8MB, so the collector ran
     more often than the growth policy intended. Removed; heap_bytes was and
     is a running total, so it keeps its subtraction.

  3. mark_loop called mark_value for every list item and map key/value, and
     mark_value returned immediately for nil/bool/int/float. Marking a
     100k-entry map of ints cost 200k calls *per collection*. The tag enum
     orders the container kinds at VY_STRING, so a tag compare now replaces
     the call. b2_insonly (100k inserts, no lookup) went 63ms -> 51ms and
     listappend 39ms -> 33ms.

  The general lesson, recorded because it will recur: the first three
  findings were invisible in the source and obvious in three 6-line
  programs.

Stage P3 -- unboxed specialisation (spec 5, 6, 7 -- the "next level")

  Status: PARTIALLY LANDED.

  - Sema tracks `is_proven_int`/`is_proven_float` for literal initializations
  - Codegen emits raw `int64_t`/`double` locals instead of boxed `VyValue` for
    proven types
  - GC registration skipped for proven numeric locals
  - Fast arithmetic paths on raw locals (add/sub/mul/div/mod, comparisons, bitwise)
  - Specialized array types: `VyInt64Array`, `VyFloat64Array`, `VyStringArray`
    with contiguous buffers and tag-guarded fast paths
  - List literals of proven int/float now emit specialized arrays

  What remains: values flowing through lists, maps, and function parameters
  are still boxed. Full type-inference to reach every value is the larger
  remaining task.

  Verify + target: intloop within ~3-5x of C; no behavioural change (both
  backends must agree per tests/interp.sh).

Stage P4 -- make the document match the machine (spec 40)

  - the pipeline diagram in section 41 shows Vayu IR -> Optimizer -> LLVM IR;
    codegen_llvm.cpp is a 13-line stub and compiler/optimizer/ is empty.
    Either date that diagram as "target architecture" or remove it until the
    backend exists. The real backend today is: emitted C -> clang (which IS
    LLVM, so sections 19-22 are partially free already -- within one
    function; P0 is what buys back the cross-call part)
  - resolve sections 14/15 against reality: the runtime IS a mark-sweep GC
    plus refcounted strings; commit to that model, then optimise it
    (generational collection, bump allocation) instead of speculating about
    ownership systems (section 13 stays parked)
  - add the missing benchmark cases section 34 already demands: float
    arithmetic, sorting, closure calls -- and make every case report
    allocation counts, not just time
  - profiling tooling: `perf record` on a benchmark binary before ANY new
    optimisation lands (section 36 without a tool is a wish)

---

Final Requirement

Do not claim that Vayu is "high performance" merely because:

- the compiler is written in C++
- LLVM is used
- the language is compiled
- the binary is native

Performance must be demonstrated experimentally.

The project should continuously answer:

How fast is it?
How much memory does it use?
How many allocations occur?
Where is the bottleneck?
What changed after optimization?
Did the optimization actually improve real workloads?

The ultimate objective is:

Python-like productivity
        +
native compilation
        +
low runtime overhead
        +
efficient memory model
        +
aggressive compiler optimization
        +
efficient concurrency
        =
high-performance Vayu

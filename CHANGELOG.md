# Changelog

Everything that has happened to Ovyth, newest first. Versions follow
[semantic versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## 0.1.4 (in progress)

### Modules and imports — 9 Oct 2026

`import "path"` loads another `.ov` file and flattens its top-level
definitions into the program. Imports are resolved at load time,
relative to the importing file (with a `.ov`-suffix fallback when
the path has no extension), and parsed before the importer's own
statements, so a module's definitions and globals are in scope
wherever the import appears. Top-level names — functions and
globals — must be unique across every module and the entry file;
duplicates are an error naming both files. Import cycles are an
error, and a file imported more than once (directly or through a
diamond) is merged once. Both backends agree;
`tests/interp/020_modules.ov` covers the covered path.

Note: a module function that reads a top-level global still hits
the pre-existing native-backend limitation (top-level `let` is an
`ov_main` local, invisible to functions); pass such values as
arguments for now.

### Unboxed loop variables — 9 Oct 2026

A `for` loop over a specialized array (`range(...)` with int-literal
arguments, all-int or all-float list literals) now keeps its loop
variable in a raw C `int64_t`/`double` when the body keeps it raw —
no writes, no shadowing, no closure capture (a pre-scan decides per
loop; anything else falls back to the boxed loop). The body is lowered
once per element type: the `OvInt64Array` and `OvFloat64Array`
branches bind the variable unboxed, while lists, strings, maps and
string arrays keep the boxed loop. Reads box on demand at `OvValue`
use sites, so `print(i)` still works, while arithmetic stays in C
registers. A 20M-iteration `for i in range(0, 20000000)`
accumulation runs roughly 30% faster.
`tests/interp/018_unboxed_loops.ov` covers both backends.

### GC rooting for containers under construction — 9 Oct 2026

The native backend could corrupt the heap when a container was built
while an element expression allocated: `OvList`/`OvMap` are
GC-tracked, but the half-built container was not rooted, so a
collection triggered by an element (`[str(i)]`,
`[str(x) for x in range(...)]`, `[str(i), "b", "c"]` inside a loop)
could sweep it mid-build — `malloc(): unaligned tcache chunk` or a
segfault, with the interpreter unaffected. Containers are now
registered as roots while being filled, `for`-loop iterables (and
the character list a string iterates over) are rooted for the loop,
and a `try` block drops the roots its body registered when a panic
unwinds past them (the `longjmp` bypasses their restores).
`tests/interp/019_gc_roots.ov` covers both backends.

### Comprehension pre-scan stops at the matching bracket — 9 Oct 2026

`is_list_comprehension` scanned past the closing `]` of a list
literal into the rest of the file, so a `for` nested inside later
braces (any `{ }` at depth 1) made a plain list literal parse as a
comprehension: `for x in [10, 20, 30] { ... }` followed anywhere by
a nested loop failed with `expected ']' but found ','`. The scan now
stops at the bracket it started from.

### Calmer diagnostics — 9 Oct 2026

Errors, warnings, info and notes now render with a calm icon and
color on a terminal (`✖` red, `▲` yellow, `ℹ` cyan, `•` dim) and
keep the icons — minus escape codes — when piped, so scripts and CI
logs stay stable. A count summary and a single tip line follow the
diagnostics. The `debug` statement reports `ℹ ovyth: file:line` on
both backends.

Compiled programs also report a `throw` that escapes every `try`
(previously it exited silently with code 0 — the interpreter always
reported it, so this was a silent backend divergence). Both backends
now print the thrown value plus a `try`/`catch` tip and exit 70;
`exit(n)` remains silent with code `n`.

### Closures now capture — 9 Oct 2026

Closures were documented as unable to capture their enclosing scope, but the
interpreter already supported it and the native backend was mid-migration.
Four codegen bugs stood between the native backend and working capture; all
are fixed and both backends now agree (`tests/interp/017_closures.ov`):

- **Anonymous closure bodies were emitted after the code that referenced
  them.** A closure created inside a function lowered to C that used its
  `ov_anon_N` symbol before that symbol's forward declaration, so clang
  rejected the file with `use of undeclared identifier 'ov_anon_N'`. The
  function phase now lowers all bodies first, forward-declares every anon
  function, and only then emits the bodies.
- **Write-only captures were missed.** Free-variable analysis only looked at a
  `VarDecl`'s right-hand side, so a closure that only *assigned* to an outer
  local (`setter = function(x) { v = x }`) never captured `v`. The left-hand
  name is now treated as a free variable when it is not bound locally, which
  matches the interpreter's `Env::find` write-through.
- **Zero-argument calls emitted `(void)`.** `f()` for a user function lowered
  to `ov_fn_f(void)`, which is only legal in a declaration. Call sites now
  emit `()`.
- **A value call passed a compound literal as an argument.** Calling a closure
  held in a collection (`pair[0](...)`) lowered to
  `ov_h_call(f, { args }, n, ...)`; a `{...}` compound literal is not a valid
  function argument in C. The argv is now materialised into a named array
  first.

The interpreter could already call a closure pulled out of a list; its `Call`
path only resolved bare-identifier callees, so `pair[0]()` raised "not
callable". It now evaluates any non-member callee expression to a value.

### Benchmark integrity fixes — 8 Oct 2026


Re-running all three suites before rewriting the docs turned up four bugs
in the benchmarks themselves. The numbers we had been publishing were
wrong, in both directions:

- **`intloop` was a loop the optimiser had deleted.** At `-O2` and above,
  clang replaces the affine recurrence `total = total + i * 3 - 1` with
  its closed-form value — in plain C as well as in generated Ovyth code.
  The binary literally contained `movabs $599999950000000` where the loop
  used to be, which is how the case reported an impossible 4ms for 20
  million iterations. The case now uses `total + (i ^ (i >> 3)) - 1` —
  same shape, no closed form — identically in `cases/intloop.ov`,
  `ref_c.c`, `ref_python.py` and `bench.ov`, and all three agree on
  `202067735460992`.
- **The C reference carried a `volatile` handicap.** Its trip count was
  `volatile`, which blocks vectorisation and forces the loop counter
  through memory on every iteration. It is a plain literal now (the
  `listappend` case keeps its `volatile`, deliberately — that body is
  still foldable).
- **The AI suite ran on a 1-second timer.** `bench_ai.ov` called `now()`
  everywhere, which has one-second resolution, so every case rounded to
  0ms or 1000ms and two "~100% faster" claims were pure artefact. All
  timing now uses `time.clock()`, and `ref_ai.c` prints two decimals
  instead of zero.
- **`compare_ai.sh` mangled its own output.** Ratios displayed as
  "0.6x faster" for cases that were slower, and dividing by a 0ms Python
  time produced `inf`. The script now says "1.7x faster" or "3.0x
  slower" as appropriate and prints `n/a` when it cannot divide.

One paper cut: Ovyth's column in `compare.sh` is whole-process wall clock
while C and Python time their cases internally, so Ovyth carries ~4ms of
process start. That note now lives in the script's output instead of
quietly in the docs.

### Fresh benchmark numbers — 8 Oct 2026

Best-of-3, same algorithms and inputs on all three sides, result columns
verified rather than assumed:

```
case         Ovyth (wall)       C -O3      Python
--------------------------------------------------
intloop              22ms    12.04ms     6827ms   1.8x vs C, 310x vs Py
fib                   6ms     0.41ms       32ms
strconcat            14ms     0.62ms        9ms
listappend           37ms     0.20ms       91ms   185x vs C, 2.5x vs Py
mapops              194ms    82.46ms      158ms
```

Headline: `intloop` runs within 1.8x of `-O3` C and 310x faster than
CPython; `listappend` is still 185x behind C because values are boxed.
The AI suite (`compare_ai.sh --release`) has Ovyth beating Python in 5 of
7 cases — `multi_parse` the biggest win at 14.5x — and losing
`context_build` by 3.0x. `run.sh` reports a 6ms startup and an 18KB
hello-world binary.

### Documentation rewritten from scratch — 8 Oct 2026

Every document was rewritten to say plainly what is true:

- `README.md` — headline numbers refreshed, known limitations kept honest.
- `docs/USAGE.md` — the full language and stdlib reference, reorganised.
- `docs/PERFORMANCE.md` — what the benchmarks actually show, including
  where Ovyth loses.
- `docs/PERFORMANCE-SPEC.md` — the 44-section spec, now written as prose,
  with the pipeline diagram dated as target architecture rather than
  current reality.
- `benchmarks/RESULTS.md` / `RESULTS_AI.md` — fresh tables, the six
  benchmark bugs we have found and fixed, and what has not been done.

No compiler or runtime code changed for the rewrite itself.

### Specialised array types

Containers whose elements are all of one proven type now get a real
array instead of a generic boxed list:

- **`OvInt64Array`** — contiguous `int64_t` buffer, O(1) push/pop/get/set
- **`OvFloat64Array`** — contiguous `double` buffer, same fast paths
- **`OvStringArray`** — contiguous `OvStr*` buffer for strings

A literal like `[1, 2, 3]` compiles to a specialised array when the
compiler can prove every element's type. The types are transparent to
programs: they render as `[1, 2, 3]`, iterate in `for` loops and
comprehensions, and unpack in tuple destructuring — all through the same
tag-guarded fast paths used elsewhere.

Under the hood: `ov_i64a_new_cap`, `ov_f64a_new_cap` and
`ov_stra_new_cap` with 2x growth, tag guards for push/get/pop/set in the
hot path, and `_slow` fallbacks for reallocation. Codegen emits the
specialised form from `ListLit`, dispatches on all six tags in `emit_for`
and `emit_listcomp`, and `ov_render` displays them as ordinary lists.

### Unboxed numeric locals

Locals the compiler has proven to be int or float now emit as raw
`int64_t`/`double` in the generated C:

- no `OvValue` boxing or unboxing for proven int/float variables
- no GC root registration for them
- fast arithmetic on raw locals: add, sub, mul, div, mod, comparisons,
  bitwise
- inlined builtins: `len`, `sum`, `abs`, `sqrt`, `floor`, `ceil`, `round`,
  `min`, `max`, `range`

This is what puts `intloop` within 1.8x of C. Values that flow through
lists, maps and function parameters are still boxed — that is the next
stage (P3 in `docs/PERFORMANCE-SPEC.md`).

### Test results

All 32 regression tests pass through both backends (interpreter and
native), and the runtime self-test reports zero failures.

## 0.1.3 (benchmark re-check, 7 Oct 2026)

Re-ran all three suites on Linux x86-64 and corrected every published
number. No code changed — it was a docs-and-numbers sync.

*Note: a second re-check the next day found the `intloop` case itself was
broken (see 0.1.4), so the tables below are a dated snapshot, not the
current headline.*

### Core suite (`compare.sh` / `run.sh`)

Best-of-3 wall clock; `intloop` varies 193-275ms run to run under ±20-40%
desktop noise:

```
case         Ovyth (wall)       C -O3      Python
--------------------------------------------------
intloop             237ms    42.14ms     5612ms
fib                   7ms     0.37ms       30ms
strconcat            14ms     0.81ms        9ms
listappend           34ms     0.11ms       86ms
mapops              187ms    64.99ms      156ms
```

- Against Python the range was 2.5x-24x (had been 2.6x-26x): `fib`
  re-measured at 4.3x rather than 8x, `mapops` at 1.2x slower rather than
  parity.
- Against C the range was 2.9x-309x (had been 4.5x-378x).
- `run.sh`'s `intloop` best moved 193ms → 275ms — run-to-run noise, not a
  regression — and binaries grew (`intloop` 26,952 → 31,048 bytes,
  `json` 35,168 → 39,264) from the v0.1.2 inline path specialisation.
- Updated everywhere the old figures appeared: `README.md`,
  `docs/PERFORMANCE.md`, `benchmarks/RESULTS.md`.

### AI suite (`compare_ai.sh --release`) — two bad claims corrected

- The claim "context assembly 1000ms → 0ms (~100% faster)" was wrong.
  Fresh runs put `context_build` at 1000-2000ms against Python's
  ~465-487ms — 2.1-4.3x slower, not faster. The `0ms` was the
  one-second-resolution timer rounding real work down to nothing (fixed
  properly in 0.1.4).
- `hash_map_str` was reported at ~50x slower than C with Python in
  between; it actually re-measured near parity, from order-insensitive
  hashing. Also: `string_scan` beat `strstr` at -O0, but `strstr` wins at
  `-O3` once GCC turns it into SSE4.2 `pcmpestri`.

## 0.1.2 (1 Oct 2026)

### Compiler

- More type inference and provability through `sema`: for-loop sequences,
  while statements, `+=`/`-=`/`*=`/`/=`/`%=`, unary +/-, and `break`/`continue`
  statements all feed the proven-type lattice.
- Intrinsic fast paths in `codegen_c.cpp` for `len`, `str`, `int`, `float`,
  `abs`, `sqrt`, `min`, `max`, `range`, `push`, `pop` — several on the
  interpreter side too.
- `import` rejected with a proper diagnostic instead of a generic parse
  error; float literal validation before the number lexer; stable
  source-order function evaluation.
- `ovc ast` prints types; `ovc fmt` canonicalises string escapes.

### Runtime

- `push`/`pop` inline fast paths (`ov_list_push_fast`, `ov_list_pop_fast`).
- String interning for literals: exact-byte reuse, hash-indexed lookup,
  pinned in gen-0 so short-lived compilations do not accumulate.
- Zero-copy string slices: `ov_str_slice` shares the parent and pins it.
  `upper`/`lower` walk the parent's bytes.
- `ov_str_concat_reserve` for one-allocation concatenation; random-access
  `len`; `gc("stats")` counts collections.

### Tests

- 32 language tests run through both backends; `tests/interp.sh` asserts
  interpreter output equals golden `.want` files and equals native output.
- Runtime self-test target; Makefile `test` target.

### Benchmarks

- `benchmarks/` gained `cases/*.ov`, `ref_ai.c`, `ref_ai.py`,
  `bench_ai.ov`, and the three comparison scripts.

## 0.1.1 (25 Sep 2025)

### AI Pipeline

- AI benchmark suite: `benchmarks/bench_ai.ov`, reference implementations
  in `ref_ai.c` and `ref_ai.py`, and `compare_ai.sh` to run all three
  side by side. Covers JSON parse, nested access, context assembly,
  chunking, hash maps, multi-parse and string scanning.

### Runtime improvements

- Fast JSON parser (`runtime/src/json_fast.c`) — fewer allocations while
  parsing JSON strings.
- String builder (`runtime/include/ov_sb.h`) — reusable buffer for
  incremental string building.
- Arena allocator (`runtime/src/arena.c`) — request-scoped memory with
  O(1) teardown.
- HTTP connection pooling (`runtime/src/http_pool.c`) — reuse connections
  across requests.

### Documentation

- README, this changelog, and `docs/PERFORMANCE.md` brought up to date
  with the AI pipeline work and the GC improvements.

## 0.1.0

First release.

### Language

- Dynamic values with inference: `Int`, `Float`, `Bool`, `null`, `String`,
  `List`, `Map`, tuples.
- `if` / `else if` / `else`, `while`, `for ... in`, `break`, `continue`.
- `function` declarations with named arguments, and `function(x) { ... }`
  literals.
- String interpolation (`"hi {name}, n={1 + 2}"`); invalid braces stay
  literal, so inline JSON is safe.
- `try` / `catch` with catchable runtime errors, plus `throw` and `assert`.
- List comprehensions, slices, and destructuring.

### Standard library

- **Strings** — `upper` `lower` `trim` `split` `join` `contains` `replace`
  `indexof` `repeat` `startswith` `endswith` `pad` `chars` `bytes`.
- **Lists** — `push` `pop` `insert` `remove` `sort` `reverse` `contains`
  `indexof` `extend` `length`.
- **Maps** — `get` `set` `has` `delete` `keys` `values` `items` `count`
  `merge`.
- **Math** — `abs` `sqrt` `floor` `ceil` `round` `min` `max` `sum` `pow`.
- **JSON** — `json.parse` `json.stringify` `json.valid`.
- **HTTP** — `http.get` `http.post` `http.put` `http.delete`, with `json=`
  and `headers=` named arguments.
- **Environment** — `env` (throws when unset), `env_or` (falls back).
- **Misc** — `type` `str` `int` `float` `bool` `range` `len` `time.clock`
  `time.now` `gc` `input` `exit`.

### Toolchain

- `ovc prog.ov` — compile to a native executable; the runtime is linked
  in, so the result needs nothing but libc.
- `ovc run prog.ov` — tree-walking interpreter for a fast edit/run loop.
- `ovc init <dir>` — scaffold a project (`hello`, `http`, `cli`
  templates) with a `main.ov`, `Makefile`, `README.md` and `.gitignore`.
- `ovc check` / `ast` / `tokens` / `fmt` — diagnostics and canonical
  formatting.
- `--release` (`-O3`, LTO, strip) and `--target=native` (`-march=native`).
- `make install` and `./install.sh` — install `ovc`, `libovrt.a` and the
  headers into a prefix so `ovc` works from anywhere.

### Runtime

- Tracing garbage collector with `gc()` / `gc("stats")` / `gc("reset")`.
- `http.*` is linked lazily: a program that never makes a request does not
  carry libcurl.
- A compiled `print("hi")` binary is about 18 KB and starts in about 5 ms.

### Known limitations

- Values are boxed, so tight numeric loops are much slower than C.
- One file per program — no modules, imports, or file I/O yet.
- No async or concurrency.

Closures are no longer a limitation: they capture their enclosing scope by
reference on both the interpreter and the native backend (read, write, and
shared capture all work), pinned down by `tests/interp/017_closures.ov`.


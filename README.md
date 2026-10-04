# Vayu

A small compiled language for automation and AI tooling. You write `.vy` files,
`vyc` compiles them to ordinary native executables — no interpreter, no VM, no
runtime to install on the target machine.

```vayu
name = "Vayu"
print("hello from", name)
```

---

## Is it fast?

Honest answer: **2.6x–26x faster than Python on loops, recursion and list
building; still 1.2x–1.6x slower than Python on string building and map churn;
4.5x–378x slower than C** — and the numbers are measured and published, not
guessed.

| workload | vs Python | vs C |
|---|---|---|
| arithmetic loops (`intloop`) | **~26x faster** | ~4.5x slower |
| function calls (`fib(25)`) | **~8x faster** | ~12x slower |
| list building (`listappend`) | **~2.6x faster** | ~378x slower |
| hash maps (`mapops`) | ~1.2x slower (was 6x) | ~2.8x slower |
| string building (`strconcat`) | ~1.6x slower (was 2.9x) | ~21x slower |
| startup of a compiled binary | **~5 ms** | — |
| binary size (`print("hi")`) | 18 KB | — |

The gap with C has one dominant cause left: every value is still a boxed,
tagged `VyValue`, so a loop variable is a 16-byte struct rebuilt per iteration
instead of a register. Arithmetic no longer *calls* out of line — those fast
paths are `static inline` in `runtime/include/vyrt.h`. See
[docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the measured tables, the six
bugs the benchmarks exposed, and what will close the rest of the gap.

---

## Setup

**Vayu targets Linux** (x86-64). The compiler resolves the runtime archive and
headers through `/proc/self/exe`, so a build on macOS or Windows will not find
them yet. There is no prebuilt binary to download — you build it from source,
which takes about a minute.

On Debian/Ubuntu:

```bash
sudo apt install build-essential clang libcurl4-openssl-dev libssl-dev zlib1g-dev llvm-dev
```

Then build the compiler and runtime:

```bash
make            # builds build/vyc and build/libvyrt.a
make test       # runs the full test suite (do this once — it should pass)
./build/vyc version   # -> vyc 0.1.0 (Vayu)
```

That's the whole install — Vayu has no package manager and no dependencies to
fetch.

To put `vyc` on your `PATH` instead of typing `./build/vyc` every time:

```bash
./install.sh                 # -> ~/.local/bin/vyc        (no sudo)
./install.sh /usr/local      # -> /usr/local/bin/vyc      (needs sudo)
DESTDIR=/tmp/pkg ./install.sh /usr/local   # stage a package tree
make install PREFIX=$HOME/.local   # the same thing via make
```

The installer places the compiler, the runtime archive and the headers into the
prefix, laid out so `vyc` finds them on its own — no environment variables, no
`pkg-config`, and the prefix stays relocatable.

---

## Create a new project

```bash
vyc init myapp
cd myapp
make run          # interpreter — instant
make build        # native executable at build/app
```

`vyc init` writes a `main.vy`, a `Makefile`, a `README.md` and a `.gitignore`,
so the project builds straight away and is ready to `git init`. Pick a starter
with `--template`:

| template | what you get |
|---|---|
| `hello` (default) | a function, a loop, and a greeting |
| `http` | call an HTTP API, read the response, parse the JSON |
| `cli` | an interactive command loop reading `input()` |

```bash
vyc init scraper --template=http
vyc init chat -t cli
vyc init --help
```

It refuses to touch a directory that already contains files, so running it twice
is safe.

---

## Your first program

Create `hello.vy`:

```vayu
name = "Sehaj"
age = 18
print(name, age)
```

Run it:

```bash
./build/vyc run hello.vy      # interpreter — instant, best while developing
./build/vyc hello.vy          # compile to ./hello (a native executable)
./hello                       # run it
```

Ship the binary by copying it — it needs nothing but the same libc.

---

## The language in 60 seconds

```vayu
# values — no type annotations needed, the compiler infers
count = 42
ratio = 0.75
name  = "Vayu"
items = [1, 2, 3]
cfg   = {"port": 8080}

# control flow
if count > 100 {
    print("big")
} else {
    print("small")
}

for item in items { print(item) }

# functions, named arguments, closures
function add(a, b) { return a + b }
print(add(b: 20, a: 10))          # 30

double = function(x) { return x * 2 }

# string interpolation — evaluates {expressions}, leaves invalid ones literal
print("hi {name}, math={1 + 2 * 3}")   # hi Vayu, math=7

# errors
try {
    throw "boom"
} catch e {
    print("caught:", e)
}
```

The full tour — lists, comprehensions, slices, tuples, the whole stdlib — is in
[docs/USAGE.md](docs/USAGE.md).

---

## The `vyc` command

| command | what it does |
|---|---|
| `vyc init <dir>` | create a new project from a template |
| `vyc run prog.vy` | run with the interpreter (instant, good while coding) |
| `vyc prog.vy` | compile to `./prog`, a native executable |
| `vyc prog.vy --release` | optimised: `-O3`, LTO, symbols stripped |
| `vyc prog.vy --release --target=native` | also tune for this CPU |
| `vyc check prog.vy` | parse and type-check, produce no binary |
| `vyc fmt prog.vy` | print canonically formatted source |
| `vyc ast` / `vyc tokens` | dump the syntax tree / token stream |

A typical loop while developing:

```bash
vyc check prog.vy     # syntax/type check, instant
vyc run prog.vy       # try it
vyc prog.vy --release -o prog    # build the real thing
./prog
```

---

## Documentation

| document | what's in it |
|---|---|
| [docs/USAGE.md](docs/USAGE.md) | **usage guide** — install, language tour, stdlib reference, errors |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | **how fast is Vayu** — measured numbers, vs C and Python, why |
| [docs/PERFORMANCE-SPEC.md](docs/PERFORMANCE-SPEC.md) | the performance engineering specification |
| [benchmarks/RESULTS.md](benchmarks/RESULTS.md) | raw benchmark output and what each optimisation bought |
| [CONTRIBUTING.md](CONTRIBUTING.md) | build, test, style, and how the two-backend rule works |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each release |

---

## Running everything

```bash
make test                              # unit + both-backend regression
bash tests/interp.sh                   # language tests only
make examples                          # run every offline example
make chatbot                           # chatbot vs. the bundled mock server
bash benchmarks/run.sh                 # benchmark suite
bash benchmarks/compare.sh             # Vayu vs C vs Python
make install PREFIX=$HOME/.local       # put vyc on your PATH
```

---

## Known limitations

- **Closures don't capture their enclosing scope.** A closure reading an outer
  local is rejected with an error rather than silently miscompiled.
- **Every value is boxed.** Arithmetic goes through a runtime call — tight
  numeric loops are much slower than C (see [docs/PERFORMANCE.md](docs/PERFORMANCE.md)).
- **No files, modules, or imports yet.** One file per program.
- **No async or concurrency.**

---

## Project layout

```
compiler/          the vyc compiler (C++17)
  lexer/ parser/ ast/ sema/   the front end
  backend/           the interpreter and the C code generator
  tools/             vyc fmt and vyc init
runtime/           the C runtime that compiled programs link against
  include/vyrt.h     the C API: values, strings, lists, maps, GC
  src/*.c            GC, strings, containers, JSON, HTTP, console
tests/             language tests, run through BOTH backends
benchmarks/        benchmark suite and comparison driver
examples/          runnable examples, including chatbot/
docs/              usage guide and performance documents
Makefile           build, test, install
install.sh         install vyc + libvyrt.a + headers into a prefix
```

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The short version: `make test` runs
every program through **both** backends and fails if they disagree, so a feature
has to land in the interpreter and the native backend together.

---

## License

No license has been chosen yet, so all rights are reserved. Add a `LICENSE`
file (MIT and Apache-2.0 are the usual picks) before publishing.

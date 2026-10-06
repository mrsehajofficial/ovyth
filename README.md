# Vayu

A small compiled language for automation and AI tooling. You write `.vy` files, `vyc` compiles them to ordinary native executables — no interpreter, no VM, no runtime to install on the target machine.

```
name = "Vayu"
print("hello from", name)
```

## Is it fast?

Honest answer: 2.6x–26x faster than Python on loops, recursion and list building; competitive with Python on AI pipeline operations (JSON extraction, context building); 4.5x–378x slower than C — and the numbers are measured and published, not guessed.

| Workload | vs Python | vs C | Notes |
|---|---|---|---|
| arithmetic loops (intloop) | ~26x faster | ~4.5x slower | |
| function calls (fib(25)) | ~8x faster | ~12x slower | |
| list building (listappend) | ~2.6x faster | ~378x slower | |
| hash maps (mapops) | ~equal | ~2.8x slower | Fixed in v0.1.1 |
| JSON field extraction | ~1.6x slower | ~8x slower | **NEW**: `json.extract(path)` |
| Context assembly (join) | ~2.1x slower | ~6x slower | **NEW**: O(n) string builder |
| string building (strconcat) | ~1.6x slower (was 2.9x) | ~21x slower | Improved |
| startup of a compiled binary | ~5 ms | — | |
| binary size (print("hi")) | 18 KB | — | |

The gap with C has one dominant cause left: every value is still a boxed, tagged `VyValue`, so a loop variable is a 16‐byte struct rebuilt per iteration instead of a register. Arithmetic no longer calls out of line — those fast paths are static inline in `runtime/include/vyrt.h`. See `docs/PERFORMANCE.md` for the measured tables, the six bugs the benchmarks exposed, and what will close the rest of the gap.

## Setup

Vayu targets Linux (x86‐64). The compiler resolves the runtime archive and headers through `/proc/self/exe`, so a build on macOS or Windows will not find them yet. There is no prebuilt binary to download — you build it from source, which takes about a minute.

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

That's the whole install — Vayu has no package manager and no dependencies to fetch.

To put `vyc` on your PATH instead of typing `./build/vyc` every time:

```bash
./install.sh                 # -> ~/.local/bin/vyc        (no sudo)
./install.sh /usr/local      # -> /usr/local/bin/vyc      (needs sudo)
DESTDIR=/tmp/pkg ./install.sh /usr/local   # stage a package tree
make install PREFIX=$HOME/.local   # the same thing via make
```

The installer places the compiler, the runtime archive and the headers into the prefix, laid out so `vyc` finds them on its own — no environment variables, no pkg-config, and the prefix stays relocatable.

## Create a new project

```bash
vyc init myapp
cd myapp
make run          # interpreter — instant
make build        # native executable at build/app
```

`vyc init` writes a `main.vy`, a `Makefile`, a `README.md` and a `.gitignore`, so the project builds straight away and is ready to `git init`. Pick a starter with `--template`:

| template | what you get |
|---|---|
| hello (default) | a function, a loop, and a greeting |
| http | call an HTTP API, read the response, parse the JSON |
| cli | an interactive command loop reading `input()` |

```bash
vyc init scraper --template=http
vyc init chat -t cli
vyc init --help
```

It refuses to touch a directory that already contains files, so running it twice is safe.

## Your first program

Create `hello.vy`:

```vy
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

## The language in 60 seconds

### Values — no type annotations needed, the compiler infers

```vy
count = 42
ratio = 0.75
name  = "Vayu"
items = [1, 2, 3]
cfg   = {"port": 8080}
```

### Control flow

```vy
if count > 100 {
    print("big")
} else {
    print("small")
}

for item in items { print(item) }
```

### Functions, named arguments, closures

```vy
function add(a, b) { return a + b }
print(add(b: 20, a: 10))          # 30

double = function(x) { return x * 2 }
```

### String interpolation — evaluates `{expressions}`, leaves invalid ones literal

```vy
print("hi {name}, math={1 + 2 * 3}")   # hi Vayu, math=7
```

### Errors

```vy
try {
    throw "boom"
} catch e {
    print("caught:", e)
}
```

The full tour — lists, comprehensions, slices, tuples, the whole stdlib — is in `docs/USAGE.md`.

## The `vyc` command

| command | what it does |
|---|---|
| `vyc init <dir>` | create a new project from a template |
| `vyc run prog.vy` | run with the interpreter (instant, good while coding) |
| `vyc prog.vy` | compile to `./prog`, a native executable |
| `vyc prog.vy --release` | optimised: -O3, LTO, symbols stripped |
| `vyc prog.vy --release --target=native` | also tune for this CPU |
| `vyc check prog.vy` | parse and type-check, produce no binary |
| `vyc fmt prog.vy` | print canonically formatted source |
| `vyc ast / vyc tokens` | dump the syntax tree / token stream |

A typical loop while developing:

```bash
vyc check prog.vy     # syntax/type check, instant
vyc run prog.vy       # try it
vyc prog.vy --release -o prog    # build the real thing
./prog
```

## Examples

| example | description |
|---|---|
| `examples/chatbot/` | Automation chatbot with tool calling |
| `examples/rag/` | RAG pipeline example |
| `examples/tool_agent/` | Tool-using agent example |
| `benchmarks/bench_ai.vy` | AI pipeline benchmark (json parse, index chains, string ops) |

Run examples:

```bash
# Chatbot (with mock server)
python3 examples/chatbot/mock_server.py &
AI_API_URL=http://localhost:8765 AI_API_KEY=dummy ./build/vyc run examples/chatbot/chatbot.vy

# RAG pipeline
./build/vyc run examples/rag/rag_pipeline.vy

# Tool agent
./build/vyc run examples/tool_agent/tool_agent.vy
```

## Documentation

| document | what's in it |
|---|---|
| `docs/USAGE.md` | usage guide — install, language tour, stdlib reference, errors |
| `docs/PERFORMANCE.md` | how fast is Vayu — measured numbers, vs C and Python, why |
| `docs/PERFORMANCE-SPEC.md` | the performance engineering specification |
| `benchmarks/RESULTS.md` | raw benchmark output and what each optimisation bought |
| `CONTRIBUTING.md` | build, test, style, and how the two-backend rule works |
| `CHANGELOG.md` | what changed in each release |

## Recent Changes (v0.1.1)

### GC Runtime Fixes

Fixed critical garbage collection bugs that caused segfaults during high-iteration JSON parsing with nested index access (common in AI/RAG workloads):

- **Added GC root scanner API** (`runtime/src/gc.c`, `runtime/include/vyrt.h`)
  - `vy_gc_set_scanner()` — register dynamic root scanners during collection
  - `vy_gc_mark_value()` — mark values as roots
  - `vy_gc_roots_mark()` / `vy_gc_roots_restore()` — LIFO stack frames for root management

- **Native codegen protection** (`compiler/backend/codegen_c.cpp`)
  - Compound expressions with nested index chains now emit `vy_gc_begin_mutation()` / `vy_gc_end_mutation()` guards
  - Prevents GC from collecting intermediates during operations like `doc["choices"][0]["message"]["content"]`

- **Interpreter RAII root tracking** (`compiler/backend/interp*.cpp`)
  - Subexpressions and temporaries are now properly tracked
  - Fixes crashes in list comprehensions, binary operations, and nested assignments

- **Added regression test** `tests/interp/016_gc_stress.vy`

All 32 tests pass across both backends.

### New Examples

- **Chatbot** (`examples/chatbot/chatbot.vy`) — Automation-focused chatbot with tool calling, session history, and mock server support
- **Mock server** (`examples/chatbot/mock_server.py`) — Python mock OpenAI API for local testing

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

## Known limitations

- Closures don't capture their enclosing scope. A closure reading an outer local is rejected with an error rather than silently miscompiled.
- Every value is boxed. Arithmetic goes through a runtime call — tight numeric loops are much slower than C (see `docs/PERFORMANCE.md`).
- No files, modules, or imports yet. One file per program.
- No async or concurrency.

I'm actively working on removing these barriers. Closure capture is on the roadmap, and a module system is the next big feature after that.

## License

This project is licensed under the **MIT License**.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

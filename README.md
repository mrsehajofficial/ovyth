# Vayu

A small compiled language for automation and AI tooling. You write `.vy`
files, `vyc` compiles them to ordinary native executables — no interpreter,
no VM, no runtime to install on the target machine. The binary just runs.

```vy
name = "Vayu"
print("hello from", name)
```

## Is it fast?

Measured, not guessed: **Vayu beats Python by 2.5x–310x on loops,
recursion and list building, beats CPython on five of the seven AI-pipeline
cases (up to 14.5x), stays within 1.2x–1.6x of Python even where it loses,
and runs within 1.8x of `-O3 -march=native` C on a tight integer loop.
Where it is slow, it is slow honestly — up to 185x behind C when every
operation allocates.** Startup is ~6ms and a hello-world binary is 18 KB.

| Workload | vs Python | vs C | Notes |
|---|---|---|---|
| integer loop, 20M iterations | **310x faster** | 1.8x slower | proven `int` locals emit as raw `int64_t` |
| function calls (`fib(25)`) | **5.3x faster** | 14.6x slower | recursion |
| list building (500k `push`) | **2.5x faster** | 185x slower | GC per push — the boxing tax |
| hash maps (100k insert + read) | 1.2x slower | 2.4x slower | within noise of CPython |
| string building (40k appends) | 1.6x slower | 22.6x slower | the last case Python wins |
| JSON field extraction (50k) | **1.7x faster** | 2.9x slower | path-based, no AST parse |
| repeated JSON access (500k) | **2.1x faster** | C's row is a cached loop | different algorithms — see PERFORMANCE §7 |
| context assembly (100k joins) | 3.0x slower | 9.4x slower | O(n) string builder |
| string scan (1M scans) | **2.0x faster** | sub-ms in C | |
| multi-parse (20k tool JSONs) | **14.5x faster** | 2.7x slower | `strtoll` fast accessors |
| startup of a compiled binary | ~6 ms | — | no VM to boot |
| binary size (`print("hi")`) | 18 KB | — | dead-code-stripped runtime |

Numbers: `bash benchmarks/compare.sh` + `bash benchmarks/compare_ai.sh
--release`, 8 Oct 2026, best-of-3 on a 4-core desktop. Desktop variance is
±20–40% run to run, so treat the ratios as orders of magnitude. Every row
is checked — the driver refuses to print a timing unless Vayu's answer
equals C's and Python's. Full tables, the caveats, and the story of the
benchmark the optimiser deleted are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

The C gap has one dominant cause left: values that flow through lists, maps
and function parameters are still boxed 16-byte tagged structs instead of
register-resident ints. Loop locals that the compiler can prove are already
unboxed — which is what put `intloop` within 1.8x of clang. Closing the rest
is tracked in [docs/PERFORMANCE-SPEC.md](docs/PERFORMANCE-SPEC.md).

## Setup

Vayu targets Linux (x86-64). The compiler resolves the runtime archive and
headers through `/proc/self/exe`, so a build on macOS or Windows will not
find them yet. There is no prebuilt binary to download — you build it from
source, which takes about a minute.

On Debian/Ubuntu:

```bash
sudo apt install build-essential clang libcurl4-openssl-dev libssl-dev zlib1g-dev llvm-dev
```

Then build the compiler and runtime:

```bash
make            # builds build/vyc and build/libvyrt.a
make test       # runs the full test suite (it should pass — if not, that's a bug worth reporting)
./build/vyc version   # -> vyc 0.1.3 (Vayu)
```

Install to PATH:

```bash
./install.sh                 # ~/.local/bin/vyc (no sudo)
./install.sh /usr/local      # /usr/local/bin/vyc (needs sudo)
make install PREFIX=$HOME/.local
```

The installer copies three things: `bin/vyc`, `lib/libvyrt.a`,
`include/vyrt.h`. `vyc` finds the last two relative to its own path — no
environment variables, no pkg-config, relocatable.

## Your first program

```bash
cat > hello.vy << 'EOF'
name = "Vayu"
print(name)
EOF

./build/vyc run hello.vy      # interpreter — instant, best while developing
./build/vyc hello.vy          # compile to ./hello (native executable)
./hello                       # run it
```

Ship the binary by copying it — it only needs the same libc.

## The language in 30 seconds

```vy
# Values — no type annotations needed, the compiler infers
count = 42
ratio = 0.75
name  = "Vayu"
items = [1, 2, 3]
cfg   = {"port": 8080}

# Control flow
if count > 100 { print("big") } else { print("small") }
for item in items { print(item) }

# Functions, named arguments, closures
function add(a, b) { return a + b }
print(add(b: 20, a: 10))      # 30

double = function(x) { return x * 2 }

# String interpolation — evaluates {expressions}, leaves invalid ones literal
print("hi {name}, math={1 + 2 * 3}")   # hi Vayu, math=7

# Errors
try { throw "boom" } catch e { print("caught:", e) }
```

The full tour — lists, comprehensions, slices, tuples, the stdlib — is in
[docs/USAGE.md](docs/USAGE.md).

## The `vyc` command

| command | what it does |
|---|---|
| `vyc init <dir>` | create a new project from a template |
| `vyc run <file.vy>` | run with the tree-walking interpreter |
| `vyc <file.vy>` | compile to a native executable |
| `vyc <file.vy> --release` | optimized: -O3, LTO, stripped |
| `vyc <file.vy> --target=native` | optimize for this CPU |
| `vyc check <file.vy>` | parse + type-check only |
| `vyc ast <file.vy>` | dump the AST |
| `vyc tokens <file.vy>` | dump the token stream |
| `vyc fmt <file.vy>` | canonical formatting (stdout) |
| `vyc version` | print version |

## Examples

```bash
# Chatbot with mock server
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
| [docs/USAGE.md](docs/USAGE.md) | usage guide — install, language tour, stdlib reference, errors |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | how fast Vayu is — measured numbers, vs C and Python, and why |
| [docs/PERFORMANCE-SPEC.md](docs/PERFORMANCE-SPEC.md) | the performance engineering specification |
| [benchmarks/RESULTS.md](benchmarks/RESULTS.md) | raw benchmark output and what each optimization bought |
| [CONTRIBUTING.md](CONTRIBUTING.md) | build, test, style, and the two-backend rule |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each release |

## Run everything

```bash
make test                              # unit + both-backend regression
bash tests/interp.sh                   # language tests only
make examples                          # run every offline example
make chatbot                           # chatbot vs. the bundled mock server
bash benchmarks/run.sh                 # benchmark suite
bash benchmarks/compare.sh             # Vayu vs C vs Python
bash benchmarks/compare_ai.sh --release # AI pipeline vs C vs Python
make install PREFIX=$HOME/.local       # put vyc on your PATH
```

## Known limitations

- **Closures don't capture** their enclosing scope. A closure reading an
  outer local is rejected at compile time rather than silently miscompiled.
- **Partial unboxing**: proven `int`/`float` locals emit as raw `int64_t`/
  `double`, but values flowing through lists, maps and function parameters
  remain boxed. Tight numeric loops over locals are fast; collection
  iteration still pays the boxing cost.
- **No files, modules, or imports yet**. One file per program.
- **No async or concurrency**.

I'm actively working on removing these. Specialized array types and value
unboxing for proven int/float are implemented and working.

## License

MIT License — see `LICENSE` for details.

# Ovyth

A small compiled language for automation and AI tooling. You write `.ov` files, `ovc` compiles them to ordinary native executables — no interpreter, no VM, no runtime to install on the target machine. The binary just runs.

```ov
name = "Ovyth"
print("hello from", name)
```

## Is it fast?

Measured, not guessed: **Ovyth beats Python by 2.5x–310x on loops, recursion and list building, beats CPython on five of the seven AI-pipeline cases (up to 19x), stays within 1.5x–2.9x of Python even where it loses, and runs within 1.5x of `-O3 -march=native` C on a tight integer loop.** Where it is slow, it is slow honestly — the worst core-suite gap is list building at ~50x behind C, and even that is down from 185x once `xs.push(int)` learned to use specialized int arrays. Startup is ~1ms and a hello-world binary is ~22 KB.

| Workload | vs Python | vs C | Notes |
|---|---|---|---|
| integer loop, 20M iterations | **310x faster** | 1.5x slower | proven `int` locals emit as raw `int64_t` |
| function calls (`fib(25)`) | **13x faster** | 8x — the ~1ms process floor | the recursion itself is at parity with C (BASELINE §5) |
| list building (500k `push`) | **2.5x faster** | ~50x slower | P1 int arrays: a raw int64 store per push, not a box; resizes + GC remain |
| hash maps (100k insert + read) | 1.2x slower | 3.1x slower | within noise of CPython |
| string building (40k appends) | **1.5x faster** | 12x slower | the case Python used to win |
| JSON field extraction (50k) | **1.9x faster** | 2.9x slower | path-based, no AST parse |
| repeated JSON access (500k) | **2.0x faster** | 242x slower* | *C's row is a cached loop — different algorithms — see PERFORMANCE §7 |
| context assembly (100k joins) | 2.9x slower | 9.5x slower | O(n) string builder |
| string scan (1M scans) | **2.2x faster** | sub-ms in C | |
| multi-parse (20k tool JSONs) | **19.3x faster** | 2.1x slower | `strtoll` fast accessors |
| startup of a compiled binary | ~1 ms | — | no VM to boot |
| binary size (`print("hi")`) | 22 KB | — | dead-code-stripped runtime |

Numbers: `bash benchmarks/compare.sh` + `bash benchmarks/compare_ai.sh --release`, re-measured 15 Oct 2026 on this machine (they supersede the 8 Oct snapshot in `benchmarks/RESULTS.md`, which predates specialized int arrays). Desktop variance is ±20–40% run to run, so treat the ratios as orders of magnitude. Every row is checked — the driver refuses to print a timing unless Ovyth's answer equals C's and Python's. Full tables, the caveats, and the story of the benchmark the optimiser deleted are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

The C gap has one dominant cause left: values that flow through generic lists, maps and function parameters are still boxed 16-byte tagged structs instead of register-resident ints. Loop locals that the compiler can prove are already unboxed — which is what put `intloop` within 1.5x of clang — and a `xs.push(int)` list that sema can prove is now a raw `int64_t` array, not a box per element. Closing the rest is tracked in [docs/PERFORMANCE-SPEC.md](docs/PERFORMANCE-SPEC.md).

## Setup

Ovyth targets Linux (x86-64). The compiler resolves the runtime archive and headers through `/proc/self/exe`, so a build on macOS or Windows will not find them yet. There is no prebuilt binary to download — you build it from source, which takes about a minute.

On Debian/Ubuntu:

```bash
sudo apt install build-essential clang libcurl4-openssl-dev libssl-dev zlib1g-dev llvm-dev
```

Then build the compiler and runtime:

```bash
make            # builds build/ovc and build/libovrt.a
make test       # runs the full test suite (it should pass — if not, that's a bug worth reporting)
./build/ovc version   # -> ovc 0.1.3 (Ovyth)
```

Install to PATH:

```bash
./install.sh                 # ~/.local/bin/ovc (no sudo)
./install.sh /usr/local      # /usr/local/bin/ovc (needs sudo)
make install PREFIX=$HOME/.local
./uninstall.sh               # take it back off
```

The installer copies three things: `bin/ovc`, `lib/libovrt.a`, `include/ovrt.h`. `ovc` finds the last two relative to its own path — no environment variables, no pkg-config, relocatable.

## Your first program

```bash
cat > hello.ov << 'EOF'
name = "Ovyth"
print(name)
EOF

./build/ovc run hello.ov      # interpreter — instant, best while developing
./build/ovc hello.ov          # compile to ./hello (native executable)
./hello                       # run it
```

Ship the binary by copying it — it only needs the same libc.

## The language in 30 seconds

```ov
# Values — no type annotations needed, the compiler infers
count = 42
ratio = 0.75
name  = "Ovyth"
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
print("hi {name}, math={1 + 2 * 3}")   # hi Ovyth, math=7

# Files — built-in persistence, no imports needed
file.write("notes.txt", "hello")
print(file.read("notes.txt"))

# Errors
try { throw "boom" } catch e { print("caught:", e) }
```

The full tour — lists, comprehensions, slices, tuples, the stdlib — is in [docs/USAGE.md](docs/USAGE.md).

## The `ovc` command

| command | what it does |
|---|---|
| `ovc init <dir>` | create a new project from a template |
| `ovc run <file.ov>` | run with the tree-walking interpreter |
| `ovc <file.ov>` | compile to a native executable |
| `ovc <file.ov> --release` | optimized: -O3, LTO, stripped |
| `ovc <file.ov> --target=native` | optimize for this CPU |
| `ovc check <file.ov>` | parse + type-check only |
| `ovc ast <file.ov>` | dump the AST |
| `ovc tokens <file.ov>` | dump the token stream |
| `ovc fmt <file.ov>` | canonical formatting (stdout) |
| `ovc pkg install <name>` | install a package globally |
| `ovc pkg list` | show installed packages |
| `ovc pkg search <term>` | search the registry |
| `ovc version` | print version |

## Packages

`ovc pkg` installs a package into a global store, and `import "name"` then
works from any directory on the machine — Python's `site-packages`, for Ovyth:

```bash
ovc pkg search ai          # what is in the registry?
ovc pkg install ai         # download + install globally
ovc pkg list               # what is installed

# then, in any .ov file, anywhere:
#   import "ai"
```

Three are published. **`greet`** (the canonical first one), **`ai`** (talk to an
OpenAI-compatible endpoint, including conversation shaping) and **`rag`**
(chunk, store, retrieve, prompt). All three are modules, not applications:
nothing runs at import time, each takes and returns plain data, and the
chatbot and rag examples are built *on* them rather than duplicating them.

Packages live in `$OVYTH_HOME/packages` (`~/.ovyth/packages` by default, so
`OVYTH_HOME=/tmp/ci` isolates a build). A file next to your own `.ov` always
wins over an installed package, and each package is just a directory of `.ov`
modules — there is nothing to build per package.

The default index is this repository's `registry/registry.json`, served over
GitHub's raw CDN: adding a package to that file and merging is publishing, and
no package code lives here — entries point at the author's own repository.
Writing one has three rules: prefix every public name (`ai_`, `rag_`) because
top-level names are flattened into one program, hold no state in the module,
and take data and return data. The full guide and the index format are in
[docs/USAGE.md](docs/USAGE.md#11-packages) and [registry/](registry/README.md).

## Examples

```bash
# Chatbot with mock server
python3 examples/chatbot/mock_server.py &
AI_API_URL=http://localhost:8765/v1/chat/completions AI_API_KEY=dummy \
  ./build/ovc run examples/chatbot/chatbot.ov

# RAG chatbot with persistent memory (offline, no API key needed)
./build/ovc run examples/chatbot/chatbot_rag.ov

# RAG pipeline
./build/ovc run examples/rag/rag_pipeline.ov

# The autonomous agent (demo mode, no key needed)
./build/ovc run examples/agent/agent.ov

# Or talk to it directly: it asks for your endpoint once, then it's a chat
./build/ovc agent

# Tool agent
./build/ovc run examples/tool_agent/tool_agent.ov
```

## Documentation

| document | what's in it |
|---|---|
| [docs/USAGE.md](docs/USAGE.md) | usage guide — install, language tour, stdlib reference, errors |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | how fast Ovyth is — measured numbers, vs C and Python, and why |
| [docs/PERFORMANCE-SPEC.md](docs/PERFORMANCE-SPEC.md) | the performance engineering specification |
| [benchmarks/RESULTS.md](benchmarks/RESULTS.md) | raw benchmark output and what each optimization bought |
| [CONTRIBUTING.md](CONTRIBUTING.md) | build, test, style, and the two-backend rule |
| [AGENTS.md](AGENTS.md) | the engineering rules every contributor and AI coding assistant follows |
| [CHANGELOG.md](CHANGELOG.md) | what changed in each release |

## Run everything

```bash
make test                              # unit + both-backend regression
bash tests/interp.sh                   # language tests only
bash tests/pkg.sh                      # package manager only
bash tests/ui.sh                       # installer + output rules
bash tests/net.sh                      # TCP client/server + http.timeout
bash tests/ai.sh                       # the AI boundary against a failing provider
bash tests/system.sh                   # os.exec: deadlines, signals, env, stdin, caps
bash tests/automation.sh               # the automation example, failures included
bash tests/rag.sh                      # indexed retrieval == a full scan
make examples                          # run every offline example
make chatbot                           # chatbot vs. the bundled mock server
make chatbot-rag                       # RAG chatbot with persistent memory
make automation                        # the end-to-end automation example
make gate                              # performance regression gate (pinned machine)
bash benchmarks/run.sh                 # benchmark suite
bash benchmarks/compare.sh             # Ovyth vs C vs Python
bash benchmarks/compare_ai.sh --release # AI pipeline vs C vs Python
make install PREFIX=$HOME/.local       # put ovc on your PATH
./uninstall.sh                         # take it back off
```

## Known limitations

- **Partial unboxing**: proven `int`/`float` locals emit as raw `int64_t`/`double`, and so does the loop variable of a `for` over a specialized array (`range(...)`, all-int or all-float list literals) when the body keeps it raw — no writes, no shadowing, no closure capture. Arithmetic in such loops stays in C registers; reads box on demand at `OvValue` use sites. Values flowing through lists, maps and function parameters remain boxed, and loop variables that are written, shadowed or captured fall back to the boxed loop.
- **Top-level globals are not visible to functions on the native backend** (pre-existing, and now more visible with modules): a top-level `let` is an `ov_main` local, so a function that reads it compiles on the interpreter but fails to compile natively with `unknown name`. Pass such values as arguments until this is fixed.
- **No async or concurrency**.

Closures *do* capture their enclosing scope — by reference, shared between the closure and the enclosing function — on both the interpreter and the native backend. A closure that reads or writes an outer local works as you'd expect; see `tests/interp/017_closures.ov` for the covered cases.

I'm actively working on removing the rest. Specialized array types and value unboxing for proven int/float are implemented and working.

## License

MIT License — see `LICENSE` for details.
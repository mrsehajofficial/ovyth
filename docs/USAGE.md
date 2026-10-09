# Ovyth usage guide

Everything you need to install, write, and run Ovyth programs. Read it
front-to-back the first time; after that it works as a reference.

---

## 1. What is Ovyth?

A small compiled language for automation and AI tooling.

```ov
name = "Ovyth"
print("hello from", name)
```

You write `.ov` files. The compiler, `ovc`, turns them into **native
machine code** — an ordinary ELF executable. Nothing interprets your program
at the other end: no Python, no Node, no VM. Copy the binary to another
machine with the same libc and it runs.

There is also an interpreter (`ovc run`) for development — it starts
instantly and prints errors more readably. What you deploy is the compiled
binary.

---

## 2. Install and build

Ovyth builds from source with Clang and a C++17 compiler. On Debian/Ubuntu:

```bash
sudo apt install build-essential clang libcurl4-openssl-dev libssl-dev zlib1g-dev llvm-dev
```

Then:

```bash
cd /path/to/ovyth
make            # builds build/ovc and build/libovrt.a
make test       # runs the full test suite
```

`make` finishes silently on success. If it fails, the error names the file.

Check it works:

```bash
./build/ovc version
# ovc 0.1.3 (Ovyth)
```

### Putting `ovc` on your PATH

Rather than typing `./build/ovc` everywhere, install it into a prefix:

```bash
./install.sh                       # -> ~/.local/bin/ovc   (no sudo)
./install.sh /usr/local            # -> /usr/local/bin/ovc (needs sudo)
make install PREFIX=$HOME/.local   # the same thing via make
```

The installer copies three things:

```
<prefix>/bin/ovc            the compiler
<prefix>/lib/libovrt.a      the runtime compiled programs link against
<prefix>/include/ovrt.h     the runtime headers ovc passes to clang
```

`ovc` locates the last two relative to its own path, so there is nothing
to configure and you can move the prefix afterwards.

### Starting a project

`ovc init` creates a project that builds and is ready to commit:

```bash
ovc init myapp
cd myapp
make run          # interpreter, instant
make build        # native executable at build/app
make release      # -O3, LTO, stripped
```

It writes four files:

```
main.ov      the program
Makefile     run / build / release / clean
README.md    a note on how to build it
.gitignore   build output
```

Three templates are available, chosen with `--template` (or `-t`):

```bash
ovc init myapp                    # hello  -- print a greeting (default)
ovc init scraper -t http          # http   -- call an API, parse the JSON
ovc init chat --template=cli      # cli    -- an interactive input() loop
```

`ovc init` won't write into a directory that already has files; it reports
the conflict instead of overwriting.

---

## 3. Your first program

Create `hello.ov`:

```ov
name = "Ovyth"
print(name)
```

Run it:

```bash
./build/ovc run hello.ov      # interpreter, instant
./build/ovc hello.ov          # compiled to ./hello
./hello                       # run the native binary
```

The second command produces a real executable. Copy it to another machine
with the same libc and it still runs — nothing else is needed.

---

## 4. Language tour

### Values

```ov
count   = 42          # Int
ratio   = 0.75        # Float
enabled = true        # Bool
missing = null        # nil
name    = "Ovyth"      # String
items   = [1, 2, 3]   # List
config  = {"port": 8080, "host": "localhost"}   # Map
```

Variables need no type annotation — the compiler infers it. You can
annotate when you want to:

```ov
count: Int = 42
```

### Printing

```ov
print("a", 1, true)     # a 1 true     (space separated, newline at end)
print(42)               # 42
print([1, 2, 3])        # [1, 2, 3]
print({"a": 1})         # {"a": 1}
eprint("to stderr", 42) # same rendering, on stderr — stdout stays clean
```

### Strings

```ov
greeting = "hello " + "world"
upper    = "ovyth".upper()
shout    = "abc".repeat(3)        # abcabcabc
parts    = "a,b,c".split(",")     # ["a", "b", "c"]
joined   = join("-", ["a", "b"])  # a-b
padded   = "ab".pad(5)            # "   ab"  (left-padded, space fill)
dotted   = "ab".pad(5, 46)        # "...ab"  (46 is the fill code point)
```

### Interpolation

```ov
name = "Ovyth"
n = 42
print("hi {name}, n={n}, math={1 + 2 * 3}")   # hi Ovyth, n=42, math=7
```

Braces that don't contain a valid expression stay literal, so JSON is safe
inline: `print("{\"a\":1}")` prints `{"a":1}`.

### Control flow

```ov
if n > 100 {
    print("big")
} else if n > 10 {
    print("medium")
} else {
    print("small")
}

i = 0
while i < 3 {
    print(i)
    i = i + 1
}

for item in ["a", "b", "c"] {
    if item == "b" { continue }   # skip
    print(item)
}

for ch in "abc" {            # strings iterate by character
    print(ch)
}

for key in some_map {        # maps iterate by key
    print(key)
}
```

### Functions

```ov
function add(a, b) {
    return a + b
}
print(add(2, 3))            # 5

# named arguments, in any order
print(add(b: 20, a: 10))    # 30

# recursion works
function fact(k) {
    if k <= 1 { return 1 }
    return k * fact(k - 1)
}

# closures
double = function(x) { return x * 2 }
print(double(21))           # 42
```

### Lists and maps

```ov
xs = [1, 2, 3]
xs.push(4)                  # [1, 2, 3, 4]
print(xs[0], xs.length())   # 1 4

squares = [x * x for x in [1, 2, 3, 4]]     # [1, 4, 9, 16]
evens   = [x for x in squares if x % 2 == 0]

m = {"a": 1}
m["b"] = 2
print(m["a"], m.has("b"), m.count())
print(m.keys(), m.values())
```

**Specialized arrays.** When a list literal contains only values of one
proven type (`[1, 2, 3]`, `[1.0, 2.0]`), the compiler emits a specialized
array — `Int64Array`, `Float64Array` or `StringArray` — with a contiguous
element buffer. This is transparent: they print and iterate like ordinary
lists and every list operation works the same. The compiler picks the
type from the inferred element type.

### Multiple assignment

```ov
a, b = 1, 2
x, y, z = [10, 20, 30]      # unpacks a list
print(a + b, x + y + z)     # 3 60
```

Works with specialized arrays too.

### Errors

```ov
try {
    print(1 / 0)
} catch e {
    print("caught:", e)
}

try {
    throw "something went wrong"
} catch err {
    print(err)
}

assert(1 < 2, "must be ordered")
```

An error that no `try` catches stops the program and prints a calm
report — the value that was thrown, plus a tip — and exits with code
70. `exit(n)` stays silent and exits with `n`.

### Debugging

```ov
debug   # prints "ℹ ovyth: prog.ov:3" — where you are, nothing more
```

`debug` is a breadcrumb: drop it in to see a file and line as the
program passes it, on either backend.

---

## 5. Real example: the chatbot

`examples/chatbot/chatbot.ov` is a complete program — it reads a line,
POSTs it to an LLM chat endpoint, prints the reply, and loops until you
type `exit`.

```bash
# terminal 1 — a fake OpenAI-compatible server, no API key needed
python3 examples/chatbot/mock_server.py 8642

# terminal 2
export AI_API_KEY=whatever
export AI_API_URL=http://127.0.0.1:8642/v1/chat
export AI_MODEL=my-model
./build/ovc examples/chatbot/chatbot.ov -o chatbot
./chatbot
```

```
Ovyth chatbot -- type 'exit' to quit
> you said: 'hello'
```

Point `AI_API_URL` at any OpenAI-compatible endpoint and it works
unchanged.

---

## 6. The `ovc` command

| command | what it does |
|---|---|
| `ovc init <dir>` | create a new project from a template |
| `ovc run prog.ov` | run with the interpreter (instant, good while coding) |
| `ovc prog.ov` | compile to `./prog`, a native executable |
| `ovc prog.ov -o path` | choose the output path |
| `ovc prog.ov --release` | optimized: `-O3`, LTO, symbols stripped |
| `ovc prog.ov --release --target=native` | also tune for this CPU |
| `ovc prog.ov -O0` … `-O3` | optimization level (default 2) |
| `ovc prog.ov --keep-ir` | keep the generated C for inspection |
| `ovc check prog.ov` | parse and type-check, produce no binary |
| `ovc fmt prog.ov` | print canonically formatted source |
| `ovc ast prog.ov` | dump the syntax tree |
| `ovc tokens prog.ov` | dump the token stream |
| `ovc version` | print the version |

Use `--release` for anything you ship. For iterating, plain `-O2` compiles
faster and keeps symbols for a debugger.

---

## 7. Typical workflow

```bash
ovc check prog.ov     # syntax/type check, instant
ovc run prog.ov       # try it
ovc prog.ov --release -o prog    # build for real use
./prog
```

---

## 8. Reading errors

Errors name the file, line, and column, and carry a calm icon that
matches their severity — `✖` error (red), `▲` warning (yellow),
`ℹ` info (cyan), `•` note (dim). Colors appear on a terminal; pipes
and logs keep the icons but no escape codes:

```
prog.ov:4:9: ✖ error: unknown name 'nmae'
✖ 1 error
tip: fix the issues above, then run ovc again
```

A count summary and, for errors, a single tip line follow the
diagnostics themselves.

When a parse surprises you, `ovc ast` and `ovc tokens` answer the
question "why did it see that?".

---

## 9. Reference

### Operators

| | |
|---|---|
| arithmetic | `+` `-` `*` `/` `%` `**` |
| comparison | `==` `!=` `<` `<=` `>` `>=` |
| logic | `and` `or` `not` |
| bitwise | `&` `|` `^` `<<` `>>` |
| membership | `x in list` |
| assignment | `=` `+=` `-=` `*=` `/=` `%=` |

### Builtins

| area | functions |
|---|---|
| output | `print` `eprint` `input` |
| environment | `env` `env_or` `setenv` |
| conversion | `len` `str` `int` `float` `bool` `type` `range` |
| json | `json.parse` `json.stringify` `json.valid` `json.extract` `json.get_int` `json.get_float` `json.get_str` |
| http | `http.get` `http.post` `http.put` `http.delete` |
| strings | `upper` `lower` `trim` `split` `join` `contains` `replace` `indexof` `repeat` `startswith` `endswith` `pad` `chars` `bytes` |
| lists | `push` `pop` `insert` `remove` `sort` `reverse` `contains` `indexof` `extend` `length` `sum` |
| maps | `get` `set` `has` `delete` `keys` `values` `items` `count` `merge` |
| math | `abs` `sqrt` `floor` `ceil` `round` `min` `max` `sum` `pow` |
| misc | `time.clock` `time.now` `gc` `exit` `throw` `assert` |

A few semantics worth knowing:

```ov
api_key = env_or("AI_API_KEY", "")   # fallback when unset; env(name) throws instead
model   = env_or("AI_MODEL", "my-model")
setenv("LOG_LEVEL", "debug")         # nil
```

`time.clock()` is monotonic seconds as a Float (use it for measuring);
`time.now()` is epoch seconds as an Int. `gc()` collects, `gc("stats")`
returns a `"live=... heap=... allocs=..."` string, and `gc("reset")` clears
the counters. `pad(s, n)` / `s.pad(n)` pads on the left to width `n`; a
third argument sets the fill code point (`pad(s, n, 46)` fills with `.`)
and a fourth of `"right"` pads on the right instead.

### HTTP response shape

```ov
response = http.post(
    "https://example.com/api",
    headers = {"Authorization": "Bearer " + api_key},
    json = {"model": "m", "messages": msgs}
)

print(response.status)      # 200
print(response.ok)          # true
print(response.body)        # the response text
print(response.elapsed_ms)

data = json.parse(response.body)
```

Transport failures (DNS, TLS, timeout) are raised as catchable errors, so
`try`/`catch` handles them like any other.

---

## 9. Modules

`import "path"` pulls another `.ov` file's top-level definitions into
your program. Paths are resolved relative to the importing file; if the
path has no `.ov` extension it is appended and retried, so both
`import "util.ov"` and `import "util"` work.

```ov
import "mods/mathx.ov"

print(scale(4))
```

Imports are flattened at load time: the imported file is parsed before
the importer's own statements, so its functions and globals are in
scope wherever the import appears. The rules:

- **Definitions only.** An imported file contributes its top-level
  `function` and `let` declarations; it is not executed as a separate
  program and runs no top-level side effects of its own beyond those
  initializations.
- **Unique names.** Top-level names must be unique across every module
  and the entry file. A duplicate is an error that names both files.
- **No cycles.** `import` cycles are an error.
- **Deduplicated.** A file imported more than once — directly or through
  a diamond — is merged once.

A module function that reads a top-level global currently fails to
compile on the native backend (see Known limitations); pass such values
as arguments until top-level globals become visible to functions there.

---

## 10. Known limitations

Worth knowing before you rely on something:

- **No async or concurrency.**
- **Top-level globals are not visible to functions on the native
  backend.** A top-level `let` is an `ov_main` local, so a function
  reading it compiles on the interpreter but fails natively with
  `unknown name`. Pass such values as arguments.
- **Partial unboxing.** Variables proven as int/float emit as raw
  `int64_t`/`double` — but values flowing through lists, maps and function
  parameters remain boxed. Tight numeric loops over locals are fast; loops
  that touch collections still pay the boxing cost. Measured numbers and
  what will close the gap: [PERFORMANCE.md](PERFORMANCE.md). Raw output:
  `benchmarks/RESULTS.md`.

Closures are **not** a limitation: they capture their enclosing scope by
reference and work on both backends. A closure may read or write an outer
local, several closures may share one captured variable, and a closure can be
returned from a function or stored in a list and called later
(`pair[0](...)`). The regression test `tests/interp/017_closures.ov` pins the
semantics down.


---

## 11. Project layout

```
compiler/          the ovc compiler (C++17)
runtime/           the C runtime that compiled programs link against
  include/ovrt.h     the C API: values, strings, lists, maps, GC
  src/*.c            GC, strings, containers, JSON, HTTP, console
tests/interp/      language tests, run through BOTH backends
benchmarks/        benchmark suite and comparison driver
examples/          runnable examples, including chatbot/
docs/USAGE.md           this guide
docs/PERFORMANCE.md     how fast Ovyth is, with measured numbers
docs/PERFORMANCE-SPEC.md  the performance engineering specification
benchmarks/RESULTS.md    measured performance
```

Run everything:

```bash
make test                              # unit + both-backend regression
bash tests/interp.sh                   # language tests only
make examples                          # run every offline example
make chatbot                           # chatbot vs. the bundled mock server
bash benchmarks/run.sh                 # benchmark suite
bash benchmarks/compare.sh             # Ovyth vs C vs Python
bash benchmarks/compare_ai.sh --release # AI pipeline vs C vs Python
```

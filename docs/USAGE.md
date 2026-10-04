# Vayu usage guide

Everything a beginner needs to install, write, and run their first Vayu
program.

---

## 1. What Vayu is

A small compiled language for automation and AI tooling.

```vayu
name = "Vayu"
print("hello from", name)
```

You write `.vy` files. The compiler `vyc` turns them into **native machine
code** — an ordinary ELF executable. No interpreter ships with your program, and
it doesn't need Python, Node, or a virtual machine at runtime.

There is also an interpreter (`vyc run`) that you can use while developing,
because it starts instantly and prints errors more readably. The compiled
binary is what you deploy.

---

## 2. Install and build

Vayu builds from source with Clang and a C++17 compiler. On Debian/Ubuntu:

```bash
sudo apt install build-essential clang libcurl4-openssl-dev libssl-dev zlib1g-dev llvm-dev
```

Then:

```bash
cd /path/to/vayu
make            # builds build/vyc and build/libvyrt.a
make test       # runs the full test suite
```

`make` finishes with no output on success. If it does, the error names the file.

Check it works:

```bash
./build/vyc version
```

### Putting `vyc` on your PATH

Rather than typing `./build/vyc`, install it into a prefix:

```bash
./install.sh                       # -> ~/.local/bin/vyc   (no sudo)
./install.sh /usr/local            # -> /usr/local/bin/vyc (needs sudo)
make install PREFIX=$HOME/.local   # the same thing via make
```

The installer copies three things into the prefix:

```
<prefix>/bin/vyc            the compiler
<prefix>/lib/libvyrt.a      the runtime compiled programs link against
<prefix>/include/vyrt.h     the runtime headers vyc passes to clang
```

`vyc` locates the last two relative to its own path, so there is nothing to
configure and the prefix can be moved afterwards.

### Starting a project

`vyc init` creates a project that builds and is ready to commit:

```bash
vyc init myapp
cd myapp
make run          # interpreter, instant
make build        # native executable at build/app
make release      # -O3, LTO, stripped
```

It writes four files:

```
main.vy      the program
Makefile     run / build / release / clean
README.md    a note on how to build it
.gitignore   build output
```

Three templates are available, chosen with `--template` (or `-t`):

```bash
vyc init myapp                    # hello  -- print a greeting (default)
vyc init scraper -t http          # http   -- call an API, parse the JSON
vyc init chat --template=cli      # cli    -- an interactive input() loop
```

`vyc init` will not write into a directory that already has files in it; it
reports the conflict instead of overwriting.

---

## 3. Your first program

Create `hello.vy`:

```vayu
name = "Sehaj"
age = 18
print(name, age)
```

Run it:

```bash
./build/vyc run hello.vy      # interpreter, instant
./build/vyc hello.vy          # compiled to ./hello
./hello                       # run the native binary
```

The second command produces a real executable. Copy it to another machine with
the same libc and it still runs — nothing else is needed.

---

## 4. Language tour

### Values

```vayu
count   = 42          # Int
ratio   = 0.75        # Float
enabled = true        # Bool
missing = null        # nil
name    = "Vayu"      # String
items   = [1, 2, 3]   # List
config  = {"port": 8080, "host": "localhost"}   # Map
```

Variables need no type annotation — the compiler infers it. You can annotate
when you want to:

```vayu
count: Int = 42
```

### Printing

```vayu
print("a", 1, true)     # a 1 true     (space separated, newline at end)
print(42)               # 42
print([1, 2, 3])        # [1, 2, 3]
print({"a": 1})         # {"a": 1}
eprint("to stderr", 42) # same rendering, on stderr -- stdout stays clean
```

### Strings

```vayu
greeting = "hello " + "world"
upper    = "vayu".upper()
shout    = "abc".repeat(3)        # abcabcabc
parts    = "a,b,c".split(",")     # ["a", "b", "c"]
joined   = join("-", ["a", "b"])  # a-b
padded   = "ab".pad(5)            # "   ab"  (left-padded, space fill)
dotted   = "ab".pad(5, 46)        # "...ab"  (46 is the fill code point)
```

### Interpolation

```vayu
name = "Vayu"
n = 42
print("hi {name}, n={n}, math={1 + 2 * 3}")   # hi Vayu, n=42, math=7
```

Braces that don't contain a valid expression stay literal, so JSON is safe
inline: `print("{\"a\":1}")` prints `{"a":1}`.

### Control flow

```vayu
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

```vayu
function add(a, b) {
    return a + b
}
print(add(2, 3))            # 5

// named arguments, in any order
print(add(b: 20, a: 10))    # 30

// recursion works
function fact(k) {
    if k <= 1 { return 1 }
    return k * fact(k - 1)
}

// closures
double = function(x) { return x * 2 }
print(double(21))           # 42
```

### Lists and maps

```vayu
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

### Multiple assignment

```vayu
a, b = 1, 2
x, y, z = [10, 20, 30]      # unpacks a list
print(a + b, x + y + z)     # 3 60
```

### Errors

```vayu
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

---

## 5. Real example: the chatbot

`examples/chatbot/chatbot.vy` is a complete program — it reads a line, POSTs it
to an LLM chat endpoint, prints the reply, and loops until you type `exit`.

```bash
# terminal 1 — a fake OpenAI-compatible server, no API key needed
python3 examples/chatbot/mock_server.py 8642

# terminal 2
export AI_API_KEY=whatever
export AI_API_URL=http://127.0.0.1:8642/v1/chat
export AI_MODEL=my-model
./build/vyc examples/chatbot/chatbot.vy -o chatbot
./chatbot
```

```
Vayu chatbot -- type 'exit' to quit
> you said: 'hello'
```

Point `AI_API_URL` at any OpenAI-compatible endpoint and it works unchanged.

---

## 6. The `vyc` command

| command | what it does |
|---|---|
| `vyc init <dir>` | create a new project from a template |
| `vyc run prog.vy` | run with the interpreter (instant, good while coding) |
| `vyc prog.vy` | compile to `./prog`, a native executable |
| `vyc prog.vy -o path` | choose the output path |
| `vyc prog.vy --release` | optimised: `-O3`, LTO, symbols stripped |
| `vyc prog.vy --release --target=native` | also tune for this CPU |
| `vyc prog.vy -O0` … `-O3` | optimisation level (default 2) |
| `vyc prog.vy --keep-ir` | keep the generated C for inspection |
| `vyc check prog.vy` | parse and type-check, produce no binary |
| `vyc fmt prog.vy` | print canonically formatted source |
| `vyc ast prog.vy` | dump the syntax tree |
| `vyc tokens prog.vy` | dump the token stream |
| `vyc version` | print the version |

Use `--release` for anything you ship. For iterating, plain `-O2` compiles
faster and keeps symbols for a debugger.

---

## 7. Typical workflow

```bash
vyc check prog.vy     # syntax/type check, instant
vyc run prog.vy       # try it
vyc prog.vy --release -o prog    # build for real use
./prog
```

---

## 8. Reading errors

Errors name the file, line, and column:

```
prog.vy:4:9: error: unknown name 'nmae'
```

`vyc ast` and `vyc tokens` are the tools for "why did it parse that way".

---

## 9. Reference

### Operators

| | |
|---|---|
| arithmetic | `+` `-` `*` `/` `%` `**` |
| comparison | `==` `!=` `<` `<=` `>` `>=` |
| logic | `and` `or` `not` |
| bitwise | `&` `\|` `^` `<<` `>>` |
| membership | `x in list` |
| assignment | `=` `+=` `-=` `*=` `/=` `%=` |

### Builtins

| area | functions |
|---|---|
| output | `print` `eprint` `input` |
| environment | `env` `env_or` `setenv` |
| conversion | `len` `str` `int` `float` `bool` `type` `range` |
| json | `json.parse` `json.stringify` `json.valid` |
| http | `http.get` `http.post` `http.put` `http.delete` |
| strings | `upper` `lower` `trim` `split` `join` `contains` `replace` `indexof` `repeat` `startswith` `endswith` `pad` `chars` `bytes` |
| lists | `push` `pop` `insert` `remove` `sort` `reverse` `contains` `indexof` `extend` `length` |
| maps | `get` `set` `has` `delete` `keys` `values` `items` `count` `merge` |
| math | `abs` `sqrt` `floor` `ceil` `round` `min` `max` `sum` `pow` |
| misc | `time.clock` `time.now` `gc` `exit` `throw` `assert` |

A few semantics worth knowing:

```vayu
api_key = env_or("AI_API_KEY", "")   # fallback when unset; env(name) throws instead
model   = env_or("AI_MODEL", "my-model")
setenv("LOG_LEVEL", "debug")         # nil
```

`time.clock()` is monotonic seconds as a Float (for measuring), `time.now()`
is epoch seconds as an Int. `gc()` collects; `gc("stats")` returns a
`"live=... heap=... allocs=..."` string and `gc("reset")` clears the counters.
`pad(s, n)` / `s.pad(n)` pads on the left to width `n`; an optional third
argument sets the fill character (`pad(s, n, 46)` fills with `.`) and a fourth
of `"right"` pads on the right instead.

### HTTP response shape

```vayu
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

## 10. Known limitations

Worth knowing before you rely on something:

- **Closures don't capture their enclosing scope.** A closure that reads a
  variable from the function around it is rejected with an error rather than
  silently doing the wrong thing.
- **No files, modules, or imports yet.** Everything is in one file per program.
- **No async or concurrency.**
- **Values are boxed.** Arithmetic goes through a runtime call, so tight
  numeric loops are far slower than C. Measured numbers and what will close
  the gap: [PERFORMANCE.md](PERFORMANCE.md). Raw output:
  `benchmarks/RESULTS.md`.

---

## 11. Project layout

```
compiler/          the vyc compiler (C++17)
runtime/           the C runtime that compiled programs link against
  include/vyrt.h     the C API: values, strings, lists, maps, GC
  src/*.c            GC, strings, containers, JSON, HTTP, console
tests/interp/      language tests, run through BOTH backends
benchmarks/        benchmark suite and comparison driver
examples/          runnable examples, including chatbot/
docs/USAGE.md           this guide
docs/PERFORMANCE.md     how fast Vayu is, with measured numbers
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
bash benchmarks/compare.sh             # Vayu vs C vs Python
```
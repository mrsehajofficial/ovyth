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
<prefix>/include/ovrt_helpers.h
```

`ovc` locates the last two relative to its own path, so there is nothing
to configure and you can move the prefix afterwards.

On a terminal the installer is greeted by Ovyth's sprite and shows its
progress as steps; piped, it prints the plain list of paths, escape-code
free, which is what a packaging script wants. `NO_COLOR=1` keeps the
structure and drops the colour.

### Uninstalling

```bash
./uninstall.sh                       # remove the install in ~/.local
./uninstall.sh /usr/local            # remove it from a prefix
./uninstall.sh --purge               # also remove ~/.ovyth and your packages
```

The uninstaller removes exactly the files the installer wrote — never a
directory it did not create, and a directory only if it is empty
afterwards, so a `bin/` you keep something else in survives. It reports
each file's size and the total freed, then signs off.

Your installed packages are yours: by default they are left alone and you
are told where they are. `--purge` removes the package store too, and
refuses if `OVYTH_HOME` points at something that does not look like a
store (`$HOME`, `/`).

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

### Files

Persistent file I/O is built in — no imports, no external dependencies.
Works identically on both the interpreter and the compiled backend.

```ov
# Read entire file (empty string on error)
content = file.read("notes.txt")

# Write (overwrites); returns Bool
ok = file.write("notes.txt", "hello")

# Append; returns Bool
ok = file.append("log.txt", "entry\n")

# Existence check
if file.exists("config.json") { ... }

# Size in bytes (Int, -1 on error)
sz = file.size("data.bin")

# Delete; returns Bool
file.delete("tmp.txt")
```

An installed package's modules import the same way, from any directory on
the machine — see [Packages](#11-packages).

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

### The autonomous agent

`examples/agent/agent.ov` is the language doing what it exists for: a loop that
decides, acts on the machine, observes, and repeats. One file, one binary.

```bash
ovc run examples/agent/agent.ov          # demo mode: scripted model, real commands
make agent                               # the same, from the Makefile
```

The loop is deliberately small. The model is given the goal and a log, replies
with **one** JSON object — `{"cmd": "..."}` or `{"done": "..."}` — the command
runs through `os.run()`, and its exit code, stdout and stderr go back as the
next turn. Tools are shell commands, because on Linux that *is* the tool
interface, so the agent is not limited to a list somebody wrote down.

`os.run(cmd)` returns `{cmd, code, ok, stdout, stderr}`: the exit status, and
both streams kept apart so an agent can tell a command's output from its
complaint. It costs about **2ms per call** (spawn, drain, wait) and pulls in no
new dependencies — an arithmetic binary's `ldd` is still libc and libm — so an
agent's step budget goes to the model call and the command's own work, not the
runtime.

#### The guard

A guard, not a sandbox. Writing, installing, cloning and building are the point
of an agent and they are all writes, so the question is not "does this change
anything" but "does this destroy something that cannot be built again" — the
system roots, the block devices, and the machine's power state. Everything else
is allowed: `rm -rf build`, `rm -rf /tmp/x`, `rm -rf ~/.cache`, `sudo apt
install` all run.

A refusal is fed back to the model as an observation, with the reason, so it
adapts rather than retrying. Two settings:

- `AGENT_PROTECT=path:path` — extra paths to protect, for the things a program
  cannot guess. Subdirectories of `$HOME` are deliberately **not** protected by
  default: which ones matter is the user's call.
- `AGENT_UNSAFE=1` — disable the guard entirely.

Deletion is decided by *parsing* the target, not substring-matching the command.
Matching `"rm -rf /usr"` looks sufficient until it is tested: `rm -rf /`
contains no such substring and sailed straight through, as did `rm -rf $HOME`.

With `AI_API_KEY` / `AI_API_URL` / `AI_MODEL` set it runs against a live
OpenAI-compatible endpoint. Without them it uses a scripted stand-in, so the
whole loop — parsing, executing, logging, the step budget — is readable and
testable offline.

### Talking to it: `ovc agent`

`ovc agent` is the same agent as a chat. No goal to phrase in advance and no
environment variables: on the first run it asks for the endpoint once, stores it
in `~/.ovyth/agent.json`, and from then on you just type.

```
$ ovc agent
   ✦
       /\      /\          O v y t h ✦  agent
      /  \____/  \         a tiny compiled language
     |  *      *  |
     |      w      |
     |   '----'   |
      \          /
       '.______.'
         |  |
        (_)(_)

model:  gemini-3.5-flash  at  http://localhost:8081/v1/chat/completions
guard:  on  (refuses the unrebuildable)
type :help for commands, :quit to leave

you> how many .ov files are in tests?
  ⬡ find tests -name '*.ov' | wc -l
  ⬡ exit 0 in 12ms
Ovyth: There are 29 .ov files in tests.
```

The model has four tools — a shell command, reading a file, writing a file, and
saying something back — and takes as many as it needs before answering, so one
line from you can be three commands on the machine. `:help` lists the slash
commands; `:readonly on|off` toggles the guard, `:config model <name>` switches
model without editing a file. The TUI ships as source next to the binary and
runs through the interpreter, so there is exactly one implementation of the
agent. Decoration is emitted only when `os.tty()`, so piping it into a file stays
clean.

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
| `ovc pkg install <name>` | install a package globally |
| `ovc pkg list` | show the installed packages |
| `ovc pkg remove <name>` | uninstall a package |
| `ovc pkg search <term>` | search the registry |
| `ovc pkg info <name>` | registry and installed detail for one package |
| `ovc version` | print the version |

Use `--release` for anything you ship. For iterating, plain `-O2` compiles
faster and keeps symbols for a debugger. `ovc pkg --help` lists the package
manager's subcommands and flags.

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
| files | `file.read` `file.write` `file.append` `file.exists` `file.size` `file.delete` |
| system | `os.run` |
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

## 10. Modules

`import "path"` pulls another `.ov` file's top-level definitions into
your program. Paths are resolved relative to the importing file; if the
path has no `.ov` extension it is appended and retried, so both
`import "util.ov"` and `import "util"` work.

When nothing is found next to the importing file, the global package store
is consulted — an installed package imports the same way, from any
directory. See [Packages](#11-packages) below.

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

## 11. Packages

`ovc pkg` installs packages into a global store so that `import "name"`
works from any directory on the machine — not just the project that happens
to hold the files. Same idea as Python's `site-packages`.

```bash
ovc pkg search ai                # what is in the registry?
ovc pkg info rag                 # versions, source, license
ovc pkg install ai rag greet     # several at once
import "ai"                      # in any .ov file, anywhere

ovc pkg list                     # what is installed
ovc pkg update rag               # upgrade to the newest version
ovc pkg remove greet             # uninstall
```

Several packages go in one command: the index is fetched once for the whole
run, every name is validated before anything is installed (so a bad name in
the middle cannot leave half a list applied), and the run ends with one
summary and one `try it` panel covering everything. A local directory and a
registry package can be mixed in the same command.

Three packages are published: **`greet`** (greeting helpers, the canonical
first one), **`ai`** (talk to an OpenAI-compatible endpoint, including
conversation shaping) and **`rag`** (chunk, store, retrieve, prompt). They
are modules, not applications — nothing runs at import time and each takes
and returns plain data, so they compose into an agent or a five-line script.

On a terminal, `install` shows each step as it happens and ends with a
`try it` panel; `list` and `search` have a header rule; `info` comes in a
box. All of it is decoration for humans only: **piped output is byte-for-byte
what it was**, so scripts, CI and golden-file tests are unaffected, and
`NO_COLOR=1` keeps the structure while dropping the colour.

### Writing a package

A package is a directory of `.ov` files with one entry module named after it
(or `main.ov`); submodules import under the package name. Three rules matter
more than anything else, because of how Ovyth resolves names:

1. **Prefix every public name** (`ai_`, `rag_`). Ovyth flattens all top-level
   names from every module and the entry file into one program, so the prefix
   *is* the namespace. A package exporting `search()` collides with the next
   package that does.
2. **Hold no state in the module.** A top-level `let` is invisible to
   functions on the native backend, and two packages sharing a global name is
   a collision. The caller owns the data and passes it in.
3. **Take data, return data** — strings, ints, floats, lists and maps — so
   anything the module hands back can be printed or `json.stringify`'d.

Sharp edges, learned the hard way: comments are `//`, not `#`; a helper must
be defined before the function that calls it; `^` is XOR rather than power
(use `sqrt`/`pow`); and a function value works as a *literal* argument but a
named function reference does not. `registry/README.md` has the full guide.

### The store

Packages live in `$OVYTH_HOME/packages/<name>` — `~/.ovyth/packages` by
default. `OVYTH_HOME` relocates the whole store, so CI or a test run can
keep its packages to itself:

```bash
OVYTH_HOME=/tmp/ci-store ovc pkg install greet
```

Resolution order is **local first, package second**: a file next to the
importing `.ov` always wins over an installed package, so a project can
shadow a global package (or vendor a patched copy) without uninstalling
anything.

### What a package looks like

A package is a plain directory of `.ov` files with one entry module named
after the package (or `main.ov`). Submodules import under the package name:

```
mymath/
  mymath.ov        <- import "mymath"
  stats.ov         <- import "mymath/stats"
```

Top-level names must be unique across the whole program — installed
packages included. The module loader already reports a collision and names
both files, exactly as it does for a local `import`.

### Publishing

The default index is this repository's `registry/registry.json`, served over
GitHub's raw CDN. There is nothing to deploy: add a package to that file,
merge to `main`, and it is published — users see it within a few minutes.

```json
{
  "packages": {
    "greet": {
      "description": "a friendly greeting",
      "license": "MIT",
      "homepage": "https://github.com/you/ovyth-greet",
      "versions": {
        "1.0.0": {
          "repo": "https://github.com/you/ovyth-greet.git"
        }
      }
    }
  }
}
```

A version's source is a `repo` (git — the version, or its `v`-prefixed tag,
is cloned), a `url` (tarball) or a `path` (local directory). Prefer `repo`:
the download traffic then lands on the author's git host instead of on this
project, and versions are tags rather than file names.
`ovc pkg install greet@1.0.0` pins a version; `ovc pkg install greet` takes
the newest.

The index is fetched over HTTP and cached in the store for an hour
(`--refresh` re-fetches; a failed fetch falls back to the cache, so an
offline machine keeps working). `--registry=<url>` or `OVYTH_REGISTRY` point
at any other index, and a `file://` URL reads one off disk.
`registry/README.md` has the full format and the publishing flow.

Installing is the whole story — there is no per-package build step. The
source is copied into the store, `.ovpkg.json` records where it came from,
and your program compiles the package's source in exactly like a local
`import`.

---

## 12. Driving the agent from another device

The agent can be driven from anywhere: a phone, a laptop, another terminal.
It listens, and every line it receives goes through exactly the same loop a
keyboard goes through — the tool dispatch, the guard and the model call do
not know the difference.

```bash
# On the PC — the port is chosen by the kernel, and announced:
OVYTH_AGENT_SERVE=0 \
OVYTH_AGENT_TOKEN=$(openssl rand -hex 16) \
OVYTH_AGENT_HOST=0.0.0.0 \
ovc agent
```

```
listening on 0.0.0.0:41732  (token required)
waiting for a connection...  (ctrl-c to stop)
```

Then, from the phone (Termux, or any machine that can reach the PC):

```bash
python3 examples/agent/phone.py <pc-ip> 41732 <token>
```

```
token: ovyth agent -- model ... :help for commands, :quit to disconnect
you> what is the biggest directory under /etc?
  ⬡ du -sh /etc/*/ | sort -rh | head -5
  ⬡ exit 0 in 104ms
  │ 85M	/etc/skel/
Ovyth: /etc/skel, at 85M.
```

The phone is I/O and nothing else — the model, the tools and the files all
stay on the PC. The protocol is a line: whatever you type is sent, whatever
the agent prints arrives.

### Finding the agent without typing the port

`OVYTH_AGENT_PORT_FILE=<path>` makes the agent write the port it bound to a
file when it starts, so the client can discover it:

```bash
OVYTH_AGENT_PORT_FILE=~/.ovyth/agent.port ovc agent ...
python3 examples/agent/phone.py <pc-ip>        # reads the port from that file
```

Port `0` means "any free port", which is why this works at all: a hard-coded
port collides with whatever else on the machine is listening, and the failure
looks like a broken agent rather than a busy port.

### Security: this is a remote shell

Read this before binding anything other than `127.0.0.1`. An agent that runs
commands on your PC, reachable from the network, *is* a remote shell, and the
model chooses the commands. Three things follow:

- **Always set a token.** Without `OVYTH_AGENT_TOKEN` the first line sent
  does not have to match anything — any port scan that finds the agent can
  drive it. With one, the socket is closed before a single turn is served.
  The comparison is constant-time, so the token cannot be recovered a
  character at a time by timing the responses.
- **Prefer `OVYTH_AGENT_HOST=127.0.0.1`** unless the phone is really on
  another machine. Loopback is unreachable from the network; `0.0.0.0` is not.
- **Guard on, not `AGENT_UNSAFE=1`.** The guard already refuses the
  unrebuildable (system roots, disk writes, power state) whether the request
  came from a keyboard or a socket. It is the same code path for both, which
  is the point of routing the transport through `emit()` rather than around
  it.

---

## 13. Known limitations

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

## 14. Project layout

```
compiler/          the ovc compiler (C++17)
runtime/           the C runtime that compiled programs link against
  include/ovrt.h     the C API: values, strings, lists, maps, GC
  src/*.c            GC, strings, containers, JSON, HTTP, console, file I/O
tests/interp/      language tests, run through BOTH backends
tests/pkg.sh       package manager regression (install/import/remove)
tests/net.sh       net.* primitives and the agent's remote mode
registry/          the live package index served to `ovc pkg`
benchmarks/        benchmark suite and comparison driver
examples/          runnable examples, including chatbot/ and agent/
docs/USAGE.md           this guide
docs/PERFORMANCE.md     how fast Ovyth is, with measured numbers
docs/PERFORMANCE-SPEC.md  the performance engineering specification
benchmarks/RESULTS.md    measured performance
```

Run everything:

```bash
make test                              # unit + both-backend regression
bash tests/interp.sh                   # language tests only
bash tests/pkg.sh                      # package manager only
bash tests/net.sh                      # net primitives + agent remote mode
bash tests/ui.sh                       # pipes stay plain (NO_COLOR, no escapes)
make examples                          # run every offline example
make chatbot                           # chatbot vs. the bundled mock server
make chatbot-rag                       # RAG chatbot with persistent memory
bash benchmarks/run.sh                 # benchmark suite
bash benchmarks/compare.sh             # Ovyth vs C vs Python
bash benchmarks/compare_ai.sh --release # AI pipeline vs C vs Python
```

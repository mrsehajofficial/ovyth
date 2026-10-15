# Changelog

Everything that has happened to Ovyth, newest first. Versions follow
[semantic versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## 0.1.4 (in progress)

### `os.exec` and `sleep` — the system layer, no shell and no busy-wait — 15 Oct 2026

The automation audit (LOW-LEVEL-AUDIT.md) found the system layer was a
shell-out: `os.run` hands a string to `/bin/sh`, so every external step pays
`fork+exec+shell`, arguments get re-parsed by another language (a value from
a file or a URL is a command), a hung child hangs the program forever, and
the child inherits everything the parent has open. There was also no way to
wait at all.

`os.exec(argv, timeout=, cwd=, env=, stdin=, stdout=, stderr=, max_output=)`
is the primitive underneath, and `os.run` is untouched:

```ovyth
r = os.exec(["grep", "-r", needle, "/srv/logs"], timeout=5000, cwd="/srv",
            env={"PATH": "/usr/bin"}, stdin="", max_output=1048576)
# -> {ok, status, code, pid, stdout, stderr, elapsed_ms}
#    status: "ok" | "failed" | "signal" | "timeout" | "truncated"
```

* **argv is bytes.** `os.exec(["echo", "a b;c", "d|e"])` prints `a b;c d|e` —
  a semicolon is an argument, not syntax. There is nothing to inject into.
* **`timeout=` bounds the run.** At the deadline the child's process group is
  killed and reaped, and the reason is reported: `status="timeout"` (the
  system suite asserts a 250ms deadline fires on a 10-second child).
* **`env=` isolates the child.** Given a map it sees exactly that; absent, it
  inherits as before. Credentials stop reaching programs never meant to see
  them.
* **`stdin=` is fed, nulled or inherited — `/dev/null` by default**, never the
  script's own input. `stdout=`/`stderr=` capture (default), inherit, or an fd.
* **`max_output=` (64MB default)** stops the buffer growing without limit,
  keeps draining so the child never blocks, and reports `"truncated"`.
* **A signal death reports `128+signo`**, the way a shell does.

Implementation: `posix_spawn`, not fork+exec. Nothing of ours runs between
the fork and the exec (so the GC's locks and the heap are never observed
mid-transition), it is faster than the current fork+execl path (vfork
semantics, no page-table copy), and `POSIX_SPAWN_SETPGROUP` puts the child
in a group of its own *at spawn time* — no window in which it is still in
the script's group, so the group kill cannot reach the script itself. The
deadline is `poll()`'s own timeout and SIGCHLD arrives through a signalfd,
so waiting costs no CPU; every pipe is `O_CLOEXEC`. `posix_spawnp` for a bare
name (PATH search, like every shell), `posix_spawn` for one with a slash.
Three bugs were found by the first differential run and fixed on the spot:
a missing PATH search, a failure path that answered nil instead of throwing,
and an argv check that rejected all-string lists (which lower to string
arrays natively).

`sleep(ms)` waits without burning a core — 20 x 50ms costs ~1s of wall and
0ms of user CPU, measured in the suite. Monotonic, so a wall-clock change
cannot shorten it; EINTR resumes the remainder.

New: `tests/system.sh` (48 assertions on both backends, byte-for-byte), the
`procs` benchmark case (200 spawns with capture, in `benchmarks/profile.sh`
and the gate). All suites green: 60 + 43 + 25 + 11 + 48 + 24 + 5 + 17.

One gate fix along the way: `gate.sh --update` could not re-record while the
current numbers were outside tolerance, which made it impossible to accept a
deliberate change (or a throttled machine — the CPU at 900MHz runs the whole
suite, *including the C reference*, ~4x slower). The wall-clock tier now
re-records; the deterministic allocation counters were unaffected.

### The AI boundary is tested, and three real bugs fell out of it — 15 Oct 2026

`tests/ai.sh` puts a deliberately misbehaving provider in front of the
published `ovyth-ai` package and asserts what a caller actually gets: a
500 that keeps the server's own message, a 200 that is not JSON, a 200 with
an HTML login page, a 200 with zero choices, a 200 with null content, a
refused connection, and a provider that never answers. Every case runs on
both backends, and the suite is wired into `make test` and CI. Writing it
found three bugs the hand-written unit tests never touched:

* **`"...".endswith()` was wrong whenever the needle appeared earlier.**
  It compared the needle's *first* occurrence against the tail, so
  `"http://127.0.0.1:11434/".endswith("/")` was false — the "/" in "://"
  answered the question. Both the runtime and the interpreter had the same
  wrong answer, so nothing disagreed; this is exactly the bug a
  single-backend suite cannot see. It is what made the package's
  `ai_normalize_url` drop the endpoint path for a host written with a
  trailing slash. Regression in `tests/interp/003_strings.ov`.
* **`json.parse` of invalid input returned the error string as a value on
  the native backend** while the interpreter threw — a two-backend
  divergence, and the AI boundary then reported "no choices" instead of
  "invalid JSON" because it indexed into the error text. The helper's
  invalid-JSON branch never set its `threw` flag; the non-string branch
  above it did. Both backends now throw.
* **`ai_request` indexed `choices[0]` before checking the shape**, so a
  gateway answering 200 with `{"choices": []}` produced "list index 0 out
  of range" (reads like the caller's own list is wrong) and a 200 with a
  JSON error body produced the same. The shape is now validated where the
  response is still in hand, the status is named alongside a parse
  failure, and `chatbot_rag.ov`'s `call_llm` — the one call site that
  indexed with no guard at all — validates the same way.

Two more fixes the work exposed: the raw-boundary planner now refuses a
parameter the body uses as a container (`len(a)`, `a[i]`, `for x in a`),
which `rag_cosine` in the RAG suite does — the claim produced a clang type
error, not a slow program; and the boxed `for` loop's element variable is
initialised, which removes a `-Wsometimes-uninitialized` warning from
generated code. `tests/rag.sh` was failing on native for exactly that
warning and was not part of `make test`; both it and the AI suite are now.

### `fib` at parity with C — the per-call root bookkeeping was the cost — 15 Oct 2026

The plan said `fib`'s gap to C was boxing on the function boundary, so
raw `int64_t` parameters and returns landed for functions the analysis
can prove. Measured, they changed nothing (fib(32): 27ms -> 25-28ms,
inside noise): `ov_int`/`ov_add` inline to a tag test at -O3, so a boxed
boundary is nearly free. Attributing the gap by experiment instead found
the real cause — hand-written C for fib(35) is 31-33ms, the same C with
`ov_gc_roots_mark`/`ov_gc_roots_restore` per call is 107ms, and Ovyth was
103-113ms. The pair exists to pop a frame's root registrations on return,
so a function that registers none needs neither: raw parameters are not
heap references and a body with no rooted local has nothing to pop. Those
functions now emit neither call. **fib(35): 103-113ms -> 31ms, at parity
with the same function in C**, every other case unchanged, every
allocation count reproducing exactly.

Two changes were measured and rejected on the way: boundary boxing alone
(no effect, kept only as the enabling half of this), and `-flto` on the
runtime archive (3-6ms, and it breaks non-release links with "plugin
needed to handle lto object").

The work also fixed a latent codegen defect it exposed: `for n in [1,2]`
inside a function with a parameter `n` assigned the raw loop element into
the parameter's `OvValue` slot, a clang type error — the program would
not compile. Loop variables now always get their own C local. New
regression: `tests/interp/028_fn_boundary.ov` (24 cases: raw and boxed
boundaries, self-recursion, param assignment, shadowing, closures, mutual
calls, mixed raw/boxed signatures, named arguments, overflow wrap,
floats).

A performance regression gate joins the suite: `make gate` /
`benchmarks/gate.sh` compares profile.sh's four dimensions against
`benchmarks/EXPECTED.tsv` (wall clock and RSS at 1.5x on the pinned
machine, allocations and resizes exactly, since those are deterministic),
and CI runs the deterministic tier on every push. The options and the
record protocol are in the script header.

### Benchmark re-measurement, and the docs that needed fixing — 15 Oct 2026

The published numbers were re-measured from the documented commands
(`benchmarks/compare.sh`, `compare_ai.sh --release`, `profile.sh`) and two
claims turned out to be stale in the *favourable* direction for the
scoreboard: `listappend` had been reported at 173x-185x of C, but that
figure predates P1 int collections. Today a provable `xs.push(int)` is a
raw `int64_t` array: **3ms whole-process against C's 0.06ms — ~50x, not
185x** — with 3 allocations and 17 resizes, and `strconcat` now beats
CPython (2ms vs 3ms) instead of losing to it. `intloop` stays within
1.5-1.8x of `-O3 -march=native`, `mapops` 3.1x, the AI suite still ahead of
Python on 5 of 7 cases (`multi_parse` now 19.3x). README.md and
docs/PERFORMANCE.md §1 carry the 15 Oct table; RESULTS.md keeps the 8 Oct
snapshot with a supersession note, and BASELINE.md needed no changes —
its table reproduces within noise on this machine.

`AGENTS.md` is new: the Ovyth PRD's section 14 rules (workflow, the
optimization protocol, the forbidden practices) now live in the
repository contributor guidance, with the commands above verified rather
than copied.

### Google leads the search chain — 14 Oct 2026

`GOOGLE_API_KEY` and `GOOGLE_CSE_ID` put Google's own search at the head of
the agent's search chain: the Custom Search JSON API, which is the same index,
ranking and snippets the search page shows. Without them the chain runs
keyless, exactly as before, and a key that is wrong answers 400/403 and is
treated as no results — the question falls through to DuckDuckGo and the topic
indexes rather than failing.

**What there is no keyless Google here.** The chain was built without keys
because "a search tool that needs a key is a tool the agent may not have", and
the cost of that choice was a federation of topic indexes — Wikipedia, Hacker
News, StackExchange, GitHub — answering for the whole web. It answers *"what
is Zig"* well and *"who is Sehaj Varma"* badly: one Wikipedia blast fills the
result quota, and the model tells the user nothing was found.

Google cannot fill that gap without a key, and this was checked rather than
assumed. `www.google.com/search` on the machine this was written on answers
200 with 92,933 bytes that hold no results: not one result link, not one result
heading, no captcha, no embedded result JSON — and its own redirect target is
`/httpservice/retry/enablejs`, the page that says *enable JavaScript to
search*. The variants that used to work are equally empty: `?gbv=1`,
`?client=chrome`, `/m?q=`, `?udm=14`, the `ie`/`oe` pair, a `co.uk` mirror,
and following the shell's own meta refresh all return the same empty 93KB.
The conclusion is in the source beside the provider, so nobody spends another
afternoon on the parser.

`tests/interp/025_websearch.ov` pins the API's hit shape, and asserts the
chain order in three states: keyless (no Google — a provider that answers with
nothing is not a provider, and it is tried first), Google plus Brave, and
Google alone.

### The agent stops segfaulting in the collector — 14 Oct 2026

`ovc agent` died with SIGSEGV inside the GC's mark phase, sometimes on the
first web search and sometimes on the one after it. Reported twice in the same
session: a search for a name, then a search for a portfolio site, both ending
in a core dump and no explanation on screen.

The crash was a use-after-free, and the object was a **response map**. Both
backends built the `http.*` response map in a C local:

```c
OvMap* m = ov_map_new();
ov_map_set(m, ov_str_new("status", 6), ...);   // allocates
```

Each of those five key allocations can trigger a collection, and at that
instant the half-built map is reachable from nothing — the collector was
entitled to free it, and did. The caller was then handed a map built on freed
memory; reading it two collections later walked a header that had been
recycled into some other object and died in `mark_loop`.

What made it an agent bug rather than a runtime curiosity is the shape of the
session: a search response, a model call, and a third search share a heap with
a memory store and a disk cache, so collections fire often enough that one of
them lands inside the five allocations. The fix brackets the build with
`ov_gc_begin_mutation()` — the guard `ov_json_parse` has always used for the
same reason — so no collection runs while the map is reachable from nothing.

`tests/net.sh` now has "every http response map survives the collector": 30
requests of a 900KB body each, against a live set large enough that the
threshold sits inside the window. Verified to dump core on both backends with
the fix reverted, and to pass with it in place — the bug was that easy to
reintroduce.

### The agent can search the web — 14 Oct 2026

The agent's tools were a shell, a file read, a file write and its own memory:
everything it knew about the world came from its training, so its answers
about anything that changes — today's news, a version, a price — were as stale
as its checkpoint. Two new tools fix that, both from one new module that ships
beside the agent: `{"search": "..."}` and `{"fetch": "https://..."}`.

```
you> what language is bun written in
  ◆ search: bun javascript runtime language
  ◆ wikipedia in 553ms
  1. Bun (software)
     https://en.wikipedia.org/wiki/Bun
     began rewriting Bun with heavy use of AI tools, from the programming language Zig to Rust
Ovyth: Bun is written in Zig.
```

**No API key, by design.** A search tool that needs a key is a tool the agent
may not have. What that choice costs is no single backend: from a datacenter,
DuckDuckGo's html endpoint answers HTTP 202 with a challenge page, Bing
answers a search for *anything* with a page of Copilot links, and the public
SearXNG instances sit behind an anti-bot page — all measured on the machine
this was written on. So there is a chain, in order: DuckDuckGo, then the
keyless JSON APIs that always answer (Wikipedia, Hacker News, StackExchange,
GitHub), each allowed to return nothing. Google's own search now leads it when
keyed (see above), with Brave just behind; both are real engines, and
everything below them is a federation of topic indexes.

**Where the speed went.** The hot path is a parse, not a request: pages are
scanned with `indexof`/`slice` and never split into lines, every parse stops
at `limit`, every string is capped at parse time so the cache holds short
entries, and `ws_fetch` reads a page against a character budget — a 5MB page
costs about what a 5KB one costs. Results are cached on disk for fifteen
minutes, so an agent that asks the same question three times pays the network
once and the last two answers are a scan. The chain also has a wall-clock
budget, because six providers times eight seconds of timeout is a thirty-second
stall and one question is not worth that.

`tests/interp/025_websearch.ov` (30 golden lines, both backends) pins the
parsers against canned bodies — no network in the language regression. The
agent's own path is covered in `tests/memory.sh` by a third fixture gateway,
`searchgateway.py`, which sends a `{"search": ...}` tool call and then an
answer: the assertions are that the tool fires, that the keyless chain answers,
that the result reaches the model as its own observation, that it lands in the
disk cache, and that `:search <query>` answers the repeat from the cache.

### The agent is drivable from another device — 13 Oct 2026

`OVYTH_AGENT_SERVE=<port> ovc agent` makes the agent wait for connections
instead of reading a keyboard. Each line a client sends goes through the same
`handle_turn` the terminal runs — tool dispatch, guard, history and the sweep
for a missing binary's spellings all shared, none duplicated. The phone is I/O
only: the model, the tools and the files stay on the PC.

```
you> what is the biggest directory under /etc?
  ⬡ du -sh /etc/*/ | sort -rh | head -5
  ⬡ exit 0 in 104ms
  │ 85M	/etc/skel/
Ovyth: /etc/skel, at 85M.
```

`examples/agent/phone.py` is a client that fits in a terminal on a phone —
one socket, one line at a time, no protocol to get out of sync.

Two runtime additions make it work: `net.listen/accept/read/write/close` for
TCP, and `net.port(fd)` to read back the port the kernel chose (binding port
0). Blocking throughout, because a server that must do something else
meanwhile is a thread, and a thread in this runtime is a much bigger
decision than the feature warrants.

**Security, since this is a remote shell.** The first line the client sends
must match `OVYTH_AGENT_TOKEN`, compared in constant time; a wrong or missing
token closes the socket before a single turn is served. `OVYTH_AGENT_HOST`
defaults to `0.0.0.0` — set `127.0.0.1` to keep the port private. The guard
runs unchanged on the remote path, which is the point of routing the
transport through `emit()` rather than around it.

`OVYTH_AGENT_PORT_FILE` makes the agent publish the port it bound, so a
client can discover it without being told.

`tests/net.sh` (9 checks, run from `make test`) covers the primitives, line
assembly across a split read, token rejection, a clean disconnect, and that
the accept loop re-arms for a second connection. It learned two things the
hard way and now documents them: ports come from the kernel rather than a
hard-coded number (a collision presents as a broken server, not a busy port),
and the port is reported through a *file* rather than stdout — stdout is
block-buffered whenever it is a pipe, so a server blocked in `accept()` holds
its port line in the buffer forever and a test polling the log waits on a
line that has already been written.

### `os.run`, and the autonomous agent — 12 Oct 2026

The language could read and write files and call an API, but it could not do
anything to the machine it runs on. `os.run(cmd)` closes that: it runs a command
through `/bin/sh` and returns `{cmd, code, ok, stdout, stderr}` — exit status,
and both streams kept apart so a caller can tell a command's output from its
complaint. fork/exec rather than popen, because popen gives one stream and no
exit status; both pipes are drained with poll() so neither can deadlock the
child while the other is read.

`examples/agent/agent.ov` is what that makes possible: an autonomous agent in
one file, compiling to one binary. It is a loop that thinks, acts, observes and
stops — the model replies with one JSON object (`{"cmd": ...}` or
`{"done": ...}`), the command runs, and its result goes back as the next turn.
Tools are shell commands, because on Linux that *is* the tool interface, so the
agent is not limited to a list somebody wrote down. `make agent` runs it in demo
mode against a scripted model, so the loop is readable and testable with no key.

Measured cost per agent step: **2ms** for `os.run` (spawn, drain, wait) and
**3us** for `json.parse`. A `du -sh /etc` step took 96ms in the interpreter and
23ms compiled — the rest is the model call. The runtime is not the bottleneck
for an agent.

`os` adds no dependencies: an arithmetic binary's `ldd` is still libc and libm.

### The biggest win measured was not in the runtime — 12 Oct 2026

Every Ovyth program paid ~12ms of TLS initialisation it never used. An
arithmetic program loaded libcurl, libssl, libcrypto and libnghttp3, because
`ovrt_helpers.o` — linked into every program for `len()`, `sum()` and
`join()` — contained the `http` caller, and the linker pulled the whole
crypto stack from there. An empty C program linked with those libraries takes
15-18ms to start against 5ms without.

`ov_h_http` now lives in `runtime/src/http_builtin.c`, its own translation
unit, so the linker only pulls it when the generated C actually calls it;
`--as-needed` — which the driver already passed — then drops the libraries for
everything else. An arithmetic binary's `ldd` is libc and libm.

```
case        before     after      ratio vs C, before -> after
-------------------------------------------------------------
intloop       25ms      5ms        1.6x -> 1.4x
fib            9ms      1ms        16x  -> 9x
strconcat     20ms      2ms        25x  -> 10x
listappend    23ms      2ms        82x  -> 40x
mapops       239ms     65ms       2.5x  -> 3.0x
startup        6ms      1ms
```

The path to that finding is worth recording, because the obvious suspects were
all wrong. `benchmarks/micro_append.c` times the runtime's own push in a plain
C loop, with the codegen out of the picture:

```
push, growing      17.8 ns/elem   runtime, exactly what codegen emits
push, pre-sized    11.6 ns/elem   the same store, growth removed
raw C               6.0 ns/elem   the floor
raw C, growing     12.3 ns/elem   growth itself, in raw C
codegen shape      14.7 ns/elem   the emitted loop, hand-written in C
```

The runtime's append is 1.45x an equivalent C loop — so the runtime was never
the problem, and neither was the loop condition, the root registration, or the
zeroing of a buffer about to be filled. All measured, all ruled out, all in the
commit that follows this entry.

### P1: int collections, half the distance to C — 12 Oct 2026

`xs = []` followed only by `xs.push(<int>)` now becomes an `Int64Array`
instead of a boxed list: a raw int64 store per push, 8 bytes per element
instead of 16, and nothing for the collector to mark.

```
case        before                    after
--------------------------------------------------------------------
listappend  41ms / 15076 KB RSS      24ms /  9316 KB RSS
vs C        184x                      82x
```

`sema` marks a name as an int collection when it starts from an empty list
literal and retires it on the first push whose argument is not provably an
int, so the value's type is never lost. Retirement is flow-insensitive by
design: a float assignment *after* the push in source but *before* it on a
loop's back edge still retires it, which is what a pre-pass over the whole
body provides.

Specialized arrays were documented as "NOT GC-tracked; must be freed
explicitly", which meant `return xs` handed back a freed array. They now
carry an `OvHeader` like every other object and are collected, so an array
is reachable exactly as long as something points at it.

Leaks the change would have exposed, all fixed: `type()` printed `?` for a
specialized array; `json.stringify()` fell through its switch; `sum()`
returned 0 because it only handled `OV_LIST`; `pop()` on an empty array
returned `0` where the interpreter returns `nil`.

Also fixed along the way, and not specific to this change: **a variable
assigned an int and then a float was emitted as a raw `int64_t`**, so
`i = 0` … `i = 2.5` printed `2` on the native backend where the interpreter
printed `2.5`. The unboxed claim is now retired when a name is given a
non-int value — the same flow-insensitivity, applied to locals.
`tests/interp/024_array_boundaries.ov` and `025_int_collections.ov` cover
both, on both backends (50 language tests now).

### Performance milestone: measurement, and two correctness fixes — 12 Oct 2026

The P0 checklist item "measure, do not guess" is done, and it changed what
the next optimization should be.

`benchmarks/profile.sh` measures four dimensions per case instead of one:
wall clock (median of 5 reps, whole process, startup included), peak RSS
(measured by a `fork`+`getrusage` wrapper *outside* the process, so the
measurement cannot be fooled), and from new `--stats` telemetry the
allocations, GC cycles, collection growths and peak heap. It refuses to
report a case whose output is not stable across three runs.

`--stats` is now implemented (it was documented in `usage()` but did
nothing) and works on both backends: `ovc run --stats x.ov` and
`ovc x.ov --release --stats` print the same counters, because they come from
the runtime either way. Telemetry is emitted only when asked for, so a
benchmark never instruments its own timing.

Two correctness bugs were found by testing the specialized-array paths the
audit had flagged, both of which made the two backends disagree:

- **an out-of-range read on a specialized array returned 0** where a plain
  list raised an index error. A silent, plausible, wrong answer — the worst
  kind. `ov_i64a_get_slow`, `ov_f64a_get_slow` and `ov_stra_get_slow` now
  raise, matching `ov_list_get_slow`.
- **an index-assign into a specialized array always threw
  "cannot index-assign"**, because the codegen handled only `OV_LIST` and
  `OV_MAP`. `d[99] = 7` on `[1, 2, 3]` now stores (in range) or raises an
  index error (out of range), and writes through range-checked setters.

`tests/interp/024_array_boundaries.ov` pins all of it on both backends:
in-range writes, negative indices, out-of-range reads for int/float/string
arrays, out-of-range writes, growth past the initial capacity, and an empty
collection.

Findings, in `benchmarks/BASELINE.md`: arithmetic is already 1.6x of C and
maps 2.5x, while `mapops`' 400k allocations turned out to be the benchmark's
own string building rather than the map. The largest measured gap is the
list-append path at 184x of C, and the evidence for the cause is that
`for x in range(500000)` — specialised into a native C loop, no collection
built — does the same work in 21ms with 2 allocations where `push` takes 45ms
with 3 GC cycles. The recommendation is type propagation into collections
(P1), not a new GC, JIT or LLVM backend.

### Install several packages in one command — 11 Oct 2026

`ovc pkg install ai rag greet` now works. The index is fetched once for the
whole run rather than once per package, every name is validated before
anything is installed (so a bad name in the middle of a list cannot leave
half of it applied), and the run ends with one summary and one `try it`
panel covering everything installed. A local directory and a registry
package can be mixed in the same command, and a repeated name is installed
once rather than failing on its second copy.

Piped output is unchanged in shape — one `installed <name> <version>` line
per package, then the `tip:` line for each — so a single-package install in
a script behaves exactly as it did before.

`ovc pkg install --help` also describes the install command rather than the
whole manager: `--help` after a subcommand belongs to that subcommand.

### Three packages, and the examples become applications — 11 Oct 2026

`greet` (the canonical first package), `ai` (OpenAI-compatible chat
transport plus conversation shaping) and `rag` (chunk, store, retrieve,
prompt) are published to the registry and install with
`ovc pkg install <name>`.

They are modules, not applications: nothing runs at import time, each one
takes and returns plain data, and the caller owns the state. The chatbot
and rag-pipeline examples are now built *on* them rather than
reimplementing them — `examples/chatbot/chatbot.ov` is the loop, the prompt
and the demo mode, and everything that talks to a model is `ai_*`;
`examples/rag/rag_pipeline.ov` is a corpus, three queries and a
persistence round trip, and everything else is `rag_*`.

Writing a package has three rules, all forced by how Ovyth resolves names:

- **prefix every public name** (`ai_`, `rag_`). Top-level names from every
  module and the entry file are flattened into one program, so the prefix
  *is* the namespace and an unprefixed export collides with the next
  package's.
- **hold no state in the module.** A top-level `let` is invisible to
  functions on the native backend, and two packages sharing a global is a
  collision — the caller owns the data and passes it in.
- **take data, return data**, so anything a module hands back can be
  printed or serialized.

`registry/README.md` documents all of it, plus the sharp edges that cost
time to find: comments are `//` (not `#`), helpers must be defined before
their callers, `^` is XOR rather than power (so `sqrt`/`pow`, never
`x ^ 0.5`), and a function value works as a literal argument but a named
function reference does not.

### Three backend bugs, found by writing the packages — 11 Oct 2026

Building the modules surfaced three real divergences between the
interpreter and the native backend. All three are fixed, each with a test
in `tests/interp/` that fails on at least one backend if it regresses.

**`time.now()` reported the monotonic clock.** Two dispatch sites in the
codegen mapped it to the monotonic helper, so the native backend returned
time-since-boot where the interpreter returned unix seconds. Added
`ov_h_now_realtime()` to the runtime and pointed both sites at it.
(`021_time_now.ov`)

**`type()` printed `?` for any specialized array.** The native backend
emits an `Int64Array` for `[1, 2, 3]` where the interpreter builds a plain
`List`, and `ov_tag_name` had no case for the array tags. Both backends now
call them `List`. (`022_type_of_literals.ov`)

**A local's type leaked between functions.** sema's proven-type map was
keyed by name alone, so one function's int counter made another
function's float of the same name an `int64_t` — and the generated code
then read `.i` off a float-holding union, producing a raw bit pattern
(`0x3FF0000000000000`) where a number was expected. The same leak turned a
one-character string into an int. The map is now keyed by the enclosing
function, and codegen's seeding ignores locals so the leak cannot come
back. (`023_locals_same_name.ov`)

Two more fixes the packages needed: a directory next to the importing file
no longer shadows an installed package (it used to be accepted as a
resolved import, and the program then compiled with none of the module's
definitions and no error to explain why), and `install.sh`/`make examples`
install the packages they need.

### `uninstall.sh`, and a sprite that says goodnight — 11 Oct 2026

`./uninstall.sh` removes what `install.sh` put there and nothing else —
by design: the four installed files are removed individually, and the
directories only if they are empty afterwards, so a `bin/` the user keeps
something else in survives. It reports each file with its size, and ends
with the total freed.

Ovyth wakes to say hello and sleeps to say goodbye: the installer greets
with the sprite awake, the uninstaller says hello with her awake and then
signs off with her asleep — eyes closed, a purr, the heart moved to her
feet.

```
$ ./uninstall.sh
   ✦
       /\      /\          ✦ O v y t h ✦   goodbye
      /  \____/  \         it was nice having you here
     |  *      *  |         let me tidy up
     |      w      |
     |   '----'   |
      \          /
       '.______.'
         |  |        ♥
        (_)(_)

   ⬡ found 4 files in ~/.local ............ done
       • bin/ovc  (15.4 MB)
       • lib/libovrt.a  (658.1 KB)
       • include/ovrt.h  (27.5 KB)
       • include/ovrt_helpers.h  (3.7 KB)
   ⬡ removing bin/ovc ...................... done
   ...
   ⬡ removing empty directory bin .......... done

   ✓ Ovyth is uninstalled — 16.1 MB freed

   ♥
       /\      /\          ✦ O v y t h ✦   goodnight
      /  \____/  \         thank you for having me
     |  -      -  |         z z z
     |      w      |
     |   '----'   |
      \          /
       '.______.'
         |  |
        (_)(_)        ♥

   ♥ come back soon
```

Your installed packages are never touched without `--purge`: by default
the uninstaller removes the compiler and runtime, tells you your packages
are still in `~/.ovyth`, and leaves them. `--purge` removes the store
too, and refuses outright if `OVYTH_HOME` is set to something that is not
a store (`$HOME`, `/`) — a destructive path needs an explicit, plausible
destination.

Same output rules as everything else: decoration on a terminal only,
nothing but plain lines when piped, `NO_COLOR` respected.

`tests/ui.sh` grew to 24 checks, including that the uninstaller removes
exactly the four installed files, leaves a file it did not install, keeps
the directory that file is in, removes directories it emptied, and refuses
`--purge` on a non-store path.

### Ovyth has a face — 11 Oct 2026

The installer, `ovc version`, `ovc init` and every `ovc pkg` command now
greet a human instead of printing a build log. A small sprite — sparkle
eyes, a heart, `✦` as her mark — introduces the installer and `ovc
version`; `ovc pkg install` shows a step-by-step progress list ending in
a `try it` panel; `pkg list` and `pkg search` gained a header rule;
`pkg info` gained a box.

```
$ ./install.sh
   ✦
       /\      /\          ✦ O v y t h ✦   0.1.4
      /  \____/  \         a tiny compiled
     |  *      *  |         language
     |      w      |
     |   '----'   |
      \          /
       '.______.'
         |  |        ♥
        (_)(_)

   ⬡ building ovc and libovrt.a ......... done (4.2s)
   ⬡ installing into ~/.local
       • bin/ovc            the compiler
       • lib/libovrt.a      linked into every compiled program
       • include/ovrt.h     the runtime API, handed to clang
   ♥ try it:  ovc init myapp && cd myapp && make run
```

**The rule that makes it safe:** decoration is TTY-only. When stdout is
piped — packaging, CI, a golden-file test — every mark, colour and box
is gone and the output is byte-for-byte what it was before, which is why
`tests/pkg.sh` and `tests/interp.sh` are unaffected. `NO_COLOR` is
honoured too: set it and the structure survives while the colour does
not. All of it lives in one new header, `compiler/tools/ui.h`, with a
UTF-8-aware width helper so box borders line up in every terminal.

`tests/ui.sh` (13 checks, run from `make test`) guards the rule: it
asserts that no command emits an escape byte or a box-drawing character
when piped, that `NO_COLOR` suppresses colour, that `ovc version` is
still one parseable line, and that the strings the other suites grep for
are still printed.

### Package manager — 11 Oct 2026

`ovc pkg` installs a package into a global store
(`$OVYTH_HOME/packages/<name>`, default `~/.ovyth/packages`) and
`import "name"` then resolves from any directory on the machine —
site-packages for Ovyth. Resolution stays **local first, package
second**, so a project can shadow or vendor over a global package
without uninstalling it.

```
ovc pkg search greeting     # what is in the registry?
ovc pkg install greet       # download + install globally
ovc pkg list                # what is installed
ovc pkg update greet        # upgrade
ovc pkg remove greet        # uninstall
```

A package is a plain directory of `.ov` files with an entry module named
after it (or `main.ov`); submodules import as `import "<pkg>/<mod>"`.
There is nothing to build per package — modules are flattened into the
program at compile time, exactly like a local `import`.

Sources, in order of preference: a `path` (local directory), a `repo`
(git; the version, or its `v`-prefixed tag, is cloned shallowly — `.git`
is dropped on install) or a `url` (tarball). The registry is a JSON
index fetched over HTTP, cached for an hour and keyed by URL, and
`--registry=` / `OVYTH_REGISTRY` point at any index (a `file://` URL
reads one off disk). A failed fetch falls back to the cached index, so
an offline machine keeps working; `--refresh` forces a fetch. Each
installed package records its origin in `.ovpkg.json`.

The default index is this repository's `registry/registry.json`, served
over GitHub's raw CDN — there is nothing to deploy and no separate
registry service: adding a package to that file and merging is
publishing. Package *code* stays out of this repository; entries point
at the author's own repository (`repo` preferred, so download traffic
lands on the author's host), and the index file itself is a few KB.

`tests/pkg.sh` drives install → import (both backends) → reinstall
policy → search/info/update → remove against a throwaway
`OVYTH_HOME`, and runs from `make test`.

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

### Built-in file I/O module — 10 Oct 2026

A new `file` namespace provides persistent file operations directly
in the language — no imports, no external dependencies. Works on
both the interpreter and the native backend.

```ov
# Read entire file as string (empty string on error)
content = file.read("notes.txt")

# Write string to file (overwrites); returns Bool
ok = file.write("notes.txt", "hello")

# Append string to file; returns Bool
ok = file.append("log.txt", "entry\n")

# Check if file exists
if file.exists("config.json") { ... }

# Get file size in bytes (Int, -1 on error)
sz = file.size("data.bin")

# Delete file; returns Bool
file.delete("tmp.txt")
```

The implementation lives in `runtime/src/io.c` and is exposed
through `ovrt.h` (`ov_file_read/write/append/exists/size/delete`).
The interpreter routes through the same C helpers via
`ovrt_helpers.h`, so both backends produce identical results.

### Interpreter scoping fix — 10 Oct 2026

The tree-walking interpreter now distinguishes three scope kinds:
`ROOT` (globals), `BLOCK` (transparent, e.g. `while`/`if` bodies),
and `FUNCTION` (a new variable-binding boundary). Assignment
resolution walks outward and stops at the first `FUNCTION` scope
— so a helper's `i = 0` creates a genuine local instead of
clobbering a same-named global. `while` loops use a single
persistent `BLOCK` scope, so variables assigned in the body are
visible to the condition on the next iteration. Both backends now
agree; `tests/interp/019_gc_roots.ov` exercises the combined
behaviour.

### String slice / find / join fixes — 10 Oct 2026

- `s.slice(lo)` now defaults the upper bound to the string length
  (previously it read as 0, so `s.slice(3)` returned "").
- `s.find(sub, from)` and `s.indexof(sub, from)` honour the
  optional `from` offset on both backends; without it they start
  at 0.
- `join(sep, list)` is now a single implementation
  (`ov_h_join` in `ovrt_helpers.c`) that handles plain `OvList`,
  `StringArray`, `Int64Array` and `Float64Array` identically.
  The interpreter delegates to it, eliminating a divergence where
  native codegen emitted specialized arrays and the interpreter
  built plain lists.

### HTTP builtin now uses the connection pool — 10 Oct 2026

`ov_http_request()` is a thin adapter over the pooled easy handle
in `http_pool.c`. Every AI API call in an agent loop targets the
same host, so the pooled connection keeps the TCP+TLS session
warm instead of paying a 50–200 ms handshake on every turn.
`atexit(ov_http_pool_cleanup)` is registered at first pool
creation (not in `console.c`), so programs that never make an
HTTP call don't pull in libcurl at all. The interpreter delegates
to the same `ov_http_pool_request` path, keeping the two backends
identical.

### New RAG chatbot example — 10 Oct 2026

`examples/chatbot/chatbot_rag.ov` demonstrates retrieval-augmented
generation with persistent file-backed memory (using the new
`file.*` builtins). It runs fully offline with a deterministic
local responder; point `AI_API_URL` / `AI_API_KEY` / `AI_MODEL` at
a real endpoint for live inference. `make chatbot-rag` runs it.

The mock server now accepts a port argument (`python3 mock_server.py
[PORT]`) and the OpenAI-compatible path was corrected to
`/v1/chat/completions` (was `/v1/chat`).

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


### `os.pipe` and the fd layer — pipelines without a shell — 15 Oct 2026

W3 and W4 from the automation audit, landing together because they share one
surface: an fd is an fd.

* **`os.pipe()`** → `{"read", "write"}`, and **`os.read` / `os.write` /
  `os.close`** work on any fd — a pipe, a socket or a file. `os.exec`'s
  `stdin=` now accepts an fd, which is what lets children be wired to each
  other without a shell anywhere in the picture.
* **`file.open(path, mode)`** (r/w/a/rw) → fd, **`file.seek(fd, off, whence)`**
  → position, both sharing the same `os.read`/`os.write`/`os.close`.
* **`os.stat(path)`** → `{size, mode, mtime, is_dir, is_file, is_link}`.

The two rules the language cannot enforce, documented because they are the
plumbing: close the caller's copy of a write end (that is the EOF), and fill
and close a pipe *before* the consumer starts — feeding a child while blocked
in its exec would wait forever, so give the exec a `timeout=` and the misuse
is bounded and reported as `status="timeout"`.

Streaming also removes a silent failure the whole-file read always had:
procfs and `/sys` report no size, so `file.read("/proc/self/status")`
answered `""`. Through an fd it reads fine; `os.stat` answers without
opening anything.

Bugs the first differential run caught, all fixed on the spot: a `os.pipe`
key allocated one byte too long (so lookups for `"write"` missed and pipes
hung at EOF forever), SIGPIPE killing the process before `os.write` could
report EPIPE, a native `file.open` that only accepted `mode=` as a *named*
argument (so a positional `"w"` silently opened read-only), a native
`os.stat` deciding success from an uninitialised value, and a C string
escaping bug that broke the same call's emitted C. `sema` now validates
`os.exec`'s and `file.open`'s named arguments the way it already did for
`http` — a typo'd `timeoutt=` was a silent nil, which for `os.exec` means
"no deadline".

`tests/system.sh` is now 88 assertions (from 48), both backends byte for
byte: two- and three-stage pipelines, the caller-owned tail, feeding, EOF-only
pipes, the bounded-misuse case, EPIPE, slice-by-slice streaming with a
slice count of 13 for a 50KB file, seek to offset and to end, write/append
round-trips, procfs, stat on files and directories, and both open-failure
modes.

### The collector was running a few times per push — a 27x-170x slowdown, found and fixed — 15 Oct 2026

Auditing the blockers on the rag suite's native run (253s interpreted 20s)
turned up a real bug, not a slow algorithm. `gc("stats")` now reports
cycles, and the native path was running **40,699 collections for a 200-chunk
retrieval** where the interpreter ran 17.

Cause: the specialized arrays (`i64a`, `f64a`, `stra`) each realloc their
backing block as they grow, and `sweep()` subtracts the array's *current*
size when it frees one — but the growth was never recorded with
`ov_heap_note_realloc`, so every growth drifted `heap_bytes` down by the
delta. Enough growth wraps it negative, and then **every allocation trips the
collection threshold**. Measured after the fix, at 800MHz:

| store size | before | after | |
|---|---|---|---|
| 100 | 1671ms | 61ms | 27x |
| 200 | 10876ms | 128ms | 85x |
| 400 | 41075ms | 242ms | 170x |
| 1000 (the rag run) | 253239ms | 629ms | 402x |

`heap_bytes` is no longer underflowing, the cycle count is 7 instead of
40,699, and the native path now beats the interpreter on this workload
instead of losing to it by 12.7x. `note_realloc` is signed now too, so a
shrink subtracts instead of wrapping to a huge add.

Two more bugs the same audit found and fixed:

* **`len()` of a specialized array emitted `->len` on an `OvValue`**, and of
  a proven int local emitted `->len` on an `int64_t` — the fast path's
  condition matched `proven_ints_`, which holds raw *scalars*, and it has
  been dead-wrong-but-unreachable since it was written. `len(n)` for a proven
  int never compiled natively; it now boxes correctly and, on an int, raises
  the same defined error the interpreter raises. Regression:
  `tests/interp/029_gc_array_growth.ov`.
* `gc("stats")` printed cycles on the interpreter and not natively, and the
  native `heap=` value was a wrapped negative. Both backends now share one
  implementation, and the cycle count that found the bug is observable.

### The automation example, and the panic-path bug building it found — 15 Oct 2026

`examples/automation/` is the PRD's flow end to end, offline and
deterministic, on a bundled records API (`records_api.py`):

```
fetch JSON from an API -> validate it -> filter -> transform -> write
-> verify with a subprocess (os.exec(["wc", "-l", ...]))
-> report structured errors, retry the transient failures
```

`make automation` runs it; `tests/automation.sh` (17 assertions, in
`make test` and CI) drives the same pipeline four ways -- a happy path with
two malformed records rejected *with reasons*, a flaky endpoint the retry
survives and recovers from identically, an endpoint that is down (exit 70
and no output file left behind), a refused connection, a 200 that is not
JSON, and a 404 -- plus a native-vs-interpreter byte-for-byte comparison.

Building it found a real bug, and it was not in the new code:

**The panic path was a single global `jmp_buf`, not a stack.** A `try`
inside a function overwrote the caller's saved unwind context, so when that
function returned, any later panic or `exit()` unwound into a frame that no
longer existed. The shape that exposed it: a `try` with a `return` inside a
loop, followed by `exit()` -- which produced `*** stack smashing detected ***`
(the canary of an unrelated function), or an `exit(0)` reporting
`ovyth: null` and exiting 70. Diagnosing it also showed the codegen let
`return`/`break`/`continue` jump straight out of a `try` body, skipping its
`ov_try_pop()` and roots restore.

Both fixed: `ov_try_push`/`ov_try_pop` now keep a stack of frames (128 deep,
a defined error beyond), `raise()` unwinds to the innermost open frame, and
the three exit statements emit their pending cleanup innermost-first. A
`try` nested inside a `try` now works rather than corrupting the outer
frame, which was a documented limitation until today. Regression:
`tests/interp/030_try_frames.ov` (return-out-of-try-then-exit, nested
rethrowing, a callee's try that must not outlive its frame, and a panic that
escapes and reports once).

Everything green: 64 language tests (both backends) + 43 pkg + 25 ui + 11
net + 24 ai + 88 system + **17 automation** + 5 rag + 17 memory.

### A segfault at exit, on one compiler and not another — 15 Oct 2026

CI's automation suite failed with `rc=139` (SIGSEGV) in the *native binary*,
after it had printed its entire correct output — and the same binary passed
locally under clang 22, ASan and UBSan. Auditing the exit path found the
cause, and it was not in the new code.

`exit()` (and any uncaught panic) unwinds with `longjmp` out of `ov_main`,
which skips `ov_main`'s epilogue. Every root `ov_main` registered therefore
stayed in the collector's root list, pointing into a frame that was already
dead — and `ov_runtime_shutdown()` runs a collection, which walks that list
and dereferences what it finds. Whether the dead frame still held plausible
values is a stack-layout question, so it crashed on CI's clang and not here:
a segfault whose location has nothing to do with its cause.

The top-level frame now captures the root mark at push and restores it before
anything walks the root list on the unwind path — the same idiom an in-language
`try` has always used on its catch path. Regression:
`tests/interp/031_exit_after_alloc.ov` (allocate 2000 rooted values, then
exit; the harness asserts rc=0, so a crash at exit fails loudly on any
compiler).

Two fixes in the example's own harness along the way: its records API was a
single-threaded `HTTPServer` (a client that held a connection stalled the
next one), now `ThreadingHTTPServer` like the suite's other providers; and
the native-vs-interpreter comparison now prints both runs' tails on a
mismatch instead of a diff summary, because "rc 139 vs 70" is not a
diagnosis.

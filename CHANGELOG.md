# Changelog

Notable changes to Vayu. Versions follow [semantic
versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

## 0.1.0

First release.

### Language

- Dynamic values with inference: `Int`, `Float`, `Bool`, `null`, `String`,
  `List`, `Map`, tuples.
- `if` / `else if` / `else`, `while`, `for ... in`, `break`, `continue`.
- `function` declarations with named arguments, and `function(x) { ... }`
  literals.
- String interpolation (`"hi {name}, n={1 + 2}"`); invalid braces stay literal,
  so inline JSON is safe.
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
- **HTTP** — `http.get` `http.post` `http.put` `http.delete`, with `json=` and
  `headers=` named arguments.
- **Environment** — `env` (throws when unset), `env_or` (falls back).
- **Misc** — `type` `str` `int` `float` `bool` `range` `len` `time.clock`
  `time.now` `gc` `input` `exit`.

### Toolchain

- `vyc prog.vy` — compile to a native executable; the runtime is linked in, so
  the result needs nothing but libc.
- `vyc run prog.vy` — tree-walking interpreter for a fast edit/run loop.
- `vyc init <dir>` — **new**: scaffold a project (`hello`, `http`, `cli`
  templates) with a `main.vy`, `Makefile`, `README.md` and `.gitignore`.
- `vyc check` / `ast` / `tokens` / `fmt` — diagnostics and canonical
  formatting.
- `--release` (`-O3`, LTO, strip) and `--target=native` (`-march=native`).
- `make install` and `./install.sh` — install `vyc`, `libvyrt.a` and the
  headers into a prefix so `vyc` works from anywhere.

### Runtime

- Tracing garbage collector with `gc()` / `gc("stats")` / `gc("reset")`.
- `http.*` is linked lazily: a program that never makes a request does not
  carry libcurl.
- A compiled `print("hi")` binary is about 18 KB and starts in about 5 ms.

### Known limitations

- Closures do not capture their enclosing scope (rejected at compile time
  rather than miscompiled).
- Values are boxed, so tight numeric loops are much slower than C.
- One file per program — no modules, imports, or file I/O yet.
- No async or concurrency.

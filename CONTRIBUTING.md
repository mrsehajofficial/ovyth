# Contributing to Ovyth

Thanks for wanting to help make Ovyth better. It's a small language on purpose — the whole compiler is a few thousand lines of C++17, it builds in seconds, and the test suite finishes while you're still drinking your coffee. The edit → build → test loop is short, so please use it.

## Getting set up

On Debian/Ubuntu:

```bash
sudo apt install build-essential clang libcurl4-openssl-dev libssl-dev zlib1g-dev llvm-dev
```

Then:

```bash
make            # build ovc and libovrt.a
make test       # runtime self-test + both-backend language regression
make examples   # run every example through the interpreter
```

`make test` should be green before you start. If it isn't, that's a bug worth reporting on its own.

## Layout

| path | what lives there |
|---|---|
| `compiler/lexer/` | source text → tokens |
| `compiler/parser/` | tokens → AST |
| `compiler/ast/` | the AST types and the dumper |
| `compiler/sema/` | name resolution and type checking |
| `compiler/backend/` | the two backends: `interp_*.cpp` and `codegen_c.cpp` |
| `compiler/tools/` | `ovc fmt` and `ovc init` |
| `runtime/` | the C runtime compiled programs link against |
| `tests/interp/` | language tests, run through **both** backends |
| `examples/` | runnable examples (chatbot, rag, tool_agent) |
| `benchmarks/` | performance benchmarks and comparison scripts |
| `docs/` | the usage guide and the performance notes |

## The one rule that matters

Ovyth has two backends: a tree-walking interpreter (`ovc run`) and a native compiler (`ovc prog.ov`). They must agree.

`tests/interp.sh` enforces this. For every `.ov` in `tests/interp/` it runs the program through the interpreter *and* compiles it natively, then requires:

1. the interpreter's output to match the `.want` golden file, and
2. the native binary's output to equal the interpreter's, byte for byte.

Adding a feature to only one backend fails the suite. This is deliberate — a native backend that silently diverges from the interpreter is the failure mode that actually hurts users.

### Adding a language test

```bash
# 1. write the program
cat > tests/interp/016_my_feature.ov <<'EOF'
print("answer:", 6 * 7)
EOF

# 2. record what it should print as the golden file
build/ovc run tests/interp/016_my_feature.ov > tests/interp/016_my_feature.want

# 3. check both backends agree
bash tests/interp.sh
```

Read the golden file before committing it — a `.want` captured from a wrong unanimous output would enshrine the bug.

## Style

- **C++ (compiler):** C++17/20, four-space indent, `clang-format`-friendly. The code puts the *why* in comments and lets the code say the *what*; match that.
- **C (runtime):** C11, four-space indent, `ov_` / `ov_h_` prefixes.
- **Ovyth (`tests/`, `examples/`):** four-space indent.
- Keep the source grep-able. Prefer a clear function name over a comment that restates the code.

## Before you open a pull request

- [ ] `make test` passes (both backends, no divergences).
- [ ] New behaviour has a test in `tests/interp/`, or a reason it can't.
- [ ] `README.md` / `docs/USAGE.md` updated if user-visible behaviour changed.
- [ ] `CHANGELOG.md` updated under a new version heading.

CI (`.github/workflows/ci.yml`) runs the same steps on every push, plus a `ovc init` smoke test, so it's worth running `make test` locally first.

## Reporting a bug

Please include:

- the `.ov` program (minimised if you can),
- `ovc version`,
- what you expected and what happened,
- which backend: `ovc run prog.ov` or `ovc prog.ov && ./prog` — if they disagree, that's the most useful bug report there is.

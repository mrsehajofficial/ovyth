# The Ovyth package index

`ovc pkg` installs from a registry index — one JSON file listing every
package, its versions, and where each version comes from. That file is this
repository's `registry/registry.json`, and it is served over GitHub's raw
CDN, which is the default every `ovc` build points at:

```
https://raw.githubusercontent.com/mrsehajofficial/ovyth/main/registry/registry.json
```

There is nothing to deploy and nothing to run. Editing the JSON and merging
it here *is* publishing.

## Adding a package

1. Add an entry to `registry/registry.json`.
2. Commit, push, merge to `main`.
3. Users get it within a few minutes — GitHub's raw CDN caches briefly, and
   `ovc pkg` caches the index locally for an hour (`--refresh` forces a
   re-fetch).

```bash
ovc pkg search mypkg                # confirm it is listed
ovc pkg info mypkg                  # versions and source
ovc pkg install mypkg
```

## The format

```json
{
  "packages": {
    "greet": {
      "description": "greeting helpers — the canonical first package",
      "license": "MIT",
      "homepage": "https://github.com/you/ovyth-greet",
      "versions": {
        "1.0.0": {
          "repo": "https://github.com/you/ovyth-greet.git"
        },
        "1.1.0": {
          "url": "https://github.com/you/ovyth-greet/archive/v1.1.0.tar.gz"
        }
      }
    },
    "mytools": {
      "description": "Single-version package: no versions map needed",
      "version": "0.3.0",
      "path": "./packages/mytools"
    }
  }
}
```

Each version takes **one** source, in order of preference:

| key | what it is | how it is installed |
|---|---|---|
| `repo` | a git repository | shallow clone of the version, or of its `v`-prefixed tag |
| `url` | a tarball | downloaded and extracted |
| `path` | a local directory | copied (for testing, not for serving) |

Prefer `repo`. It keeps the code with whoever owns it, versions are tags
rather than file names, and the download traffic lands on the author's git
host instead of on this project.

`ovc pkg install greet@1.0.0` pins a version; `ovc pkg install greet` takes
the newest one listed.

## What the package itself needs

A package is a directory of `.ov` files with one entry module named after
the package, or `main.ov`:

```
mytools/
  mytools.ov      <- import "mytools"
  extra.ov        <- import "mytools/extra"
  README.md       <- what it is for, and how to use it
```

## Writing a module: the three rules

Modules are the part of the language you will get wrong if you write them
like Python. Three rules, all of them forced by how Ovyth works:

**1. Prefix every public name.** Ovyth flattens *all* top-level names from
every module and the entry file into one program. A package that exports
`search()` collides with the next package that does, and the compiler will
tell you both file names and stop. So the convention is a prefix:

```ov
// rag.ov — every public name starts rag_
function rag_chunk(text, size, overlap) { ... }
function rag_topk(store, query, k) { ... }
```

**2. Hold no state in the module.** Top-level `let` is a local of `ov_main`
and functions cannot see it on the native backend, and two packages sharing
a global name is a collision. So the caller owns the data and passes it in:

```ov
// the caller's store, serialisable, theirs to keep or discard
store = rag_build(docs, 8, 2)
hits = rag_topk(store, query, 2)
```

**3. Take data and return data.** Strings, ints, floats, lists and maps —
never a class, never a module-level singleton. Anything the module returns
you can `print`, `json.stringify` or hand to another package.

Two more that save time:

- **Comments are `//` and `/* */`.** `#` is not a comment character; a file
  that starts with one fails to parse.
- **Define helpers before they are used.** A name resolves at the call site,
  so a callee has to be written above its caller in the same file.
- **`^` is XOR, not power.** Use `sqrt(x)` and `pow(x, y)`; `2.0 ^ 0.5` is
  `2`, and a vector "normalised" with `^ 0.5` is not normalised at all.
- **A function value passed as a literal works; a named function reference
  does not.** Pass `function(x) {...}`, not the name of a `function` you
  declared.

## Publishing your own

The `repo` field points at a public git repository whose root holds the
entry module, tagged with the version (`v1.0.0` or `1.0.0`, both work). Add
the JSON entry here and it is installed.

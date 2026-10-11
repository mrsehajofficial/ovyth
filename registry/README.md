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
      "description": "Greeting helpers for scripts and CLIs",
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
```

Installing checks for that entry and refuses the package without it.
Top-level names must be unique across the whole program, installed packages
included — a collision is reported with both file names, same as for a local
`import`.

## Using a different index

The default is a starting point, not a restriction. `--registry=<url>` and
`OVYTH_REGISTRY` point at any index, a `file://` URL reads one off disk,
and a failed fetch falls back to whatever is cached locally — so an offline
machine installs from the last index it saw.

```bash
ovc pkg install greet --registry=https://example.com/my-index.json
export OVYTH_REGISTRY=https://example.com/my-index.json
ovc pkg install greet
```

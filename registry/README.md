# The Ovyth package index

`ovc pkg` installs from a registry index — one JSON file listing every
package, its versions, and where each version comes from. This directory is
where that file lives.

## Using an index

```bash
ovc pkg search greet --registry=https://your-host/registry.json
ovc pkg install greet --registry=https://your-host/registry.json

# or point every command at it
export OVYTH_REGISTRY=https://your-host/registry.json
ovc pkg install greet
```

The index is fetched over HTTP and cached in `$OVYTH_HOME` for an hour
(`--refresh` re-fetches). A `file://` URL reads one off disk, which is handy
for testing an edit before you publish it.

## Hosting it

Any static host works. The easiest is this repository:

1. Edit `registry.json` below.
2. Commit and push to the `main` branch.
3. Use the raw URL:
   `https://raw.githubusercontent.com/<you>/ovyth/main/registry/registry.json`

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
| `path` | a local directory | copied (mostly for testing) |
| `repo` | a git repository | shallow clone of the version, or of its `v`-prefixed tag |
| `url` | a tarball | downloaded and extracted |

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
Top-level names must be unique across the program, installed packages
included — a collision is reported with both file names, same as for a local
`import`.

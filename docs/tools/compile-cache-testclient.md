# compile-cache-testclient

A standalone client and validation harness for the compile cache. This is test
infrastructure, not a shipped product: it is not built or installed by default.

It is the **reference localizer**. It links the same `PathCanon` /
`CompileValue` code the server uses, so the canonicalization parity contract is
*exercised* rather than reimplemented — if the client and server ever disagree
about how a path is canonicalized, this is what catches it.

## Building

```sh
cmake --preset clang-debug -DFASTCACHED_BUILD_TESTCLIENT=ON
cmake --build --preset clang-debug --target compile-cache-testclient
```

## Usage

```
compile-cache-testclient <store|fetch> --port N [--key K] [--cohort C]
    --srcroot P --buildtree Q [--compiler cl|clang-cl] [--source F] [--out OBJ]
```

Run `compile-cache-testclient --help` for the generated reference; it is
rendered from the same option table the parser matches against, so it cannot
fall out of step with what the tool accepts.

Options take their value either as the next argument or joined with `=`, the
same as the daemon's own parser. An unrecognised option is an error rather than
something silently ignored.

| Flag | Default |
|------|---------|
| `--host` | `127.0.0.1` |
| `--port` | none — **required** |
| `--key` | empty |
| `--cohort` | `default` |
| `--srcroot`, `--buildtree` | empty |
| `--compiler` | `cl` |
| `--source` | empty |
| `--out` | store: a temp path; fetch: skip writing when empty |

### `store`

Runs a real compiler with `/showIncludes /c`, captures the object file and the
include output, frames them as a `CompileValue`, and STOREs it through a running
`fastcached` — carrying the *producer's* source-root and build-tree layout.

### `fetch`

FETCHes the canonical value, **localizes** every path to the *consumer's*
layout, optionally writes the object out, and verifies that each localized
header path resolves on disk. A path that does not resolve is a failure.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `2` | Usage error, socket failure, malformed value, or localization failure |
| `3` | The compiler failed during `store` |
| `4` | FETCH miss |
| `5` | FETCH hit, but at least one localized header path did not resolve on disk |

Code `5` is the interesting one: it means the cache returned an entry whose
dependency paths do not exist on the consuming machine — exactly the corruption
the canonicalization layer exists to prevent.

## Cross-depth validation

`run-crossdepth.ps1` proves the core value-portability guarantee: an object
compiled at a **deep** checkout path (mimicking a CI runner nested deeper) is
usable when fetched from a **shallow** checkout. That is the scenario that
poisons a build tool's dependency database when absolute paths from a
different checkout depth are replayed verbatim.

```powershell
# Synthetic: generated source, no project files involved.
pwsh src/apps/compile-cache-testclient/run-crossdepth.ps1 -Synthetic

# Rooted at real checkouts. Validates the real per-machine paths WITHOUT
# compiling or reading any project source: a generated tree is laid under a
# throwaway subfolder of each root and removed afterwards.
pwsh src/apps/compile-cache-testclient/run-crossdepth.ps1 `
    -LastradaRoots "D:\proj","D:\repo\proj"
```

## Privacy rule

This tool and its harness must **never** print, log, or persist project-private
information — no project source, no proprietary header names, no absolute
checkout paths — into anything under version control. The harness prints generic
status and counts only; its working files live in temp directories and throwaway
subfolders that are cleaned up afterwards.

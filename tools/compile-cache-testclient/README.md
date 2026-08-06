# compile-cache-testclient

A standalone client and validation harness for the compile-cache executor. It
is the **reference localizer**: it links the same `PathCanon` / `CompileValue`
code the server uses, so the canonicalization parity contract is exercised,
not reimplemented.

Not built by default. Enable with:

```sh
cmake --preset clangcl-debug -DFASTCACHE_BUILD_TESTCLIENT=ON
cmake --build --preset clangcl-debug --target compile-cache-testclient
```

## What it does

- `store` — runs a real compiler (`cl` or `clang-cl`) with `/showIncludes /c`,
  captures the object file and the `/showIncludes` output, frames them as a
  `CompileValue`, and STOREs it through a running `fastcached` over the custom
  0xFC protocol (carrying the *producer's* source-root / build-tree layout).
- `fetch` — FETCHes the canonical value, **localizes** every path to the
  *consumer's* layout, writes the object out, and verifies that each localized
  `/showIncludes` header resolves on disk. Exit code is non-zero if any
  localized path does not resolve.

## Cross-depth validation

`run-crossdepth.ps1` proves the core value-portability guarantee: an object
compiled at a **deep** checkout path (mimicking a CI runner nested deeper) is
usable when fetched from a **shallow** checkout — the exact scenario that
poisons Ninja's `.ninja_deps` today.

```powershell
# Synthetic (generated source, no project files):
pwsh tools/compile-cache-testclient/run-crossdepth.ps1 -Synthetic

# Rooted at real checkouts (validates the real per-machine paths WITHOUT
# compiling or reading any project source — a generated tree is laid under a
# throwaway subfolder of each root and removed afterwards):
pwsh tools/compile-cache-testclient/run-crossdepth.ps1 `
    -LastradaRoots "D:\lastrada","D:\repo\lastrada"
```

## Privacy rule (must hold)

This tool and harness must **never** print, log, or persist project-private
information — no project source, no proprietary header names, no absolute
checkout paths — into anything under version control. The harness prints
generic status and counts only; its working files live in temp directories and
throwaway subfolders that are cleaned up. Nothing it produces is committed.

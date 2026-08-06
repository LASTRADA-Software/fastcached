# compile-cache-testclient

A standalone client and validation harness for the compile-cache executor. It
is the **reference localizer**: it links the same `PathCanon` / `CompileValue`
code the server uses, so the canonicalization parity contract is exercised,
not reimplemented.

Test infrastructure, not a shipped product — not built by default:

```sh
cmake --preset clang-debug -DFASTCACHED_BUILD_TESTCLIENT=ON
cmake --build --preset clang-debug --target compile-cache-testclient
```

**User documentation lives in the docs site**, so there is one source of truth:
[compile-cache-testclient reference](../../../docs/tools/compile-cache-testclient.md)
— the `store` / `fetch` subcommands, every flag, the exit codes, and the
cross-depth validation harness.

## Privacy rule (must hold)

This tool and harness must **never** print, log, or persist project-private
information — no project source, no proprietary header names, no absolute
checkout paths — into anything under version control. The harness prints
generic status and counts only; its working files live in temp directories and
throwaway subfolders that are cleaned up. Nothing it produces is committed.

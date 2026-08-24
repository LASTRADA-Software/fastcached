# compile-cache-testclient

A standalone client and validation harness for the compile-cache executor. It
is the **reference localizer**: it links the same `PathCanon` / `CompileValue`
code the server uses, so the canonicalization parity contract is exercised,
not reimplemented.

Test infrastructure, not a shipped product — not built by default, but built by
the `linux` and `clang-tidy` CI jobs so that it cannot quietly stop compiling the
way it had (issue #84):

```sh
cmake --preset clang-debug -DFASTCACHED_BUILD_TESTCLIENT=ON
cmake --build --preset clang-debug --target compile-cache-testclient
```

It drives either compiler family — MSVC spellings plus `/showIncludes`, or GNU
spellings plus `-MD -MF` — and tags the stored region with whichever grammar
produced it. Its socket, its process spawning and its dependency-path check are
all the same code the daemon and the launcher use, which is the point: this tool
exists to prove the canonicalization contract, so re-implementing either side of
it here would prove nothing.

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

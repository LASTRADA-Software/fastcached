# fastcache-cc

A drop-in, sccache-style **compiler launcher** that uses fastcached as a
cross-machine-portable compile cache over the custom `0xFC` protocol. It fronts
every compile, serves cache hits (reproducing the object and replaying compiler
output), and falls back safely to a real compile whenever the cache is
unavailable — so it can never break a build.

**User documentation lives in the docs site**, so there is one source of truth:

- [fastcache-cc reference](../../../docs/tools/fastcache-cc.md) — environment
  variables, flags, statistics, exit codes, fall-back reasons, limitations.
- [Compile-cache protocol](../../../docs/protocols/compile-cache.md) — the
  `0xFC` wire format and the canonicalization contract.
- [Running a compile cache](../../../docs/operations/compile-cache-server.md) —
  daemon sizing, notably `--storage-max-value`.

## Layout

| File | Contents |
|------|----------|
| `main.cpp` | The launcher flow: config, key derivation, FETCH/replay, MISS/STORE, `--show-stats`. |
| `CmdLine.*` | The driver descriptor table and command-line parsing. |
| `CacheKey.*` | Key computation and argument relativization. |
| `DirectManifest.*` | Direct mode: the header manifest that avoids preprocessing. |
| `Stats.*` | The invocation log and its report renderer. |
| `IProcessRunner.hpp`, `ProcessRunner.cpp` | Process-spawn seam; Windows and POSIX implementations. |
| `EndpointDial.hpp`, `EndpointDial.cpp` | Turns `"host:port"` into a connected `ISocket`. The socket itself is the library's `Net/TcpClient`; this is only the join between `Core/HostPort` and it, which `Net` must not make itself. |

Built by default (`FASTCACHED_BUILD_LAUNCHER=ON`) and installed alongside
`fastcached`.

## Validation

`run-launcher-e2e.ps1` drives the launcher as the compiler with real `cl` and
`clang-cl` on Windows; `scripts/compile-cache-e2e.sh` does the equivalent on
POSIX. Both assert the same contract: first compile MISSes and stores; second
(object deleted) HITs and reproduces the object byte-identically; content
stored from a deep checkout HITs from a shallow checkout. CI runs both.

```powershell
pwsh src/apps/fastcache-cc/run-launcher-e2e.ps1
```

Unit tests build as `fastcache-cc-tests` and run under `ctest`.

## Privacy

Contains no project-specific data; it compiles whatever it is pointed at. The
E2E harness prints generic status only and cleans up its temp trees. Nothing it
produces is committed.

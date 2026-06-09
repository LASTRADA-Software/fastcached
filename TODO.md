# TODO items

- [ ] CoW storage needs to expose the on-disk format version, such that we can identify old stores on newer fastcached versions with a newer on-disk format (to gracefully bail, or migrate)
- [ ] Add valgrind memcheck CI job to ensure no memory issues on clang release build
- [ ] Github CI should create a static `fastcached` binary and provide it as artifact.
- [ ] Add `fastcached stats` command to show in-process stats of the currently running instance (assuming we auto-detect it via config file)
- [ ] Add `fastcached live-stats` command to show in-process stats live on the terminal (using Sixels, if supported by the connected terminal, otherwise Unicode or ASCII art)
- [ ] Reactor `UpdateInterest` swallows real `kevent`/`epoll_ctl` failures — surface them instead of stalling. See risk note below.
- [ ] **Data-driven `ConfigMerge` (review follow-up — Group D #12).**
      The current `Merge()` in `src/FastCache/Config/ConfigMerge.cpp` is 25
      hand-rolled if-branches; adding a Config field requires touching
      Config + CliResult bit + CliParser + YamlReader + Merge, and the
      `lruRecency` / `cpuAffinity` silent-drop bug from the
      transactions-keyspace-dual-listener review was the predictable
      consequence. Replace the if-ladder with a descriptor table
      (`{explicit-bit-getter, src-field-getter, dst-field-setter, mode}`,
      where `mode` is one of `ExplicitWins` / `NonDefaultWins`) and a
      coverage test that asserts every `CliResult::*Explicit` bit is
      referenced by exactly one descriptor — that guard fails fast the next
      time someone adds a field without wiring it into Merge.
- [ ] **`NotifyingStorage` IStorage decorator (review follow-up — Group D #13).**
      Keyspace notifications are currently scattered across each Redis verb
      handler in `RedisResp.cpp` (~12 `NotifyKeyspace` call sites). This
      bolts notifications to the Redis protocol only — Memcached writes
      silently miss keyspace events, and lazy-expiry has no callback to fire
      `expired` at all (which is why the `x` flag and `Expired` event class
      had to be removed from the parser in #3). The right altitude is an
      `IStorage` decorator (`NotifyingStorage` wrapping the inner storage),
      matching the existing `LayeredStorage` / `ShardedStorage` /
      `TracingStorage` decorator chain pattern. Every mutation funnels
      through `IStorage::Set/Delete/CompareAndSwap/Update/Remove`, so one
      decorator publishes for every protocol *and* every backend. Once
      this lands, restore the `x` flag and `Expired` class in
      `KeyspaceNotifier`, and the per-protocol `NotifyKeyspace` scatter
      in `RedisResp.cpp` collapses to a no-op (the decorator publishes
      first, the protocol handler only handles the reply).
- [ ] **Startup-failure log on partial bind (review follow-up — Group D #14).**
      `RunMultiReactorPosix` in `src/FastCache/Server/ReactorServerLoop.cpp`
      binds `reactorCount × binds` listeners. When one bind fails mid-loop,
      the function logs a generic "cannot bind A:P on reactor R" and
      returns; the storage chain and reloader thread are already running.
      Add an `OnBindFailure` log path that names which (reactorIndex,
      bindIndex) pair failed and surfaces the kernel errno verbatim, so the
      operator can distinguish "port already in use on reactor 0" from
      "fd-table exhaustion on reactor 14". Inject a fake listener factory in
      `ReactorServerLoop_test.cpp` (create if absent) that succeeds for
      the first N calls then fails, assert the message names the
      offending pair.
- [x] Add support for Tracy (via CMake variable, off by default)
- [x] disk storage should be btrfs-like copy-on-write to allow O(1) blocking disk saves

## Risk note: ignoring `Attach`/`UpdateInterest` return values in the socket layer

**Context.** `KqueueSocket.cpp` / `EpollSocket.cpp` call the reactor's
`[[nodiscard]] bool Attach(...)` and `bool UpdateInterest(...)` at fire-and-forget
sites and discard the result with `std::ignore`. Adding the macOS CI job surfaced
this on the kqueue side (`-Werror,-Wunused-result`); the epoll side already did it.

**Is ignoring the return safe? Mostly yes, today — but it hides a stability hazard.**

- Both methods return `false` *only* on precondition violations
  (`handler == nullptr`, `fd < 0`, dead reactor `_kq`/`epfd < 0`). At every
  ignore site the fd was just created/accepted and is known-valid, so `false` is
  unreachable in practice. Ignoring a value that is always `true` is harmless.
- The genuinely risky part is **inside** `UpdateInterest`: the real
  `::kevent(...)` (resp. `epoll_ctl`) result is itself `(void)`-discarded, so a
  `true` return does **not** mean the kernel accepted the interest change. The
  discard is intentional for the benign `ENOENT`-on-`EV_DELETE` case, but it also
  swallows *every other* failure (`EINVAL`, `ENOMEM`, `EMFILE`/fd-table pressure,
  etc.).

**Stability impact.** If an interest-arming call fails for a non-benign reason,
the reactor silently stops delivering readiness for that fd. The owning
connection coroutine then **suspends and never resumes** — a silent, permanent
per-connection stall with a leaked fd, rather than a clean error the client can
observe and retry. For a long-lived daemon whose primary goal is stability, a
silent hang is a worse failure mode than a reported error. This affects Linux and
macOS equally.

**Recommendation (not blocking; tracked above).**
1. Distinguish benign `ENOENT`-on-delete from real failures inside
   `UpdateInterest`; on a real arming failure, propagate it so the pending
   `IoAwaitable` is completed with an error (clean connection teardown) instead of
   stalling. Apply symmetrically to `EpollReactor` and `KqueueReactor`.
2. Once failures are reportable, reconsider the call-site contract: either keep
   `std::ignore` only where the return is provably non-actionable, or `assert()`
   the precondition-only sites to catch misuse in debug builds.
3. Leaving `std::ignore` in place for now is an acceptable interim state — it
   matches the existing epoll behaviour and does not regress anything — but the
   silent-failure swallow in (1) is the real fix for the stated stability goal.

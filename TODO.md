# TODO items

- [ ] CoW storage needs to expose the on-disk format version, such that we can identify old stores on newer fastcached versions with a newer on-disk format (to gracefully bail, or migrate)
- [ ] Add valgrind memcheck CI job to ensure no memory issues on clang release build
- [ ] Github CI should create a static `fastcached` binary and provide it as artifact.
- [ ] Add `fastcached stats` command to show in-process stats of the currently running instance (assuming we auto-detect it via config file)
- [ ] Add `fastcached live-stats` command to show in-process stats live on the terminal (using Sixels, if supported by the connected terminal, otherwise Unicode or ASCII art)
- [ ] Reactor `UpdateInterest` swallows real `kevent`/`epoll_ctl` failures — surface them instead of stalling. See risk note below.
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

# Platform integration, service registration and configuration

Rules for `src/FastCache/Platform/` and `src/FastCache/Config/`, plus the CLI
option tables both binaries drive their parsing and help from.

Read this before touching `ServiceControl`, `IDaemonHost`, `DefaultConfigPath`,
`FileTrust`, `HostInfo`, `CliOptions()`/`CliParser`, `NodeConfig`, or any
packaged unit/plist/WiX fragment that launches a binary.

Two failure shapes recur: a registration that **succeeds** and then produces a
service which cannot do its job, and a configuration file that is present,
readable and silently ignored. Every rule below has already been one of them.

## Registering a service

- **A service to register is a `ServiceSpec`, and what it runs as is part of it.**
  Every function in `Platform/ServiceControl` took `Config const&` -- the *daemon's*
  configuration type -- so a second binary could reach none of it without either
  depending on the daemon's configuration or growing a parallel copy of a 1273-line
  file. That is the whole reason `fastcache-compile-node` had no service integration
  on macOS or Windows. Four things about the seam's shape are load-bearing:
  - **`daemonFlag` is a field rather than an argument**, because the two supervisors
    disagree and one spec has to answer both: the Windows SCM needs it (it is the
    hook that hands control to `WindowsServiceHost`) and launchd needs its absence,
    since like systemd it supervises the process it started and reaps a job that
    forks as "exited".
  - **`serviceAccount` and `ownedDirectories` replaced a constant and a reach into
    `cfg.storagePath`.** Which account a service runs as, and which directories it
    must own before its first write, are properties of the *service*. The rule they
    carry is unchanged and still the point: only directories the operator actually
    named, never a parent -- `--storage=/var/db/fc` must not hand `/var/db`, shared
    with other system services, to an unprivileged cache account.
  - **An empty `serviceAccount` means root, so a worker names one.** A system-scope
    launchd job with no `UserName` runs as root, and `fastcache-compile-node`
    compiles input that arrived over the network. Naming `fastcache-node` -- the
    account the Linux unit already uses -- puts the existing "that account does not
    exist" guard in the way, so a macOS system-scope install **refuses** until the
    package creates it rather than silently succeeding with root privileges
    (issue #87). It is deliberately not the daemon's `_fastcached`: one account
    would let a compromised compile rewrite every cached object.
  - **`BuildServiceArgv` stays hand-written per binary.** An `OptionSpec` says how
    to *parse* a flag and carries no way to read a value back out, so "emit every
    field that differs from its default" cannot be written once generically. Each
    binary's version is guarded by a test that walks its own option table and
    requires every non-excluded row to be emitted -- the daemon's table once
    stopped after nine fields, so `--install-service --tls --metrics` reported
    success and registered a plaintext, unmonitored daemon.
  - **The refactor was checked to change nothing rather than argued to.** The
    registered command line is byte-identical to the one the previous version
    produced, which is why `--daemon` is inserted at the *front*: nothing downstream
    cares about flag order, and a refactor that can be shown to be identity is worth
    more than one that merely ought to be.
- **An install-time refusal is the only place an operator is watching.** Every rule
  in `NodeServiceRejection` describes a registration that would **succeed** and then
  produce a service which cannot do its job -- silent from both ends, since the
  operator is told it was installed and nothing later says otherwise. `--advertise`
  is the one worth naming: left empty it bakes in `{--bind}:{--port}`, and `--bind`
  defaults to `0.0.0.0`, which is not an address a client can dial. Such a worker
  registers, heartbeats happily, is leased out by the scheduler, and is never
  reached. `--scheduler` and `--toolchain` get the same treatment because each
  would start and exit at every boot. Two related choices: an **uninstall** is not
  gated the same way -- refusing to remove a registration because it was
  misconfigured is how a bad one becomes permanent -- and a bare compiler path is
  **not** resolved to a fingerprint at install time, because the worker derives that
  at startup through the identical code its clients use and a digest computed once
  would pin the registration to a toolchain an update then changes underneath it.
- **The supervisor's launch arguments must not pass `--daemon`.** The POSIX
  daemonize path double-forks and sends stdout/stderr to `/dev/null`, which
  silences journald; its pidfile is also written after both parents exit, racing
  `Type=forking`. launchd has the identical problem — it reaps the forked job
  instantly as "exited" — which is why `BuildServiceArgv` takes an
  `EmitDaemonFlag` rather than always emitting it.
- **`--install-service` registers the *command-line* config, not the merged
  one.** A flag in `ProgramArguments` outranks the same key in YAML for the life
  of the registration, so baking merged values in froze every configured key at
  install time and made later edits to that same file silent no-ops — and copied
  `requirepass:` out of a mode-0640 file into a world-readable plist. Hence
  `main.cpp` hands `parsed->config` to `InstallService`, and
  `InlineCredentialRejection` refuses a `--requirepass` typed on the install
  command line rather than dropping or publishing it — *including* alongside
  `--config`, since nothing there can tell whether the named file carries the
  secret, and accepting it was the silent drop under another name.
- **What reaches a supervisor must survive its own parser.** Every registration
  flag is re-read by the daemon at the next start, so a value that cannot be
  spelled back is a service that registers cleanly and then fails forever:
  `--listen=[::]:11211` came back as `--listen=:::11211`, which the CLI rejects,
  and a Windows path ending in `\` escaped its own closing quote and swallowed
  the flags after it. `FormatListenHost` and `MaybeQuote` are where that round
  trip is kept honest. `ServiceNameRejection` covers the other direction: the
  name is concatenated into the directory launchd scans, so a separator writes a
  root-owned plist nothing knows how to remove.
- **Teardown must address every domain, not re-probe for one.** Which launchd
  domain a user agent lives in is decided at install time — `gui/<uid>` needs an
  Aqua session, so an SSH install lands in `user/<uid>`. Re-probing at uninstall
  booted out a job that was never there and reported success while the real one
  kept the port. `BootOutEverywhere` walks the whole `ScopeTraits::domains` row,
  and `fastcached-uninstall` mirrors it.
## Finding and trusting a configuration file

- **A candidate config location names an application, not `fastcached`.** Every row
  of `DefaultConfigCandidates()` hardcoded the daemon's file name, so a second
  binary had nothing to generalize onto. The rows carry `{app}` now, and two things
  about that are deliberate: the table stays `constexpr` so its four `static_assert`s
  keep stopping the *build* rather than waiting for `ctest`, and the substitution is
  a literal replace rather than `std::format` -- a display form already contains
  `%VAR%` on Windows and `$VAR` on POSIX, so a third meta-syntax whose braces are
  also `std::format`'s own grammar is how a path containing a brace becomes a thrown
  exception at startup.
- **A config the operator named is strict; one the daemon found is not.**
  Without `--config`, `DefaultConfigPath` walks a per-platform candidate table
  (user location before machine-wide) and takes the first entry that exists *and
  opens for reading* — `Config/DefaultConfigPath.cpp` is the single source of
  truth for that order, for what `--help` lists, and for where `--seed-config`
  writes. Readability, not mere existence, is the test: the macOS system config
  is mode `0640 root:_fastcached`, so a per-user agent has to fall through it
  rather than fail to start. A discovered file that is absent or unreadable is
  skipped silently; one that parses badly is still fatal, as is a missing file
  named with `--config`. The resolved path goes into a *local* in `main.cpp` and
  never back into `parsed->config` — that object is what `InstallService`
  registers, and a discovered path baked into `ProgramArguments` would outrank
  the file itself forever (see the next bullet) and make
  `InlineCredentialRejection` name a path nobody typed.
- **The lookup's two rules both turn on one question: is this process the
  machine-wide daemon, or somebody's own?** `probe.IsPrivilegedProcess()` (root;
  elevated administrator or LocalSystem on Windows, via `CheckTokenMembership`,
  which is false for the unelevated half of a split token) decides both halves,
  and neither is driven by the platform or by the row alone:
  - **Unprivileged runs skip every `ConfigScope::System` row.** The machine-wide
    file describes the system service, whose cache only the service account can
    write, so a `systemctl --user` instance that adopted its `storage_path:`
    would fail to open that directory and be respawned until the start limit
    tripped — the out-of-the-box restart loop the user unit's header says it
    exists to prevent. Readability is not enough of a filter here: the packaged
    `/etc/fastcached/fastcached.yaml` is `0644 root:root` and readable by all.
  - **Privileged runs trust-check *every* row, per-user ones included.** `$HOME`
    and `$XDG_CONFIG_HOME` are inputs an unprivileged account often controls and
    sudo does not always reset, so checking only `System` rows would leave
    `sudo -E fastcached` taking root's `storage_path:` from a file that account
    wrote. A path named with `--config` is never checked — that is the operator's
    assertion to make, and it is the escape hatch when a location is refused.
- **A machine-wide config is only obeyed when only an administrator could have
  written it.** `C:\ProgramData` grants `BUILTIN\Users` create-file on every
  subdirectory it hands down to, so moving the Windows config there made the
  configuration of a *LocalSystem* service plantable by any standard account —
  `storage_path:` alone turns that into arbitrary directory creation and file
  writes as SYSTEM. Two halves, and both are needed: the MSI owns
  `%ProgramData%\fastcached` and gives it a protected access list of its own
  (`PermissionEx`, not `Permission` or `util:PermissionEx` — those take an
  account *name* and are wrong on a non-English Windows), and `DefaultConfigPath`
  refuses a candidate whose directory fails `Platform/FileTrust`. The test there
  is the containing directory's owner and entries, never the file's owner: on
  Windows a new object belongs to its *creator*, so a config seeded by hand from
  an elevated shell is owned by that administrator's own account, while a file
  planted by a standard account is granted to that account's own SID through the
  inherited `CREATOR OWNER` entry — an owner whitelist rejects the first and an
  entry scan misses the second. A rejection goes to stderr *and* through the
  logger once there is one: a service started by the SCM has no console, so
  stderr alone would put the message nowhere in the one deployment where a
  machine-wide config is the norm, and a file that is present, readable and
  ignored anyway is the silent no-op this list exists to prevent.
- **`--seed-config` secures the directory before it looks for the file, not
  after.** The whole point of the repair is the case where something is *already*
  there: any standard account can create a `%ProgramData%` subdirectory and drop
  a config into it long before the installer runs, and the MSI cannot undo that
  on its own — `PermissionEx` replaces the access list but not the owner, who
  keeps `WRITE_DAC` regardless. So seeding secures the parent first (repairing a
  squat), then decides what to do about the file: seed-once keeps an operator's
  edits, but a file found in a directory that until that moment anybody could
  write is not established to be an operator's, and is reported rather than
  blessed by silence or destroyed by overwriting. Seeding refuses outright when
  it has the rights for none of this, and deletes a directory it created but
  could not secure — which would otherwise be the very shape being defended
  against, authored by the defence.
- **`ExecStart` still passes `--config` on Linux and macOS — by choice, not
  necessity.** It predates the lookup, where its absence made `ConfigReloader`
  have nothing to re-read and `systemctl reload` a silent no-op; the lookup now
  closes that hole for every daemon started without the flag. The packaged units
  keep it because the path is unambiguous there and CI asserts it. Windows goes
  the other way: its custom action registers *no* `--config`, so a seed that did
  not happen degrades to built-in defaults instead of a service that fails at
  every start. The per-user launchd agent and the systemd user unit pass none —
  the packaged config describes the system daemon, whose cache only the service
  account can write.
## The CLI option table

- **A flag is one row, and every binary's row table drives both parsing and
  help.** The daemon used to declare flags four ways — hand-written `if (arg ==
  …)`, a descriptor array, two inline `initializer_list<tuple<…>>` tables, and
  seventeen copy-pasted blocks in `HandleTypedFlag` — and then spell every name
  again in a separate help table, so a flag could be accepted but undocumented
  or documented but rejected. `CliOptions()` is now the single source of truth,
  and the help column is *derived* from `primary`/`alias`/`operand` rather than
  restated. Rendering lives in `Cli/UsageDoc`, which must stay dependency-free:
  `fastcache-cc` compiles it in rather than linking `FastCache`, so an include
  of anything from `Config/` there breaks the launcher's link, not just its
  build. Two remaining hand-written spellings are guarded by tests rather than
  generated: `BuildServiceArgv` (a `ServiceControl_test` case walks
  `CliOptions()` and requires each non-excluded flag to be emitted — the
  exclusions, `--requirepass` above all, are listed with their reasons) and the
  launcher's `FASTCACHE_*` oracle list in `LauncherCli_test`.
## The daemon host, and what a machine IS

- **A daemon host wraps the body, so what must reach a terminal has to happen
  first.** `WorkerBody` is separate from `main` because `IDaemonHost` double-forks on
  POSIX or hands control to the SCM, and neither can wrap a `main` that has already
  parsed, validated and registered. The cheap, fallible checks stay in `main`
  deliberately: run inside the body they would print their diagnosis to a stdout the
  POSIX host has already redirected to `/dev/null`, so a misconfigured worker would
  exit in silence.

- **An architecture is what the compiler built for, not what the kernel is running.**
  `QueryHostFacts` reads the architecture from the compiler's own macros rather than
  from `uname`/`GetNativeSystemInfo`, because an x86-64 process under Rosetta or
  WOW64 executes x86-64 code on a machine that truthfully reports `arm64`. A
  scheduler weighing this node has to be told what the binary that will run a compile
  is, not what silicon is under it. Windows's version comes from `RtlGetVersion` and
  not `GetVersionEx`, which reports 6.2 for every release since Windows 8 unless the
  caller ships a compatibility manifest. And free space is `space.available`, not
  `space.free`: the difference is the root-reserved portion, which an unprivileged
  worker cannot write and must not offer to a scheduler as room it has.


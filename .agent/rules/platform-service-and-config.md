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
  - **`serviceAccount` and `ownedPaths` replaced a constant and a reach into
    `cfg.storagePath`.** Which account a service runs as, and which paths it must
    own before its first write, are properties of the *service*. The rule they
    carry is unchanged and still the point: only paths the operator actually named,
    never a parent -- `--storage=/var/db/fc` must not hand `/var/db`, shared with
    other system services, to an unprivileged cache account.
  - **An empty `serviceAccount` means root, so a worker names one.** A system-scope
    launchd job with no `UserName` runs as root, and `fastcache-compile-node`
    compiles input that arrived over the network. Naming `fastcache-node` -- the
    account the Linux unit already uses -- puts the existing "that account does not
    exist" guard in the way, so a macOS system-scope install refuses rather than
    silently succeeding with root privileges. It is deliberately not the daemon's
    `_fastcached`: one account would let a compromised compile rewrite every cached
    object. **The package creates it** (issue #87), from the *Runtime* component and
    not the LaunchDaemon one -- which launchd choice an operator made for
    `fastcached` says nothing about whether they will run a worker, and the
    installer's default is the per-user agent. One spelling reaches the binary, the
    packaging and the Linux unit; `ctest -R service-accounts` fails on every
    platform when they drift, because the failure itself is macOS-only and silent.
  - **An empty `applicationName` means "configured entirely from argv", and
    `WithScopeDefaults` must then bake in nothing.** It used to look the machine-wide
    config up under `DaemonApplicationName` unconditionally, so *every* spec got the
    *daemon's* defaults: a worker registration was handed `--config=` (system) or
    `--storage=` (user), and `NodeOptions()` has neither flag. That is the
    supervisor-must-survive-its-own-parser rule below, produced by the installer
    itself -- the job registered, reported success, and answered `unrecognised
    argument` at every boot. On a packaged macOS system it was worse than that: the
    daemon's config is `0640 root:_fastcached`, so `ServiceAccountReadDenial`
    refused the worker install for a reason that had nothing to do with the worker.
    `WithScopeDefaults` was private to the `__APPLE__` block for all of this, which
    is exactly why no test saw it; it is declared in the header now, like
    `BuildLaunchdPlist`, and asserted on every platform.
  - **`ownedPaths` is not just the daemon's `--storage`.** A worker given
    `--cache-dir` or `--cluster-dir` gets directories root created for an account
    that is not root, so they must change hands too -- and only those, never a
    parent.
  - **An owned path is created only when it is absent AND is not a file, and is
    otherwise handed over as whatever it already is.** The field is `ownedPaths`
    and not `ownedDirectories` because `storage_path` is allowed to name ONE CoW
    file -- `ResolvePhysicalShards` keeps a path with a file extension single-file
    for backward compatibility. Both handovers used to `create_directories` over
    their entries unconditionally, and that failed in both directions on the same
    value: `--storage=D:\fc\cache.cow` on a fresh install produced a *directory* of
    that name, which the very next start read as a shard directory and silently
    fanned out into; and once the file existed, the create *failed*, so the
    chown/grant was skipped on exactly the upgrade the handover is there for.
    `Core/PathKind`'s `PathNamesAFile` is the one answer to "is this a file", and
    `ResolvePhysicalShards` asks it too, so the shard count and what the installer
    may create cannot drift apart. A `chown` and an `SE_FILE_OBJECT` DACL apply to
    a file just as well as to a directory, so only the *create* has to care --
    which is why the refusal lives in the **handover** and not in each producer of
    an owned path. `MakeDaemonServiceSpec` and `main.cpp`'s install branch are two
    such producers of the very same value, and gating one of them was the first,
    wrong, shape of this fix.
  - **`serviceAccount` does not answer the SCM, so `windowsLogon` does.** The two
    supervisors take different *kinds* of answer: launchd wants an account name that
    must already exist, while the SCM derives a per-service identity from the
    service's own name. `CreateService` passed `nullptr` for `lpServiceStartName`,
    which is **LocalSystem** -- so on Windows `fastcache-compile-node` registered
    itself with the whole machine while the macOS spec was carefully naming an
    unprivileged account for the same binary. It names
    `WindowsLogonAccount::VirtualAccount` now: `NT SERVICE\<serviceName>`, created by
    the SCM itself, no account to make and no password to keep. **Both** services name
    one; neither has any use for the local Administrators group. The daemon can still
    read what it needs, because the `%ProgramData%\fastcached` access list grants
    `BUILTIN\Users` read and execute and a virtual account is an Authenticated User --
    and what it can no longer do is WRITE there, which is the point: a service that
    cannot rewrite its own configuration cannot be made to load a different one.
  - **The daemon's storage is granted from the MERGED config, and that is not the
    `--install-service` rule below being broken.** `storage_path` is read from YAML at
    every start, so a registration built from the command line cannot see it -- and the
    daemon no longer runs as an account that can write wherever it likes. `main.cpp`'s
    install branch therefore adds `effective.storagePath` to `ownedPaths`.
    Nothing from the merged config is *registered*: this contributes a path to hand
    over, never a flag to bake in, so the hazard that rule exists for -- a path in
    `ProgramArguments` outranking the very file it came from, forever -- cannot arise.
    Only what is configured, never a speculative default: a daemon with no
    `storage_path` is memory-only and needs no directory at all. Whether that path may
    be *created* is not decided here but in the handover (see `ownedPaths` above),
    because `MakeDaemonServiceSpec` contributes the same value from the command-line
    config and gating one producer leaves the other open. An
    operator who adds one *after* installing gets a permission failure naming the
    account and the `icacls` line, because at that point nothing has handed the
    directory over -- and the message must not tell them to re-run
    `--install-service`, which returns on `ERROR_SERVICE_EXISTS` *before* the grant
    loop and would repair nothing. The startup hint is gated on an actual write probe
    rather than on the error text, because three of the four storage failures report a
    string from the storage layer rather than an `error_code`, and because a localized
    "permission denied" is not something to build advice on. It is also gated on
    `--daemon`: the probe proves only that *this* process cannot write, so a foreground
    run by an administrator would otherwise be handed a diagnosis naming an identity
    that is not running it, and an `icacls` line for an account that may not be
    registered at all.
  - **A virtual account cannot write what an administrator created, so the install
    grants it.** LocalSystem never noticed, which is why nothing on the Windows path
    had ever needed the equivalent of the launchd `chown`. Two things about the
    grant: `NT SERVICE\<name>` does not resolve until the service exists, so it runs
    **after** `CreateService` (before it, `SetEntriesInAcl` fails with
    `ERROR_NONE_MAPPED`); and the entry is *added* to the existing list rather than
    replacing it, or the directory becomes one only the service can repair. A grant
    that fails is reported and the registration kept -- an operator can fix an ACL,
    but not a registration that was rolled back.
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
  reached. `--scheduler` gets the same treatment because it would start and exit at
  every boot.

- **An install is judged by the STARTUP rules as well, through
  `NodeInstallRejection` -- never by `NodeServiceRejection` alone.** The install
  branch returns before `StartupPolicyRejection` is ever reached, so for a while it
  applied four rules where seventeen were fatal: `--install-service` with
  `--tls-cert` and no `--tls-key` registered cleanly and exited `ExitUsage` at every
  boot, and so did `--dashboard` with no `--admin-listen` and a dozen more
  ([#166](https://github.com/LASTRADA-Software/fastcached/issues/166)). Every
  startup rule is a **pure invariant of the parsed configuration** -- no clock, no
  filesystem, no port -- so each is decided the moment the command line is typed,
  and `--install-service` bakes that command line in. That makes a startup rule
  strictly *more* worth refusing at install than at a start: a start refuses once,
  in front of the person who typed it.

  The two tables stay separate and `NodeInstallRejection` only composes them, so a
  new row in either reaches the install path without anyone remembering to add it
  twice. `StartupPolicyRejection` must keep running at startup as well, or a
  hand-started worker makes the identical mistake in silence. And the composition
  lives in `NodeConfig.cpp` rather than `main()`, because `main()` is in no test
  target -- which is exactly how a gap between two well-tested tables survived.

  `--toolchain` used to, and **no longer does** -- the reversal is the whole of
  [#139](https://github.com/LASTRADA-Software/fastcached/issues/139). Registering a
  worker before anybody knows what a machine holds is precisely what makes
  installing the package the entire setup; the node surveys the machine at boot and
  answers the question itself. What still cannot work is `--no-toolchain-discovery`
  with nothing named, and that shape alone is refused. The flag therefore has to
  survive the round trip into the registration: it changes what the service DOES at
  every boot, so losing it would leave the worker quietly serving compilers the
  operator excluded, with nothing saying the registration had changed meaning.

  Two related choices: an **uninstall** is not gated the same way -- refusing to
  remove a registration because it was misconfigured is how a bad one becomes
  permanent -- and a bare compiler path is **not** resolved to a fingerprint at
  install time, because the worker derives that at startup through the identical
  code its clients use and a digest computed once would pin the registration to a
  toolchain an update then changes underneath it.
- **A rule that is fatal at every boot belongs in a table, not in the tier that
  needs it.** `ConsensusTier::Start` made three refusals that were pure functions of
  the parsed configuration -- a malformed `--raft-peer`, a `--node-id` naming no
  peer, and an unusable `--listen-raft` -- and the install path returns long before
  any tier is built. So `--install-service --node-id=n1` with no peer passed both
  rejection tables, was reported installed, and exited `ExitUsage` at every boot
  ([#168](https://github.com/LASTRADA-Software/fastcached/issues/168)). The rule
  above closed that shape for the rules in the tables; this is what keeps a rule
  from living outside them in the first place. Being *reachable only through a
  tier* is not a reason for a check to live there: the question is whether the
  answer depends on anything but the parsed configuration, and if it does not, the
  table is where it goes.

  Where it goes depends on what the answer needs to say. A **grammar** goes in the
  option table -- `--raft-peer` holds `Cluster::ClusterMember`, parsed by the row
  that accepts it, which refuses a bad token on every path and *names the token*.
  A **cross-flag invariant** goes in `StartupPolicyRejection`, whose messages are
  static prose and so can name flags but no values. When both a row and a tier must
  assert one rule, they share a predicate and a message constant -- `ClusterSelfMember`
  and `NodeIdNamesNoPeerRefusal` -- because a rule asked two ways is one that drifts,
  which is the whole reason for a table. And a new row goes **after** any narrower
  rule about the same flags: first match wins, so `--raft-join needs --raft-peer`
  would otherwise be answered in its place and become a rule nothing reaches.

  Two things that look like members of the group and are not. `--cluster-dir` is
  read by `FleetHistoryPath` for the dashboard's history file, so it is *not*
  refused alongside `--listen-raft` when there is no `--node-id` -- a node running
  no consensus still has a use for it. And a row asks `ParseEndpoint` rather than
  `raftListen.empty()`, so an unusable port is refused with the missing one, the
  way the `--dashboard` row judges the address `AdminEndpoint` will actually take
  rather than the text an operator typed.

  **The line is "pure function of the command line", and it is narrower than "would
  be fatal".** `--listen-scheduler`, `--admin-listen`, `--listen-cache` and
  `--discovery` were the same defect one tier along
  ([#186](https://github.com/LASTRADA-Software/fastcached/issues/186)): each names an
  address, each grammar was checked where the surface is opened, and a typo therefore
  registered cleanly and killed the node at every boot. They are one `EndpointFlag`
  table now, because four rules differing only in a flag name and a grammar are the
  repetition a table exists to remove -- and `--listen-cache` is in it, which is easy
  to miss: its failure is fatal only once the address is *named*, and a value that
  does not parse is named by construction.

  What stayed out needs stating carefully, because the easy reason is the wrong one.
  `--bind`, `--advertise`, `--scheduler`, `--upstream` and `--fleet-member` are
  addresses on the same command line, and it is tempting to say they are excluded
  because they fail at `bind()` or `connect()` -- but that is about **reachability**,
  and their *grammar* is every bit as much a pure function of the command line as a
  listen flag's. The honest split is narrower: this table covers the surfaces this
  node OPENS, whose grammar was already being checked in a tier and only in the wrong
  place. Whether a dialled address is well-formed is a rule nobody has written yet,
  not a rule deliberately declined
  ([#208](https://github.com/LASTRADA-Software/fastcached/issues/208)). What genuinely
  cannot move here is only the resolving: an address that does not exist when a
  service is installed may exist by the time it boots.

  **A host somebody did not write is not a host they meant.** All four rows refuse an
  empty one, and it is worth knowing why that is not fussiness: an empty BIND host
  reaches `getaddrinfo` as nullptr under `AI_PASSIVE` (`SocketAddress.cpp:163`), which
  is the WILDCARD -- so `--listen-cache=:6674` would quietly serve this node's private
  cache to the whole network, which is exactly what `CacheListenDefaultHost`'s loopback
  exists to make a decision rather than a typo. `UdpSocket::Send` refuses an empty
  destination outright, so `--discovery=:6681` announces to nobody. A **bare** port is
  a different thing and stays legal for the three bound surfaces: it names no host at
  all, so it takes that surface's default instead of silently replacing it.

  And a table column must be one an answer can depend on. The endpoint table carries
  no default host: `ParseEndpoint` uses one only to fill in the host of a bare port,
  never to decide whether the text parses, so a column for it would have been a value
  that could be wrong for years with nothing to show it. The default hosts are named
  (`SchedulerListenDefaultHost` and its siblings) for the rules that *do* turn on
  them -- `--dashboard`'s loopback test is the one that does.
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

- **Which flags carry text OTHER MACHINES will read is a column of the table.**
  `ParseUtf8Text` rather than `ParseText`, and the rows in it are the ones whose
  value leaves the machine: `--advertise` and `--bind` become the endpoint clients
  dial, `--node-id` and `--raft-peer` a member's identity and the address its peers
  open a socket to, `--cluster-id` rides every discovery beacon, and
  `--cluster-admit`/`--cluster-set` commit their operand through consensus — where
  an entry is applied *after* it is committed, with nobody left to refuse it, so the
  CLI is the last place a person can be told. Since #141 the scheduler refuses a
  registration whose fields are not valid UTF-8, and a value that gets past the CLI
  is refused there on every heartbeat, forever, with the operator's only recovery
  being to rename the thing.

  Three rows are deliberately OUT of the column, and each omission is load-bearing:

  - **`--cluster-forget`.** Its operand *is* the offending id. A check covering it
    would make a member admitted by an older peer impossible to remove, and it would
    count towards quorum forever — the trap
    [#159](https://github.com/LASTRADA-Software/fastcached/issues/159) records.
  - **Every path-valued flag.** On a host that transcodes nothing a legacy filename
    is a perfectly good filename; refusing one would break a working node over a
    rule about a field it is not.
  - **The compiler half of `--toolchain=<fingerprint>=<compiler>`.** Only the
    fingerprint travels, so `ParseToolchain` checks what is before the first `=` and
    nothing after it. Asked by the PARSE rather than where the halves are used,
    which is the difference between a refusal and a trap: `--install-service`
    returns before a toolchain is ever resolved, bakes the command line into a
    registration, and replays it at every boot with nobody watching.

- **A value parser does not know which flag it was reached through, so it must not
  name one.** It is a free function shared by every row that uses it, and a
  hand-written field is one that drifts when a flag is renamed. `ApplyOneOption`
  stamps the row's own `primary` into an error whose `field` the parser left empty;
  a parser with something more specific to say — the node's log-level parser, its
  cluster appliers — keeps saying it. Without the stamp a refusal names nothing,
  which is what the node's own report used to do for an unrecognised argument.
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

## Open work

- **[#208](https://github.com/LASTRADA-Software/fastcached/issues/208)** — the rule
  above covers the addresses this node OPENS. The ones it dials — `--advertise`,
  `--scheduler`, `--upstream`, `--fleet-member` — are never checked for shape at all.
  `--advertise` is the costly one: nothing parses it, so `--advertise=nope` installs,
  registers, heartbeats, is leased out and is never reached, which is word for word
  the failure the emptiness rule beside it was written to prevent. Deciding it needs
  a grammar per flag (`--bind` is a host alone; `--upstream` may be empty;
  `--fleet-member` is a list) and words other than "the surface it configures".

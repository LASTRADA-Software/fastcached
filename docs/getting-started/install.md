# Install

## Packages

The release artifacts include a `.deb`, an `.rpm`, a macOS `.pkg` (also
offered inside a `.dmg`), and a Windows `.msi`. All of them install both
executables — `fastcached` (the daemon) and `fastcache-cc` (the compiler
launcher) — and register the daemon as a service that starts on boot.

### Debian / Ubuntu, Fedora / RHEL

```sh
sudo apt install ./fastcached_<version>_<arch>.deb
sudo dnf install ./fastcached-<version>.<arch>.rpm
```

Installing creates a dedicated `fastcached` system user, enables the unit,
and starts it. The daemon listens on `127.0.0.1:6674` out of the box (see
[Ports](#ports)):

```sh
systemctl status fastcached
journalctl -u fastcached -f
```

Configuration lives in `/etc/fastcached/fastcached.yaml`, which is where
fastcached looks when it is started without `--config`. It ships fully
commented with every setting at its default, and is marked as a package
config file, so local edits survive upgrades.

```sh
sudoedit /etc/fastcached/fastcached.yaml
sudo systemctl restart fastcached
```

`systemctl reload fastcached` applies the reloadable subset — log level,
memory budget, and the authentication settings — without dropping
connections. Changing `bind`, `port`, `listen`, `listen_tls`, any `storage*` key, or
`threads` requires a restart; a reload that touches them is rejected and
the reason is logged.

To change the command line rather than the config file, use a drop-in
instead of editing the shipped unit (which is replaced on upgrade):

```sh
sudo systemctl edit fastcached
```

### Running it as your own user

For a personal compile cache, no root is involved:

```sh
systemctl --user enable --now fastcached
```

The user unit runs on built-in defaults, so that is the whole setup. To
customise it, just drop a config where fastcached already looks — no unit
override needed:

```sh
mkdir -p ~/.config/fastcached
cp /etc/fastcached/fastcached.yaml ~/.config/fastcached/fastcached.yaml
systemctl --user restart fastcached
```

That file is the *only* one any fastcached you start yourself will read:
`/etc/fastcached/fastcached.yaml` describes the system service, whose cache only
the service account can write, so an unprivileged instance passes over it
entirely rather than inheriting settings it cannot act on. Your own copy is
therefore the whole configuration of a personal cache, not an overlay on the
machine-wide one.

State goes to `~/.local/state/fastcached`. Add `loginctl enable-linger
$USER` if you want it running while you are not logged in.

### Windows

Run the MSI and choose what this machine runs: **fastcached** (the cache daemon),
**fastcache-cc** (the compiler launcher), **fastcache-compile-node** (the compile
worker), and the command-line client, which is always installed. The installer
registers the services that follow from the choice:

| Installed | `FastCacheCompileNode` | `FastCached` |
|---|---|---|
| the node (with or without fastcached) | starts with Windows, started now | manual, stopped |
| fastcached without the node | — | starts with Windows, started now (pass `FASTCACHED_START_SERVICE=0` to leave it stopped) |

The node needs no property to be registered: it finds its scheduler from the fleet it forms or
joins (the [fastcache-compile-node page](../tools/fastcache-compile-node.md#macos-and-windows)
has the details). There is no property for the address clients reach the node at: the node
advertises this machine's fully qualified name, resolved at every start, so a renamed machine or a
new VPN address needs no reinstall; an address that must be typed goes under `advertise:` in the
node's configuration file. `FASTCACHE_FIREWALL_ALLOW=10.0.0.0/8` is optional, and limits the
firewall rules both registrations create to that remote range; left out, they admit any address. So is
`FASTCACHE_FLEET_SEED=office-a.vpn.example`, one machine of the fleet for the node to ask when no
discovery beacon reaches it, as across a VPN; it is registered as the node's `--fleet-seed`, and a
node that needs more than one names them under `fleet_seed:` in its configuration file. So is
`FASTCACHE_FLEET_ID`, the one fleet the node may join, pasted as `fastcache-cli node` prints it on a
machine of that fleet (`<cluster-id>@<key>[,<key>...]`); it is registered VERBATIM as the node's
`--fleet-id`. A value the node cannot read **fails the whole transaction** (msiexec exits 1603, and
the log names the action `FastCacheNodeCheckArguments`; `fastcache-compile-node --check-arguments
--fleet-id=<value>` names what is wrong with the value): the node's own parser checks
the node's arguments before anything is remembered or registered, so a refused pin is never
remembered, and a repair or upgrade leaves the existing registration and its remembered pin as they
were rather than starting a node that trusts whichever fleet proves itself first. A malformed
`FASTCACHE_FLEET_SEED` fails it the same way. So does a node registration
that cannot be made, or a node that is not running five seconds after its start: the node is
checked, registered, remembered and started in that order, each step failing the transaction, and a
transaction that fails puts back the registration and the remembered values it found. The package also
pins the node's discovery reply port, `FASTCACHE_DISCOVERY_REPLY_PORT=6682` unless you pass another
(or pass it empty), so its firewall opens UDP 6682 rather than every local UDP port. A node run by
hand leaves that port to the kernel, because two nodes on one machine each need one of their own;
the package installs one node per machine.

A silent install names its features: `msiexec /i fastcached.msi /qn ADDLOCAL=CM_C_Cli,CM_C_Daemon,CM_C_Launcher`.
Changing the selection later (Settings > Apps > Modify, or `msiexec /i fastcached.msi ADDLOCAL=CM_C_Node`
and `REMOVE=CM_C_Node`) re-applies the table. An upgrade applies it too: it keeps the registration
of every feature that stays installed, and removes the registration of a feature it no longer
installs.

**Upgrading from 0.3.0 or earlier is the exception.** Those installers delete both services when
they are removed, and a major upgrade removes the old version first. The new installer registers
`FastCached` and the node again from the table, the node with no property needed.

Every transaction that keeps a service registers it again, and the optional properties are
remembered for it: `FASTCACHE_FIREWALL_ALLOW`, `FASTCACHE_FLEET_SEED` and `FASTCACHE_FLEET_ID` are written to
`HKLM\SOFTWARE\fastcached\Installer` and read back by the next repair or upgrade, so one that
leaves them out registers what was installed. The node's two are written only by a transaction
that keeps the node, once its parser has accepted them. (An advertised endpoint an earlier package of this
installer remembered there is forgotten by the next transaction, and that registration carries none.) A transaction that states a new value replaces the
remembered one. An empty value counts as leaving the property out, so a remembered scope is kept;
to drop it, uninstall (which forgets the values) and install again. An installer older than this
remembering has nothing to read back, so the first upgrade from one registers what it states: with
no property the node is still registered and started, advertising this machine's name with its
firewall rules admitting any address, so pass `FASTCACHE_FIREWALL_ALLOW` (and any other property
you installed with) on that upgrade to keep it.

```powershell
sc.exe query FastCached
sc.exe start FastCached
```

Configuration lives in `C:\ProgramData\fastcached\fastcached.yaml`, which the
service reads at every start. The installer seeds it from the
`etc\fastcached.yaml.default` template beside the executables, but only when
nothing is there yet — so an upgrade never discards your edits. Edit it from an
elevated editor and restart:

```powershell
sc.exe stop FastCached
sc.exe start FastCached
```

Uninstalling removes the services and leaves your configuration in place.

Registering a service also opens the Windows Firewall for whatever it listens on beyond
loopback — nothing, for fastcached's default `127.0.0.1:6674` — and uninstalling, or
deselecting a feature, removes its rules. An upgrade that drops a feature deletes its
registration but leaves its rule group (`fastcached: <service>`) behind, inert, since it
names a program that is no longer installed. What each binary opens is on its own page:
[fastcached](../operations/deployment.md#windows-service) and
[fastcache-compile-node](../tools/fastcache-compile-node.md#macos-and-windows).

### macOS

Open the `.dmg` and run the `.pkg` inside it (the `.pkg` is also published
on its own — the disk image is only a convenience). Both executables land
in `/opt/fastcached/bin`.

The installer asks how you want fastcached to start:

| Choice | Runs as | Starts | Plist |
|---|---|---|---|
| **Start at login** (default) | you | your next login | `~/Library/LaunchAgents/software.lastrada.fastcached.plist` |
| **Start at boot, system-wide** | `_fastcached` | boot | `/Library/LaunchDaemons/software.lastrada.fastcached.plist` |

They are alternatives, not additions. Both would listen on the same
address, and fastcached has no unix-socket endpoint to fall back on, so if
you select the system-wide service it wins and the per-user agent is
skipped. Selecting neither installs the tools without starting anything.

```sh
launchctl print gui/$UID/software.lastrada.fastcached      # per-user
sudo launchctl print system/software.lastrada.fastcached   # system-wide
```

Restart it after editing the config:

```sh
launchctl kickstart -k gui/$UID/software.lastrada.fastcached
```

You can also register the service by hand at any time, which is how you
set one up for a second user account:

```sh
fastcached --install-service --service-scope=user
sudo fastcached --install-service --service-scope=system
```

Note which one takes `sudo`. The user scope installs an agent for *the
invoking account*, so running it under `sudo` would register one for root —
started by nobody's login and invisible to your own `--uninstall-service`.
That combination is refused rather than guessed at.

The system scope runs as the `_fastcached` account, which only the installer
package creates — on a tarball or source install that command tells you so
instead of registering a job that could never start.

**Open a new terminal window after installing.** The package adds
`/opt/fastcached/bin` to the system `PATH` via `/etc/paths.d/fastcached`,
and macOS only reads that when a *login* shell starts — an already-open
terminal never sees it, and neither does fish, which does not read
`/etc/profile`. Both tools are also symlinked into `/usr/local/bin`, which
is on the stock `PATH` everywhere, so in practice they work straight away.

The **system daemon** reads `/opt/fastcached/etc/fastcached.yaml`. Your edits
survive upgrades: only the `fastcached.yaml.default` beside it is replaced,
and the live file is seeded from it just once, when it is absent. The
installer sets it to mode `0640` owned `root:_fastcached`, so the daemon can
read it and other accounts cannot — which is what makes it a safe home for
`requirepass:`.

The **per-user agent** normally does not read that file. The installer sets it
to `0640` owned `root:_fastcached`, so an agent running as you cannot read it
and falls through to per-user defaults, with its cache under
`~/Library/Caches/fastcached`. That is deliberate: the file describes the system
daemon, whose cache lives under the package prefix and is writable only by the
service account, so an agent pointed at it would have nowhere to write.

Give the agent a configuration of its own by putting one where it looks first:

```sh
mkdir -p ~/.config/fastcached
cp /opt/fastcached/etc/fastcached.yaml.default ~/.config/fastcached/fastcached.yaml
launchctl kickstart -k gui/$UID/software.lastrada.fastcached
```

One exception: `storage_path:` in that file will *not* move the agent's cache.
Registering a user agent with no `--config` bakes
`--storage=~/Library/Caches/fastcached/cache` into its `ProgramArguments`, and a
launch argument outranks the file for the life of the registration. Everything
else in the file applies normally. To choose the cache location, name the file
at registration time instead:

```sh
fastcached --install-service --service-scope=user --config=~/my-fastcached.yaml
```

Whichever file you name governs `storage_path` too: the registration passes
no `--storage` when you pass a `--config`, precisely so that editing the
file and restarting the job actually changes where the cache lives.

To remove everything:

```sh
sudo /opt/fastcached/bin/fastcached-uninstall
```

A `.pkg` has no built-in uninstaller — `pkgutil --forget` only drops the
receipt and deletes nothing — so that script ships as part of the package.
It stops and unregisters the launchd jobs, removes `/opt/fastcached`, the
`PATH` entry and the symlinks, deletes the `_fastcached` and
`fastcache-node` accounts, and forgets the receipts. Your own cache and
logs under `~/Library` are left alone.

Apple Silicon only. On an Intel Mac, build from source.

## Building from source

fastcached builds with CMake 3.28 or newer and a C++23 compiler.

!!! warning "A build you installed by hand does not learn that it is out of date"

    Nothing in either executable consults the release feed, so a manual install
    runs the version you built until you rebuild it — and a package install does
    not update itself either.

    That matters more here than the usual "please upgrade" advice, because some
    of this project's fixes are **correctness** fixes for the cache: a stale
    object served for a translation unit whose body changed, a wrong object under
    a right key. An install that predates one of those does not merely lack a
    feature — it can reintroduce a bug that was already found and closed, and
    every layer above it reports success. `ninja`, `cmake` and the linker are all
    perfectly happy with a wrong object.

    So when you follow a `type/bug` fix in
    [the changelog](https://github.com/LASTRADA-Software/fastcached/releases),
    **rebuild and redeploy** — both executables and any service registration, on
    every machine, including the compile nodes. `--version` on either binary
    reports what is running; comparing it against the latest release is a manual
    step today
    ([#181](https://github.com/LASTRADA-Software/fastcached/issues/181)).

    Two things make the staleness outlive a restart rather than expire with it:
    the on-disk cache tier persists, so a wrong object survives reboots until
    something evicts it; and a compile node's fingerprint does not change when the
    binary does, so an old node keeps being matched by the fleet.

## Linux / macOS

```sh
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

The clang-debug preset enables address and undefined-behavior
sanitizers and runs clang-tidy as part of compilation.

## Windows

```sh
cmake --preset cl-debug
cmake --build --preset cl-debug
ctest --preset cl-debug
```

Requires `VCPKG_ROOT` to be set in the environment.

## Building the packages yourself

An ordinary `cmake --install --prefix /usr/local` gives the conventional
layout — `/usr/local/bin/fastcached`, and no service assets, since systemd
does not read units from under a `/usr/local` prefix.

Building a `.deb` or `.rpm` needs the package layout instead: the payload is
rooted at `/` so the units land in `/usr/lib/systemd` and the config in
`/etc`. That is opt-in:

```sh
cmake --preset gcc-release -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON
cmake --build --preset gcc-release --target fastcached fastcache-cc
cd out/build/gcc-release && cpack -G "DEB;RPM"
```

Do not install that build tree directly with `cmake --install` — with the
option ON the binaries deliberately carry a `usr/` prefix of their own, which
only makes sense inside a package.

## Other presets

The repository includes presets for:

- `gcc-debug` — GCC debug build on Linux.
- `clang-coverage` — Linux coverage build. Building its `coverage` target runs the
  whole test suite under instrumentation and writes an HTML report, an lcov export
  and the percentage to `out/build/clang-coverage/coverage/`. Needs `llvm-profdata`
  and `llvm-cov` at the same major version as the `clang` building it.
- `clang-asan-ubsan` — sanitizers without clang-tidy.
- `clang-tsan` — ThreadSanitizer. Run it the way CI does, through the gate that
  refuses to report clean unless the sanitizer is proven to be instrumenting and
  reporting:

  ```sh
  cmake --preset clang-tsan
  cmake --build --preset clang-tsan --target <each binary in the gate's TARGETS table> tsan-canary
  bash scripts/tsan-gate.sh out/build/clang-tsan
  ```

  The binaries to build are the first field of each row of the `TARGETS` table in
  `scripts/tsan-gate.sh`, plus `tsan-canary`. They are not copied here, because a
  copy stops matching when the table grows. The gate stops at the first row whose
  binary was not built, and names it.

- `clangcl-debug` — clang-cl on Windows.

See `CMakePresets.json` for the complete list.

## Running a build

The build produces two executables under the preset's `target/`
directory: `fastcached` and `fastcache-cc`. The daemon runs in the
foreground and listens on `127.0.0.1:6674` by default:

```sh
./fastcached
```

A `--help` flag prints the full configuration surface.

## Ports

fastcached's own port is **6674** — the leading digits of the gravitational
constant, G = 6.674×10⁻¹¹. It is unassigned in the IANA service-name registry,
above the privileged floor (so it needs no `CAP_NET_BIND_SERVICE`), and below
Linux's ephemeral range, so nothing else has a claim on it.

**The port selects no protocol.** fastcached detects the wire format per
connection, so memcached text, memcached binary, redis RESP and the compile-
cache protocol are all served on 6674 — and on any other port you bind. Earlier
releases defaulted to memcached's 11211, which implied a protocol the daemon
never restricted itself to and collided with a real memcached on the same host.

Clients that cannot be re-pointed keep working: bind their port alongside ours
rather than instead of it. In `/etc/fastcached/fastcached.yaml`:

```yaml
listen:
  - 127.0.0.1:6674
  - 127.0.0.1:11211
```

or on the command line, `--listen=127.0.0.1:6674 --listen=127.0.0.1:11211` — the
same `host:port` spelling, because each key is its flag. A TLS endpoint goes under
`listen_tls:` instead.
Both ports then speak every protocol, not just their namesake.

The admin HTTP endpoint (`/metrics`, `/healthz`) is separate and defaults to
port **9259**; it only listens when `--metrics` is given.

### Distributed compilation

One further port, **off unless you ask for it**:

| Port | What | Default |
|------|------|---------|
| **6675** | The fleet scheduler | a convention, not a default: every `fastcache-compile-node` serves the scheduler on its own `--listen-node` rather than on a port of its own, so a scheduling node given `--listen-node=6675` answers there. Not served by `fastcached` |

A worker needs no port of its own. Dispatched compiles arrive on the **same
`--listen-node` surface** that carries the node's cache verbs, so a worker opens one
`0xFC` port in total.

A compile node also serves a **cache tier of its own**, on the same `--listen-node`
port, which defaults to `6674` on loopback — the same address as the daemon's,
deliberately, because that is where `fastcache-cc` already looks. A node that cannot
bind that port **refuses to start** and names the address, whether you typed it or
not: it opens exactly one `0xFC` port, and without it would register an address
nothing answers. So do not run a node and `fastcached` on one machine — the node
answers every verb the daemon does — or give one of them a port of its own.
A node running consensus additionally binds `--listen-raft` — every connection on that
port proves the member's own identity key before a message is read — and,
with discovery on, a UDP `--discovery` port plus a per-node answering port; none has
a conventional number, and all are off unless configured.

For who dials whom on each of these — and what a machine needs to accept rather
than merely bind — see
[Cluster communication](../operations/cluster-communication.md#what-to-open-on-a-firewall).

**One port does not mean one policy.** Cache and compile share the listener and are
still governed separately: the cache verbs answer this machine alone whatever the
socket is bound to, while a dispatched compile additionally needs a lease the
scheduler signed for this worker's advertised endpoint. Which caller is admitted to
which verb is a property of the verb, never of the port it arrived on — so widening
`--listen-node` to reach workers does not widen who may read the node's cache.

The endpoint a worker advertises is not an IANA request and is not a client-side
default: the scheduler hands a client that endpoint explicitly, so it is only ever
what an operator configured — and it must be an address other machines can dial,
which the node refuses at startup if it is not.

See [Distributed compilation](distributed-compilation.md).

## Building the packages

```sh
cmake --preset clang-release
cmake --build --preset clang-release
cd out/build/clang-release && cpack -G "DEB;RPM;TGZ"    # Windows: cpack -G WIX
```

The Windows MSI additionally needs the WiX Toolset (v4 or v5) installed.

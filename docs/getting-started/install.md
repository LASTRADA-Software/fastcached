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
connections. Changing `bind`, `port`, `listeners`, any `storage*` key, or
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

Run the MSI. It installs both executables and registers `fastcached` as an
auto-start Windows service, then starts it — clear the checkbox in the
installer to register the service without starting it for now.

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

Uninstalling removes the service and leaves your configuration in place.

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
- `clang-coverage` — Linux coverage build with an HTML report.
- `clang-asan-ubsan` — sanitizers without clang-tidy.
- `clang-tsan` — thread sanitizer.
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
listeners:
  - address: 127.0.0.1
    port: 6674
  - address: 127.0.0.1
    port: 11211
```

or on the command line, `--listen=127.0.0.1:6674 --listen=127.0.0.1:11211`.
Both ports then speak every protocol, not just their namesake.

The admin HTTP endpoint (`/metrics`, `/healthz`) is separate and defaults to
port **9259**; it only listens when `--metrics` is given.

### Distributed compilation

Two further ports, both **off unless you ask for them**:

| Port | What | Default |
|------|------|---------|
| **6675** | The fleet scheduler | off; enable with `fastcache-compile-node --listen-scheduler`. Not served by `fastcached` |
| **6676** | A compile worker's own port | the worker's `--port` |

The dispatch endpoint is separate from the cache **on purpose**. The cache may
reasonably be reachable across a build LAN; the surface that causes a compiler to
*run* on another machine should be something you switch on and firewall
deliberately. A dispatch request arriving on a cache-only listener is refused
with a typed error rather than served.

6676 is not an IANA request and is not a client-side default: the scheduler hands
a client the worker's endpoint explicitly, so it is only ever what an operator
configured.

See [Distributed compilation](distributed-compilation.md).

## Building the packages

```sh
cmake --preset clang-release
cmake --build --preset clang-release
cd out/build/clang-release && cpack -G "DEB;RPM;TGZ"    # Windows: cpack -G WIX
```

The Windows MSI additionally needs the WiX Toolset (v4 or v5) installed.

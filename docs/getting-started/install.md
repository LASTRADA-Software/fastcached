# Install

## Packages

The release artifacts include a `.deb`, an `.rpm`, and a Windows `.msi`.
All of them install both executables — `fastcached` (the daemon) and
`fastcache-cc` (the compiler launcher) — and register the daemon as a
service that starts on boot.

### Debian / Ubuntu, Fedora / RHEL

```sh
sudo apt install ./fastcached_<version>_<arch>.deb
sudo dnf install ./fastcached-<version>.<arch>.rpm
```

Installing creates a dedicated `fastcached` system user, enables the unit,
and starts it. The daemon listens on `127.0.0.1:11211` out of the box:

```sh
systemctl status fastcached
journalctl -u fastcached -f
```

Configuration lives in `/etc/fastcached/fastcached.yaml`. It ships fully
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
mkdir -p ~/.config/fastcached
cp /etc/fastcached/fastcached.yaml ~/.config/fastcached/fastcached.yaml
systemctl --user enable --now fastcached
```

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

The service reads `fastcached.yaml` from the installation directory.
Uninstalling removes the service.

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
foreground and listens on `127.0.0.1:11211` by default:

```sh
./fastcached
```

A `--help` flag prints the full configuration surface.

## Building the packages

```sh
cmake --preset clang-release
cmake --build --preset clang-release
cd out/build/clang-release && cpack -G "DEB;RPM;TGZ"    # Windows: cpack -G WIX
```

The Windows MSI additionally needs the WiX Toolset (v4 or v5) installed.

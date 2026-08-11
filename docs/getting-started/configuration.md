# Configuration

fastcached takes its settings from a YAML file, from command-line flags, or
from both. `fastcached --help` prints the complete, always-current flag list;
this page covers where the file lives, which source wins, and what happens when
a setting cannot be applied.

## Where the config file lives

You do not have to tell fastcached where its configuration is. Started with no
`--config`, it reads the **first of these that exists and it can read** — which
is exactly where each installer puts it:

| Platform | Per-user | Machine-wide |
|---|---|---|
| Linux | `$XDG_CONFIG_HOME/fastcached/fastcached.yaml`, else `~/.config/fastcached/fastcached.yaml` | `/etc/fastcached/fastcached.yaml` |
| macOS | `$XDG_CONFIG_HOME/fastcached/fastcached.yaml`, else `~/.config/fastcached/fastcached.yaml` | `/opt/fastcached/etc/fastcached.yaml` |
| Windows | `%APPDATA%\fastcached\fastcached.yaml` | `%ProgramData%\fastcached\fastcached.yaml` |

The per-user file wins, so you can shadow a machine-wide configuration without
touching it and without root. The startup banner reports which file was chosen:

```
[INFO] fastcached 0.0.1 starting; bind=127.0.0.1:6674 ... config=/etc/fastcached/fastcached.yaml ...
```

`config=<none>` there means no file was found and the built-in defaults are in
effect — which is a perfectly good way to run fastcached.

!!! note "Named files are strict, discovered ones are not"

    `--config=<path>` overrides the search entirely, and the file must exist —
    you asserted it was there, so a typo is an error rather than a silent
    fallback to different settings.

    A *default* location that does not exist, or that this account may not read,
    is skipped and the next one tried. That is what lets a per-user daemon start
    normally alongside a machine-wide config it has no access to. A default file
    that exists and *is* readable but does not parse is still a startup error:
    at that point it is plainly meant to be used.

## What the file looks like

Every installed copy ships fully commented, with each setting shown at its
built-in default, so an untouched file behaves exactly like running fastcached
with no flags. Uncomment only what you want to change:

```yaml
# bind: 127.0.0.1
# port: 6674
max_memory: 4g
storage_path: /var/lib/fastcached/cache
```

Path-valued settings (`storage_path`, `tls_cert`, `tls_key`) understand `$VAR`
and `${VAR}` environment references; write `$$` for a literal dollar. A
reference to a variable that is not set is an error rather than an empty
string, so a typo fails loudly instead of quietly relocating the cache. Windows
`%VAR%` is *not* expanded — a bare `%` is valid in a path.

## Precedence

From strongest to weakest:

1. **Command-line flags** — `--port=7000` beats everything.
2. **The config file** — whether named with `--config` or discovered.
3. **Environment** — only `FASTCACHED_METRICS_PORT`, so a container's daemon
   and its `--healthcheck` probe can agree on a port with one `-e`.
4. **Built-in defaults.**

Because a flag outranks the file for as long as it is on the command line, a
service registered with a setting baked into its launch arguments will ignore
that key in the file forever. This is why `--install-service` registers only
what you typed, and why editing the config file is the way to change a running
installation.

## Reloading

`systemctl reload fastcached` (or `SIGHUP` directly, or the service manager's
`PARAMCHANGE` on Windows) re-reads the file in place, without dropping
connections. Only part of the configuration is reloadable:

--8<-- "reload-matrix.md"

The right-hand column is live-wired at startup — listeners are bound and the
storage backend is constructed once — so a reload that changes any of it is
rejected *in full*, the previous configuration is kept, and the reason is
logged. A reload therefore either applies everything or nothing.

## Secrets

Keep `requirepass` in the config file, not on the command line: launch
arguments are world-readable through the process table and through the service
registration. `--install-service` refuses `--requirepass` outright for that
reason. Give the file mode `0640` and make it readable by the account the
service runs as — which is what the macOS installer does to
`/opt/fastcached/etc/fastcached.yaml`.

## Editing the installed file

=== "Linux"

    ```sh
    sudoedit /etc/fastcached/fastcached.yaml
    sudo systemctl reload fastcached     # or restart, for the non-reloadable keys
    ```

    The file is a package config file (dpkg conffile / rpm
    `%config(noreplace)`), so your edits survive upgrades.

=== "macOS"

    ```sh
    sudo vi /opt/fastcached/etc/fastcached.yaml
    sudo launchctl kickstart -k system/software.lastrada.fastcached
    ```

    Only the `fastcached.yaml.default` beside it is package payload; the live
    file is seeded from it once, when absent, and never replaced.

=== "Windows"

    ```powershell
    notepad C:\ProgramData\fastcached\fastcached.yaml   # elevated
    sc.exe stop FastCached; sc.exe start FastCached
    ```

    The MSI installs a `fastcached.yaml.default` template under the install
    directory and seeds `%ProgramData%` from it only when nothing is there yet,
    so a later upgrade does not discard your edits. The live file is deliberately
    not part of the installer payload, and uninstalling leaves it behind.

## Per-user daemons

A personal cache needs no root and no packaged file at all — drop one in your
own config directory and fastcached will find it:

```sh
mkdir -p ~/.config/fastcached
cp /etc/fastcached/fastcached.yaml ~/.config/fastcached/fastcached.yaml
systemctl --user restart fastcached
```

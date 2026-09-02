# Deployment

fastcached ships the pieces needed to run it beyond a trusted LAN:
authentication, TLS, a Prometheus metrics endpoint, a health probe, and a
container image. This page shows how they fit together.

## systemd

The `.deb` and `.rpm` packages install a unit that runs the daemon as a
dedicated `fastcached` system user and starts it on boot. See
[Install](../getting-started/install.md) for the package-level workflow;
this section covers what the unit does and how to adapt it.

The unit runs the daemon in the **foreground** under `Type=simple` and does
not pass `--daemon`. That is deliberate: the POSIX daemonize path
double-forks and redirects stdout/stderr to `/dev/null`, so journald would
capture nothing, and its pidfile is written by the grandchild only after
both parents exit, which races `Type=forking` with `PIDFile=`. Running in
the foreground avoids both and gives journald the daemon's stderr directly.

```sh
journalctl -u fastcached -f
```

Leave `log_timestamps` off — journald timestamps every line already, and off
is the default on Linux.

### Reloading

`systemctl reload fastcached` sends `SIGHUP`, which re-reads the config file in
use — `/etc/fastcached/fastcached.yaml` for the packaged unit. Only part of the
configuration is reloadable:

--8<-- "reload-matrix.md"

The right-hand column is live-wired at startup — listeners are bound and the
storage backend is constructed once — so a reload that changes any of it is
rejected *in full*, the previous configuration is kept, and the reason is
logged. Reload therefore either applies everything or nothing.

### Hardening

fastcached never drops privileges on its own, so every restriction comes
from the unit: `User=`/`Group=`, `StateDirectory=`, and the usual
`Protect*`/`Restrict*` set. Check it with:

```sh
systemd-analyze security fastcached.service
```

Two consequences worth knowing. `ProtectSystem=strict` and
`ProtectHome=yes` mean TLS certificates must live somewhere the unit can
still read — `/etc/fastcached/` is the natural place. And binding a port
below 1024 needs capabilities the unit drops by default:

```ini
# systemctl edit fastcached
[Service]
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
```

### Persistent storage

`StateDirectory=fastcached` provides `/var/lib/fastcached`, owned by the
service user. Point `storage_path` at it to enable the on-disk tier:

```yaml
storage_path: /var/lib/fastcached/cache
```

The path's *shape* selects the layout: a path with no file extension (as
above) becomes a sharded directory, while one with an extension
(`cache.cow`) is a single file.

## launchd

On macOS the `.pkg` registers a launchd job. The same registration is
available from the command line on any installation, and `--service-scope`
picks the domain:

```sh
fastcached --install-service --service-scope=user            # LaunchAgent
sudo fastcached --install-service --service-scope=system      # LaunchDaemon
```

The system scope needs the `_fastcached` service account, which the `.pkg`
creates; installing from a tarball or a source build fails with a message
saying so rather than registering a job launchd cannot spawn.

The `.pkg` creates a second account, `fastcache-node`, for
`fastcache-compile-node`. It is deliberately not shared with `_fastcached`: a
worker runs a compiler on input that arrived over the network while `fastcached`
owns the cache storage, so one account would let a compromised compile rewrite
every cached object. It comes from the package's Runtime component, so it is
present whichever launchd choice you made here, and the uninstaller removes both.

Every flag you pass **on the command line** alongside `--install-service` is
baked into the job's `ProgramArguments`, exactly as on Windows. Values read
from a `--config` file are not: they stay in the file, so editing it and
restarting the job keeps working.

```sh
sudo fastcached --install-service --service-scope=system \
    --config=/opt/fastcached/etc/fastcached.yaml --threads=4
```

Prefer the config file for anything you might want to change later. A flag
on this command line outranks the same key in YAML for the life of the
registration, so `--storage=…` here would quietly pin the cache location and
make a later `storage_path:` edit a no-op.

`--requirepass` is the one flag that cannot be baked in, and the install is
refused rather than silently dropping it: `ProgramArguments` lands in a
world-readable plist, so the secret belongs in the config file passed with
`--config`. Passing both is refused too — nothing can tell from the command
line whether that file actually carries `requirepass:`, so accepting the
combination would be the silent drop under another name.

The package sets the system daemon's config to mode `0640`, owned
`root:_fastcached`, so it is readable by the account the daemon drops to and
by nobody else — which is the whole reason to keep the secret there rather
than in the plist. If you point the daemon at a config of your own, the
install checks that `_fastcached` can read it and tells you how to fix it if
not; without that check an unreadable config shows up only as a job that
exits at every start, with the `EACCES` visible nowhere.

`--service-scope=user` must **not** run under `sudo`: the agent belongs to
the invoking account, and sudo would resolve that to root — registering an
agent under `/var/root` that your own login never starts and your own
`--uninstall-service` cannot see. The install refuses rather than guess.

| | `user` | `system` |
|---|---|---|
| Plist | `~/Library/LaunchAgents/` | `/Library/LaunchDaemons/` |
| Runs as | the invoking user | `_fastcached` |
| Starts at | login | boot |
| Privileges | none | root to install |
| `KeepAlive` | on crash only | always |

The generated plist deliberately does **not** pass `--daemon`. launchd, like
systemd, supervises the process it started; a job that double-forks is
reaped immediately as `exited` and the service silently never runs.

The two scopes are alternatives — both bind the same address, and there is
no unix-socket endpoint to separate them. A per-user agent uses
`KeepAlive={Crashed:true}` rather than `true` for that reason: an agent that
loses the race for the port exits cleanly, and restart-always would turn
that into a permanent ten-second crash loop instead of one log line.

Status, restart, logs:

```sh
launchctl print gui/$UID/software.lastrada.fastcached
launchctl kickstart -k gui/$UID/software.lastrada.fastcached
tail -f ~/Library/Logs/fastcached/software.lastrada.fastcached.err.log
```

The system daemon logs to `/opt/fastcached/var/log/<label>/` instead — a
directory per job, because that directory is owned by the account the job runs
as and a machine may run both `fastcached` and `fastcache-compile-node`
system-wide:

```sh
tail -f /opt/fastcached/var/log/software.lastrada.fastcached/software.lastrada.fastcached.err.log
```

### Timestamps

Those are plain files, and **launchd writes nothing into them itself** — unlike
journald, which stamps every line it captures, and unlike the Windows event log,
where the time is part of the record. A log with no times is close to useless the
one time it is read.

So `log_timestamps` **defaults to on when running on macOS**, in both binaries and
in every deployment, not only under launchd: a terminal run stamps too. Turn it off
with `log_timestamps: false` in the configuration file, or with the
`--no-log-timestamps` flag, which exists for this — a bare `--log-timestamps` can
only ask for the default that is already in force.

A configuration file outranks the platform default in **both** directions, so
`log_timestamps: false` on macOS is obeyed and `log_timestamps: true` on Linux is
too. The command line outranks the file, as everywhere else.

There is no reload equivalent to `systemctl reload`: send `SIGHUP` to the
pid `launchctl print` reports, or kickstart the job.

To remove it: `fastcached --uninstall-service --service-scope=<scope>`, or
`sudo /opt/fastcached/bin/fastcached-uninstall` to remove the whole install.

## Windows service

The MSI registers `fastcached` as an auto-start service and seeds
`C:\ProgramData\fastcached\fastcached.yaml` from the template it ships, unless
a config is already there. The registration passes **no** `--config`: the
service resolves that path itself at every start, so editing the file is all it
takes to reconfigure it, and a machine without one still starts on the built-in
defaults rather than failing.

The same registration is available from the command line on any installation:

```powershell
fastcached.exe --install-service
sc.exe start FastCached
fastcached.exe --uninstall-service
```

Pass `--config=C:\path\to\fastcached.yaml` only to point the service at a file
*other* than the default location.

### What it runs as

The service logs on as the **virtual account** `NT SERVICE\FastCached`. The SCM
derives that from the service name and manages it itself, so there is no account
to create and no password to keep. Told nothing, `CreateService` would use
LocalSystem, which has unrestricted access to every local resource and is a
member of the local Administrators group — more than a cache daemon listening on
a socket has any use for.

It can still read `C:\ProgramData\fastcached\fastcached.yaml`, because that
directory grants `BUILTIN\Users` read and execute. It deliberately cannot *write*
there: a service that cannot rewrite its own configuration cannot be talked into
loading a different one.

**If you set `storage_path`, the account needs access to it.** `--install-service`
hands over whatever `storage_path` is configured at the time it runs, so seeding
the config first and installing second needs nothing extra.

One exception: a `storage_path` that names a **single cache file** — an existing
file, or a path with a file extension such as `D:\fastcached\cache.cow` — is only
created by the installer if it is already there. Creating `cache.cow` as a
*directory* would make the very next start treat it as a directory of shards and
fan out inside it, so the installer refuses and says so. An upgrade therefore needs
nothing extra (the file exists, and is handed over); a first install against a
not-yet-created cache file prints that refusal as a `warning:` on the install line
and needs the `icacls` command below, run against the directory that will hold it.

If you add or move `storage_path` afterwards, grant it from an elevated prompt:

```powershell
icacls "D:\fastcached\cache" /grant "NT SERVICE\FastCached":(OI)(CI)F
```

Note that re-running `--install-service` does **not** repair it: registering a
service that already exists is refused before the handover happens, so the grant
never runs. Either use `icacls`, or `--uninstall-service` first — which stops the
service, so `icacls` is the less disruptive of the two.

A daemon that cannot open its storage says so at startup and prints this command
with your own paths and service name filled in; without `storage_path` it is
memory-only and needs no directory at all. The advice appears only when the
daemon is *running as the service* (`--daemon`) — a foreground run is not the
virtual account, so naming it there would send you after an identity that is not
the one being refused.

Renaming the service with `--service-name` renames the account with it, since the
SCM derives one from the other.

**A storage path belongs to one daemon.** The store is claimed exclusively while
it is open, so a second daemon started against the same `storage_path` refuses
rather than sharing it:

```
failed to open storage 'D:\fastcached\cache': StorageError(code=InUse system=0 context=FilePageStore::Open)
Another process already has this store open. A storage path belongs to one daemon:
stop the other one, or give this daemon a path of its own.
```

Running two daemons on one machine is fine — give each its own path. Nothing is
written to the store to enforce this, so its files stay readable by any build.

`InUse` is one of four codes an open failure can carry, and they call for
different things. `UnsupportedFormatVersion` means a healthy store of another
vintage — [convert it](upgrading-a-store.md), never delete it. `Corrupt` means the
bytes really are damaged: [When a store reports `Corrupt`](corrupt-store.md) says
whether the process starts, what deleting costs, and what to keep first.

`--install-service` records the flags it was given on the command line into
the service's command line (values from a `--config` file stay in the file)
and makes path arguments absolute — a service starts with its
working directory set to `C:\Windows\System32`, so a relative path captured
at install time would resolve elsewhere at boot. It creates the service
already set to auto-start but leaves it stopped, so the first start is
explicit.

## Container

A multi-stage [`Dockerfile`](../../Dockerfile) builds a Release binary with TLS
enabled and copies it onto a slim Debian runtime:

```sh
docker build -t fastcached .
docker run --rm -p 6674:6674 -p 9259:9259 fastcached \
    --bind=0.0.0.0 --metrics --metrics-bind=0.0.0.0 --requirepass=secret
```

The image's default `CMD` binds `0.0.0.0` (so the cache is reachable from
outside the container) and enables the metrics endpoint. **Binding `0.0.0.0`
without `--requirepass` exposes the cache to anyone who can reach the port** —
that is exactly the deployment this bundle exists to secure, so pair it with
`--requirepass` and, across untrusted networks, `--tls`.

### Health check

The image's `HEALTHCHECK` runs `fastcached --healthcheck`, which probes
`http://127.0.0.1:<metrics-port>/healthz` and exits 0 (healthy) or 1. It is
self-contained — no `curl`/`wget` in the image — but requires the daemon to run
with `--metrics`.

## Authentication

Set a shared secret to require clients to authenticate before any data command:

```sh
fastcached --requirepass=secret            # redis AUTH + memcached binary SASL PLAIN
fastcached --requirepass=secret --auth-username=alice
```

- **Redis:** `AUTH secret` or `AUTH alice secret` → `+OK`; data commands before
  auth get `-NOAUTH`.
- **memcached binary:** SASL `PLAIN` against the same secret.
- **memcached text:** has no auth handshake, so it rejects data commands while a
  secret is configured — use the binary or RESP protocol with auth.

The secret is compared in constant time and never logged.

## TLS

TLS is compiled in with `-DFASTCACHED_ENABLE_TLS=ON` (the image enables it) and
turned on at runtime:

```sh
fastcached --tls --tls-cert=/etc/fastcached/server.crt --tls-key=/etc/fastcached/server.key
# clients:
redis-cli --tls --insecure -a secret ping
```

`--tls` on a build without TLS support exits with a clear error. Client
certificate (mutual TLS) auth is not yet implemented.

## Metrics

```sh
fastcached --metrics --metrics-bind=0.0.0.0 --metrics-port=9259
curl http://host:9259/metrics    # Prometheus text exposition
curl http://host:9259/healthz    # 200 OK
```

## Kubernetes

A minimal Deployment with probes and a Prometheus scrape annotation:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: fastcached
spec:
  replicas: 1
  selector:
    matchLabels: { app: fastcached }
  template:
    metadata:
      labels: { app: fastcached }
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "9259"
        prometheus.io/path: "/metrics"
    spec:
      containers:
        - name: fastcached
          image: fastcached:latest
          args: ["--bind=0.0.0.0", "--metrics", "--metrics-bind=0.0.0.0", "--requirepass=$(CACHE_SECRET)"]
          env:
            - name: CACHE_SECRET
              valueFrom: { secretKeyRef: { name: fastcached, key: requirepass } }
          ports:
            - { containerPort: 6674, name: cache }
            - { containerPort: 9259, name: metrics }
          livenessProbe:
            httpGet: { path: /healthz, port: metrics }
            periodSeconds: 30
          readinessProbe:
            httpGet: { path: /healthz, port: metrics }
            periodSeconds: 10
```

## sccache

The original use case still works — point sccache at the cache port:

```sh
export SCCACHE_REDIS=redis://:secret@host:6674   # with auth
export SCCACHE_MEMCACHED=tcp://host:6674         # binary protocol, no auth
```

--8<-- "sccache-backend-caveat.md"

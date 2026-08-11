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

Leave `log_timestamps` off — journald timestamps every line already.

### Reloading

`systemctl reload fastcached` sends `SIGHUP`, which re-reads
`/etc/fastcached/fastcached.yaml`. Only part of the configuration is
reloadable:

| Reloadable | Requires a restart |
|---|---|
| `log_level`, `max_memory`, `requirepass`, `auth_username`, `notify_keyspace_events` | `bind`, `port`, `listeners`, `storage_path`, `storage_shards`, `storage_durability`, `storage_max_value`, `threads` |

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

The system daemon logs to `/opt/fastcached/var/log/` instead.

There is no reload equivalent to `systemctl reload`: send `SIGHUP` to the
pid `launchctl print` reports, or kickstart the job.

To remove it: `fastcached --uninstall-service --service-scope=<scope>`, or
`sudo /opt/fastcached/bin/fastcached-uninstall` to remove the whole install.

## Windows service

The MSI registers `fastcached` as an auto-start service. The same
registration is available from the command line on any installation:

```powershell
fastcached.exe --install-service --config=C:\path\to\fastcached.yaml
sc.exe start FastCached
fastcached.exe --uninstall-service
```

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

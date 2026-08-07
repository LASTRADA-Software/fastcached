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

## Windows service

The MSI registers `fastcached` as an auto-start service. The same
registration is available from the command line on any installation:

```powershell
fastcached.exe --install-service --config=C:\path\to\fastcached.yaml
sc.exe start FastCached
fastcached.exe --uninstall-service
```

`--install-service` records the flags it was given into the service's
command line and makes path arguments absolute — a service starts with its
working directory set to `C:\Windows\System32`, so a relative path captured
at install time would resolve elsewhere at boot. It creates the service
already set to auto-start but leaves it stopped, so the first start is
explicit.

## Container

A multi-stage [`Dockerfile`](../../Dockerfile) builds a Release binary with TLS
enabled and copies it onto a slim Debian runtime:

```sh
docker build -t fastcached .
docker run --rm -p 11211:11211 -p 9259:9259 fastcached \
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
          args: ["--bind=0.0.0.0", "--port=11211", "--metrics", "--metrics-bind=0.0.0.0", "--requirepass=$(CACHE_SECRET)"]
          env:
            - name: CACHE_SECRET
              valueFrom: { secretKeyRef: { name: fastcached, key: requirepass } }
          ports:
            - { containerPort: 11211, name: cache }
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
export SCCACHE_REDIS=redis://:secret@host:11211   # with auth
export SCCACHE_MEMCACHED=tcp://host:11211         # binary protocol, no auth
```

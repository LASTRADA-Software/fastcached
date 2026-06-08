# Deployment

fastcached ships the pieces needed to run it beyond a trusted LAN:
authentication, TLS, a Prometheus metrics endpoint, a health probe, and a
container image. This page shows how they fit together.

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

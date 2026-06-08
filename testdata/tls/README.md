# Throwaway TLS test fixtures

`server.crt` / `server.key` are a **self-signed** certificate and private key for
`CN=localhost`, used only by the unit tests and the TLS smoke test, and handy for
trying `--tls` locally:

```sh
fastcached --tls --tls-cert testdata/tls/server.crt --tls-key testdata/tls/server.key
# then, e.g.:  redis-cli --tls --insecure ping
```

They carry no secret value — the key is committed on purpose so tests need no
generation step. **Never use them in production.** Generate real per-host
material instead, e.g.:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.crt \
    -days 365 -subj "/CN=your-host"
```

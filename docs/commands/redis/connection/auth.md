# AUTH

**Protocols:** Redis RESP2 · **Status:** Supported (when `--requirepass` is set)

fastcached authenticates clients against a single shared secret configured
with `--requirepass=<secret>` (or the `requirepass` YAML key). When no secret
is configured, authentication is disabled: every command is served without a
credential check, and `AUTH` replies with the redis-compatible
`-ERR Client sent AUTH, but no password is set`.

When a secret **is** configured, every command except the handshake verbs
(`AUTH`, `HELLO`, `QUIT`) replies `-NOAUTH Authentication required.` until a
successful `AUTH`. The optional `--auth-username=<name>` flag (default
`default`) sets the username expected by the two-argument form.

## Synopsis

```text
AUTH password
AUTH username password
```

## Response

```text
+OK                                                          # accepted
-WRONGPASS invalid username-password pair or user is disabled.  # rejected
-ERR Client sent AUTH, but no password is set               # auth disabled
```

## Notes

- The secret is compared in constant time, so a wrong guess cannot be
  recovered byte-by-byte through response timing.
- The memcached binary protocol authenticates the same credential via SASL
  PLAIN; the memcached text protocol has no auth handshake and therefore
  rejects data commands when a secret is configured.
- Authentication protects against unauthorized access but not eavesdropping —
  the wire is still plaintext unless TLS is enabled.

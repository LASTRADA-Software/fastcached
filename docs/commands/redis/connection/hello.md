# HELLO

**Protocols:** Redis RESP2

Negotiates protocol version. fastcached accepts version 2 (RESP2) and
rejects version 3 with `-NOPROTO`.

## Synopsis

```text
HELLO [protocol-version]
```

## Responses

- For `HELLO` or `HELLO 2`: an array describing the server (server
  name, version, proto = 2, id, mode, role, modules)
- For `HELLO 3` (or any version other than 2):
  `-NOPROTO sorry, RESP3 not supported`

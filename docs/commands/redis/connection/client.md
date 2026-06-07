# CLIENT

**Protocols:** Redis RESP2

A compatibility stub for the handshake probes Redis client libraries
send during connection setup. fastcached holds no per-client state, so
each sub-command is acknowledged benignly rather than erroring (an
error here would abort the library's handshake).

## Synopsis

```text
CLIENT SETNAME <name>
CLIENT SETINFO <attr> <value>
CLIENT ID
CLIENT GETNAME
```

## Responses

| Sub-command   | Reply |
|---------------|-------|
| `GETNAME`     | Empty bulk string (`$0\r\n\r\n`) |
| `ID`          | Integer `:1\r\n` (a constant) |
| anything else | `+OK\r\n` |

No connection registry is kept: the returned id is always `1` and the
name passed to `SETNAME` is never stored.

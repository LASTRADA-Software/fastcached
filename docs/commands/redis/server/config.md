# CONFIG

**Protocols:** Redis RESP2

A compatibility stub. Redis client libraries probe parameters such as
`maxmemory` and `save` on connect; fastcached exposes no runtime
tunables, so it answers these probes without holding any configuration
state.

## Synopsis

```text
CONFIG GET <param>...
CONFIG SET <param> <value>
CONFIG RESETSTAT
CONFIG REWRITE
```

## Responses

- `CONFIG GET <param>...` — an array of name/value pairs, one per
  requested parameter, with every value reported as the string `0`.
  `CONFIG GET` with no parameter returns an empty array (`*0`).
- `CONFIG SET`, `CONFIG RESETSTAT`, `CONFIG REWRITE`, and any other
  sub-command — `+OK\r\n`.

Nothing is actually configured; the reply shape exists only to satisfy
client libraries that parse the response into a map.

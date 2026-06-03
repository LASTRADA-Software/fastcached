# SET

**Protocols:** Redis RESP2

Stores `value` at `key`. Supports `EX` / `PX` for TTL and `NX` / `XX`
for store-if-absent / store-if-present.

## Synopsis

```text
SET key value [EX seconds | PX milliseconds] [NX | XX]
```

## Response

- `+OK` on success
- Nil if `NX` / `XX` precondition failed

## Example

```sh
redis-cli> SET k hello EX 60
OK
redis-cli> SET k overwrite NX
(nil)
```

## See also

- [SETEX](setex.md), [PSETEX](psetex.md)

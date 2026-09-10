# HELLO

**Protocols:** Redis RESP2 / RESP3

Negotiates protocol version. fastcached accepts version 2 and version 3.
A connection starts in RESP2; `HELLO 3` upgrades it, and every later
reply on that connection uses RESP3 types.

## Synopsis

```text
HELLO [protocol-version]
```

## Responses

- For `HELLO` or `HELLO 2`: a flat **array** describing the server
  (server name, version, `proto` = 2, id, mode, role)
- For `HELLO 3`: the same fields as a RESP3 **map** (`%6`), with
  `proto` = 3, and the connection is upgraded
- For any other version: `-NOPROTO unsupported protocol version`

## Notes

- The upgrade is per connection and survives until it closes or `RESET`.
- `RESET` returns the connection to RESP2 along with clearing any
  transaction, watch set and subscription.
- What RESP3 changes on the wire — maps, sets, doubles, booleans, push
  and attribute frames — is in [Redis RESP](../../../protocols/redis-resp.md).

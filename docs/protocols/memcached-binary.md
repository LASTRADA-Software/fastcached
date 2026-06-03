# memcached (binary)

A length-prefixed binary protocol. Each request and response is a
24-byte header followed by optional extras, key, and value sections.

## Request header (24 bytes)

| Offset | Size | Field          |
|-------:|-----:|----------------|
| 0      | 1    | Magic = `0x80` |
| 1      | 1    | Opcode         |
| 2      | 2    | Key length     |
| 4      | 1    | Extras length  |
| 5      | 1    | Data type (reserved) |
| 6      | 2    | vBucket id (unused) |
| 8      | 4    | Total body length |
| 12     | 4    | Opaque         |
| 16     | 8    | CAS            |

## Response header (24 bytes)

Same layout as the request header, except:

- Magic is `0x81`.
- Bytes 6–7 hold the status code (see
  [Status codes](status-codes.md)).

## Quiet variants

Opcodes whose name ends in `Q` (`SetQ`, `GetQ`, `IncrementQ`, etc.)
suppress the response on the success path. On error they still emit
the error response so the client can detect failures.

For commands that emit no response on the success path, the standard
pipelining technique is to send a `NoOp (0x0a)` after the quiet
operation — the `NoOp` reply confirms the connection is alive and all
preceding writes have been processed.

## Opcodes

See [Binary opcodes](binary-opcodes.md) for the hex table.

## Status codes

See [Status codes](status-codes.md).

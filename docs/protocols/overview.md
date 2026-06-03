# Protocols overview

fastcached speaks four wire protocols off the same storage engine:

| Protocol             | Reference                                  | Status      |
|----------------------|--------------------------------------------|-------------|
| memcached (text)     | [memcached protocol.txt][mc]               | Full        |
| memcached (binary)   | [memcached protocol_binary.xml][mcb]       | Full        |
| memcached (meta)     | [memcached protocol.txt §meta][mc]         | Full        |
| Redis (RESP2)        | [Redis Serialization Protocol][resp]       | Key-value subset |

[mc]: https://github.com/memcached/memcached/blob/master/doc/protocol.txt
[mcb]: https://github.com/memcached/memcached/blob/master/doc/protocol-binary.xml
[resp]: https://redis.io/docs/latest/develop/reference/protocol-spec/

Each connection is routed to one handler based on its first bytes —
see [Autodetection](autodetect.md) for the exact rules. A given client
speaks one protocol; mixing on a single connection is not supported.

## At a glance

Each row is a logical capability area; cells show how complete each
protocol is in fastcached today.

| Capability area               | memcached text | memcached binary | memcached meta | Redis RESP2 |
|-------------------------------|:---:|:---:|:---:|:---:|
| Storage (set/add/replace/append/prepend) | Full | Full | Full | Partial (SET only) |
| CAS                           | Full | Full | Full | Not applicable |
| Retrieval (get / gets)        | Full | Full | Full | Full |
| TTL refresh (touch / GAT)     | Full | Full | Full | Not supported |
| Arithmetic (incr / decr)      | Full | Full | Full | Not supported |
| Deletion                      | Full | Full | Full | Full |
| Flush                         | Full | Full | n/a  | Full |
| Stats                         | Sub-commands | Basic | Via `me` | INFO only |
| Slabs / LRU tuning            | Synthetic stub | n/a | n/a | n/a |
| `watch` event streaming       | Not supported | n/a | n/a | n/a |
| Authentication                | n/a | SASL rejected | n/a | AUTH rejected |
| Multiple databases            | n/a | n/a | n/a | Single keyspace |
| Pub / sub                     | n/a | n/a | n/a | Not supported |
| Scripting (EVAL)              | n/a | n/a | n/a | Not supported |

Legend: **Full** = full spec coverage · **Partial** = subset · **n/a** =
not part of that protocol · **Not supported** = not implemented in
fastcached.

The next page, the [Coverage matrix](coverage-matrix.md), maps each
logical operation to the specific command, opcode, or meta-flag in
every protocol.

## Per-protocol pages

- [memcached text](memcached-text.md) — framing, exptime semantics,
  error tokens.
- [memcached binary](memcached-binary.md) — header layout, status
  codes, quiet variants.
- [memcached meta](memcached-meta.md) — meta-flag conventions.
- [Meta flags reference](meta-flags-reference.md) — every single-letter
  flag across `mg`, `ms`, `md`, `ma`.
- [Binary opcodes](binary-opcodes.md) — hex table.
- [Binary status codes](status-codes.md) — every status code
  fastcached emits.
- [Redis RESP2](redis-resp.md) — supported subset and rationale.

## Compatibility

See [Compatibility with upstream](compatibility-with-upstream.md) for
a per-protocol completeness scorecard.

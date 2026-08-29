# memcached (text)

The ASCII line-based protocol that telnet and most older memcached
clients speak.

## Framing

- Every command starts on its own line and ends with `\r\n`.
- Storage commands are followed by a separate data block, also
  `\r\n`-terminated.
- Maximum line length: 4 KiB.
- Maximum payload length: 16 MiB.

## exptime semantics

The `exptime` field on `set`, `add`, `replace`, `cas`, `touch`, `gat`,
`gats`, and the meta `T` flag follows the standard memcached rules:

| Value             | Meaning |
|-------------------|---------|
| `0`               | Never expires |
| `1` … `2592000`   | Relative seconds from now (up to 30 days) |
| `> 2592000`       | Absolute UNIX timestamp |

The 30-day threshold is the memcached convention.

## Flags

The `flags` field is a 32-bit opaque integer that the server stores
verbatim and returns on `get`.

!!! warning "Two flags values are not opaque, and a decoder reads the value under them"

    The Redis value types that are not plain strings are tagged by their `flags` word,
    and the two front ends share one `CacheEngine` — so a value written by a memcached
    client is a value a Redis verb will decode:

    | `flags` | decimal | Tagged as | Verbs that parse the value |
    | --- | --- | --- | --- |
    | `0x5E700001` | 1584398337 | Redis **set** | `SADD`, `SREM`, `SMEMBERS`, `SCARD`, `SISMEMBER`, … |
    | `0x5E700002` | 1584398338 | Redis **stream** | `XADD`, `XLEN`, `XRANGE`, `XREAD`, the group verbs, … |

    Setting one of those words on a `set` means the next such verb on the key parses
    your bytes as that type's blob rather than echoing them.

    That is not a privilege boundary and never was one: it is the same key space,
    reached by two front ends. It is called out because the value under such a key is
    parsed rather than echoed, so it is subject to the decoder's validation. A blob
    declaring more elements than its bytes can supply is refused, and the verb answers
    `-ERR storage failure` rather than the daemon attempting the allocation — for a
    set's member count
    ([#271](https://github.com/LASTRADA-Software/fastcached/issues/271)) and for each of
    a stream's five counts, which previously clamped the reservation instead of
    refusing it ([#269](https://github.com/LASTRADA-Software/fastcached/issues/269)).

    Every other flags value is stored and returned untouched.

## CAS

Every entry has a CAS token (64-bit, monotonically increasing per
storage instance). `cas` and meta `C(token)` use it for
compare-and-swap; `gets`, `gats`, and `mg c` return it to the client.

## noreply

`set`, `add`, `replace`, `append`, `prepend`, `cas`, `delete`, `incr`,
`decr`, `touch`, `flush_all`, `cache_memlimit`, and `verbosity` accept
a trailing `noreply` token. When present, the response (including any
error) is suppressed. Pipelines that use `noreply` lose visibility
into failures.

## Error tokens

| Token            | Meaning |
|------------------|---------|
| `ERROR`          | Unknown command |
| `CLIENT_ERROR <msg>` | The client sent a malformed command |
| `SERVER_ERROR <msg>` | The server hit an internal failure |

## Commands

See the [Commands index](../commands/index.md) for the full list.

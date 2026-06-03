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
`gats`, `cache_memlimit`, and meta `T` flag follows the standard
memcached rules:

| Value             | Meaning |
|-------------------|---------|
| `0`               | Never expires |
| `1` … `2592000`   | Relative seconds from now (up to 30 days) |
| `> 2592000`       | Absolute UNIX timestamp |

The 30-day threshold is the memcached convention.

## Flags

The `flags` field is a 32-bit opaque integer that the server stores
verbatim and returns on `get`. fastcached never interprets it.

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

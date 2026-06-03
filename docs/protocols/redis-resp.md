# Redis RESP2

fastcached implements a subset of RESP2 large enough to serve `sccache`
and similar key-value cache clients. RESP3 is intentionally not
supported.

## Framing

RESP2 messages are length-prefixed text:

- `*N\r\n` — an array of N elements
- `$M\r\n<M bytes>\r\n` — a bulk string of M bytes
- `+<simple string>\r\n` — a simple OK-style reply
- `-<error>\r\n` — an error reply
- `:<integer>\r\n` — an integer reply

Commands are issued as an array of bulk strings. Inline commands (a
single line of space-separated tokens) are also accepted for telnet
debugging.

## Supported commands

| Command                                 | Notes |
|-----------------------------------------|-------|
| `GET <key>`                             | Returns `$M\r\n<value>` or nil |
| `SET <key> <value> [EX|PX <ttl>] [NX|XX]` | Full options |
| `SETEX <key> <ttl> <value>`             | Sets with TTL in seconds |
| `PSETEX <key> <ttl_ms> <value>`         | Sets with TTL in milliseconds |
| `DEL <key>...`                          | Returns count |
| `UNLINK <key>...`                       | Synonym for DEL |
| `EXISTS <key>...`                       | Returns count |
| `PING [msg]`                            | `+PONG` or bulk echo |
| `ECHO <msg>`                            | Bulk echo |
| `INFO`                                  | Returns version + basic stats |
| `HELLO [2]`                             | Acknowledges RESP2 |
| `HELLO 3`                               | `-NOPROTO` (RESP3 unsupported) |
| `COMMAND`                               | Returns command introspection |
| `FLUSHDB` / `FLUSHALL`                  | Drops all entries |
| `QUIT`                                  | `+OK` and close |
| `AUTH ...`                              | `-ERR` (no auth backend) |

## What is not supported

Pub/sub, scripting (EVAL), cluster commands, streams, sorted sets,
hashes, lists, transactions (MULTI/EXEC), CLIENT subcommands beyond
the obvious, RESP3 features (push messages, big numbers, sets,
attributes). See [Unsupported Redis commands](../commands/redis/unsupported.md)
for the full list of common commands that return `-ERR unknown command`.

## Why a subset?

fastcached's storage engine is a key-value cache. The Redis surface
that maps cleanly to that engine is the subset shown above. Adding
data-structure commands (sorted sets, hashes, lists) is a larger
effort tracked separately.

# Coverage matrix

This is the canonical map of logical operations to wire commands. Each
row describes one thing a client can ask the server to do; each
column shows how that operation is expressed in a given protocol.

| Cell value | Meaning |
|------------|---------|
| `name`     | Concrete command, opcode, or meta form (clickable) |
| –          | Not applicable to this protocol |
| stub       | Recognised but returns synthetic output (no real backing) |
| **no**     | Not implemented; the command is rejected or ignored |

| Operation                                    | memcached text | memcached binary | memcached meta | Redis RESP2 |
|----------------------------------------------|----------------|------------------|----------------|-------------|
| Store value unconditionally                  | [`set`](../commands/memcached/storage/set.md) | `0x01` SET / `0x11` SETQ | [`ms M=S`](../commands/memcached/meta/ms.md) | [`SET`](../commands/redis/string/set.md) |
| Store only if absent                         | [`add`](../commands/memcached/storage/add.md) | `0x02` ADD / `0x12` ADDQ | [`ms M=E`](../commands/memcached/meta/ms.md) | [`SET NX`](../commands/redis/string/set.md) |
| Store only if present                        | [`replace`](../commands/memcached/storage/replace.md) | `0x03` REPLACE / `0x13` REPLACEQ | [`ms M=R`](../commands/memcached/meta/ms.md) | [`SET XX`](../commands/redis/string/set.md) |
| Append to existing value                     | [`append`](../commands/memcached/storage/append.md) | `0x0e` APPEND / `0x19` APPENDQ | [`ms M=A`](../commands/memcached/meta/ms.md) | **no** |
| Prepend to existing value                    | [`prepend`](../commands/memcached/storage/prepend.md) | `0x0f` PREPEND / `0x1a` PREPENDQ | [`ms M=P`](../commands/memcached/meta/ms.md) | **no** |
| Compare-and-swap                             | [`cas`](../commands/memcached/storage/cas.md) | via CAS field in header | [`ms C(token)`](../commands/memcached/meta/ms.md) | – |
| Fetch by key                                 | [`get`](../commands/memcached/retrieval/get.md) | `0x00` GET / `0x09` GETQ / `0x0c` GETK / `0x0d` GETKQ | [`mg v`](../commands/memcached/meta/mg.md) | [`GET`](../commands/redis/string/get.md) |
| Fetch with CAS                               | [`gets`](../commands/memcached/retrieval/gets.md) | implicit (CAS in header) | [`mg c v`](../commands/memcached/meta/mg.md) | – |
| Fetch and refresh TTL                        | [`gat`](../commands/memcached/retrieval/gat.md) / [`gats`](../commands/memcached/retrieval/gats.md) | `0x1d` GAT / `0x1e` GATQ / `0x23` GATK / `0x24` GATKQ | [`mg v T(token)`](../commands/memcached/meta/mg.md) | **no** |
| Refresh TTL without reading value            | [`touch`](../commands/memcached/lifetime/touch.md) | `0x1c` TOUCH | [`mg T(token)`](../commands/memcached/meta/mg.md) | **no** |
| Increment numeric value                      | [`incr`](../commands/memcached/arithmetic/incr.md) | `0x05` INCREMENT / `0x15` INCREMENTQ | [`ma M=I`](../commands/memcached/meta/ma.md) | **no** |
| Decrement numeric value                      | [`decr`](../commands/memcached/arithmetic/decr.md) | `0x06` DECREMENT / `0x16` DECREMENTQ | [`ma M=D`](../commands/memcached/meta/ma.md) | **no** |
| Auto-vivify on miss                          | – | `0x05`/`0x06` with exptime!=0xFFFFFFFF | [`mg N(token)`](../commands/memcached/meta/mg.md) / [`ma N J`](../commands/memcached/meta/ma.md) | – |
| Delete key                                   | [`delete`](../commands/memcached/deletion/delete.md) | `0x04` DELETE / `0x14` DELETEQ | [`md`](../commands/memcached/meta/md.md) | [`DEL`](../commands/redis/keys/del.md) / [`UNLINK`](../commands/redis/keys/unlink.md) |
| Mark stale (recache coordination)            | – | – | [`md I`](../commands/memcached/meta/md.md) / [`ms I`](../commands/memcached/meta/ms.md) | – |
| Drop all entries                             | [`flush_all`](../commands/memcached/admin/flush_all.md) | `0x08` FLUSH / `0x18` FLUSHQ | – | [`FLUSHDB`](../commands/redis/server/flushdb.md) / [`FLUSHALL`](../commands/redis/server/flushall.md) |
| Check key existence                          | implicit via `get` | implicit via `0x00` | [`mg h`](../commands/memcached/meta/mg.md) | [`EXISTS`](../commands/redis/keys/exists.md) |
| Server version                               | [`version`](../commands/memcached/admin/version.md) | `0x0b` VERSION | – | inside [`INFO`](../commands/redis/server/info.md) |
| Server stats                                 | [`stats`](../commands/memcached/admin/stats.md) + sub-commands | `0x10` STAT | per-entry via [`me`](../commands/memcached/meta/me.md) | [`INFO`](../commands/redis/server/info.md) |
| Set memory limit at runtime                  | [`cache_memlimit`](../commands/memcached/admin/cache_memlimit.md) | – | – | – |
| Verbosity dial                               | [`verbosity`](../commands/memcached/admin/verbosity.md) (no-op) | `0x1b` VERBOSITY (no-op) | – | – |
| Close connection                             | [`quit`](../commands/memcached/admin/quit.md) | `0x07` QUIT / `0x17` QUITQ | – | [`QUIT`](../commands/redis/connection/quit.md) |
| Heartbeat                                    | – | `0x0a` NOOP | [`mn`](../commands/memcached/meta/mn.md) | [`PING`](../commands/redis/connection/ping.md) |
| Pipeline sync barrier                        | – | – | [`mn`](../commands/memcached/meta/mn.md) | – |
| Slab rebalance                               | stub ([`slabs`](../commands/memcached/slabs/slabs.md)) | – | – | – |
| LRU tuning                                   | stub ([`lru`](../commands/memcached/slabs/lru.md)) | – | – | – |
| LRU crawler control                          | stub ([`lru_crawler`](../commands/memcached/slabs/lru_crawler.md)) | – | – | – |
| Event watch streaming                        | **no** | – | – | – |
| Authentication                               | – | SASL rejected | – | [`AUTH`](../commands/redis/connection/auth.md) rejected |
| Echo                                         | – | – | – | [`ECHO`](../commands/redis/connection/echo.md) |
| Server handshake                             | – | – | – | [`HELLO`](../commands/redis/connection/hello.md) |
| Command introspection                        | – | – | – | [`COMMAND`](../commands/redis/server/command.md) |
| Select database                              | – | – | – | single keyspace only |
| Pub / sub                                    | – | – | – | **no** |
| Scripting (EVAL)                             | – | – | – | **no** |
| Streams / sorted sets / hashes               | – | – | – | **no** |

## Reading the matrix

- A row marked **no** in every column is an operation fastcached does
  not support in any protocol.
- A cell marked – means the protocol doesn't have a way to express the
  operation, so the absence is not a fastcached limitation.
- "stub" cells return well-shaped responses (`OK`) so capability-probing
  clients don't error out, but no underlying state is changed. See the
  linked command page for each stub's exact behavior.

# Commands

Every command fastcached recognises, grouped by protocol and category.
Each page describes the wire form, response codes, and at least one
working example.

## memcached

### Storage
- [set](memcached/storage/set.md) · [add](memcached/storage/add.md) ·
  [replace](memcached/storage/replace.md) ·
  [append](memcached/storage/append.md) ·
  [prepend](memcached/storage/prepend.md) ·
  [cas](memcached/storage/cas.md)

### Retrieval
- [get](memcached/retrieval/get.md) · [gets](memcached/retrieval/gets.md) ·
  [gat](memcached/retrieval/gat.md) · [gats](memcached/retrieval/gats.md)

### Deletion
- [delete](memcached/deletion/delete.md)

### Arithmetic
- [incr](memcached/arithmetic/incr.md) · [decr](memcached/arithmetic/decr.md)

### Lifetime
- [touch](memcached/lifetime/touch.md)

### Meta protocol
- [mg](memcached/meta/mg.md) · [ms](memcached/meta/ms.md) ·
  [md](memcached/meta/md.md) · [ma](memcached/meta/ma.md) ·
  [me](memcached/meta/me.md) · [mn](memcached/meta/mn.md)

### Admin
- [stats](memcached/admin/stats.md) ·
  [flush_all](memcached/admin/flush_all.md) ·
  [cache_memlimit](memcached/admin/cache_memlimit.md) ·
  [verbosity](memcached/admin/verbosity.md) ·
  [version](memcached/admin/version.md) ·
  [quit](memcached/admin/quit.md)

### Slabs / LRU (synthetic stubs)
- [slabs](memcached/slabs/slabs.md) · [lru](memcached/slabs/lru.md) ·
  [lru_crawler](memcached/slabs/lru_crawler.md)

## Redis (RESP2)

### Strings
- [GET](redis/string/get.md) · [SET](redis/string/set.md) ·
  [SETEX](redis/string/setex.md) · [PSETEX](redis/string/psetex.md)

### Keys
- [DEL](redis/keys/del.md) · [UNLINK](redis/keys/unlink.md) ·
  [EXISTS](redis/keys/exists.md)

### Connection
- [PING](redis/connection/ping.md) · [ECHO](redis/connection/echo.md) ·
  [HELLO](redis/connection/hello.md) · [AUTH](redis/connection/auth.md) ·
  [SELECT](redis/connection/select.md) · [CLIENT](redis/connection/client.md) ·
  [QUIT](redis/connection/quit.md)

### Server
- [INFO](redis/server/info.md) · [COMMAND](redis/server/command.md) ·
  [CONFIG](redis/server/config.md) ·
  [FLUSHDB](redis/server/flushdb.md) · [FLUSHALL](redis/server/flushall.md)

### Unsupported
- [Unsupported Redis commands](redis/unsupported.md)

# SELECT

**Protocols:** Redis RESP2

Selects a logical database by index. fastcached has a single flat
keyspace, so `SELECT` is accepted as a no-op for compatibility: any
index replies `+OK` and the index is ignored.

## Synopsis

```text
SELECT <index>
```

## Response

- Always `+OK\r\n`, regardless of the index.

The command exists only so a Redis client library whose connection URL
names a database (for example `redis://host/0`) completes its setup
instead of erroring. There is no per-database isolation — all keys
share one space.

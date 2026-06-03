# lru (stub)

**Protocols:** memcached text · **Status:** Synthetic stub

fastcached does not implement memcached's segmented LRU. Recognises
`lru tune`, `lru mode`, and `lru temp_ttl` and replies `OK` so
capability probes succeed.

## Synopsis

```text
lru tune ...\r\n
lru mode <flat|segmented>\r\n
lru temp_ttl <seconds>\r\n
```

## Response

```text
OK\r\n
```

No state changes.

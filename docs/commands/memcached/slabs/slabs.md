# slabs (stub)

**Protocols:** memcached text · **Status:** Synthetic stub

fastcached does not implement slab allocation — its storage engine
is a flat LRU, not a slab-classed pool. Recognises `slabs reassign`
and `slabs automove` and replies `OK` so capability probes succeed.

## Synopsis

```text
slabs reassign <source> <dest>\r\n
slabs automove <0|1|2>\r\n
```

## Response

```text
OK\r\n
```

No state changes.

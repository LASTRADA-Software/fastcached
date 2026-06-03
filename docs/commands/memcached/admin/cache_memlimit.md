# cache_memlimit

**Protocols:** memcached text

Reconfigures the storage byte budget at runtime. Excess entries are
evicted immediately to fit.

## Synopsis

```text
cache_memlimit <megabytes> [noreply]\r\n
```

## Response

```text
OK\r\n
```

## Example

```text
> cache_memlimit 64
< OK
```

The storage budget is now 64 MiB.

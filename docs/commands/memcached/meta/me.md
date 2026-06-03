# me (meta debug)

**Protocols:** memcached meta

Dumps internal metadata for a single key. The exact field set is
debug-grade and may change between releases.

## Synopsis

```text
me <key> [b]\r\n
```

## Response

| Token | Meaning |
|-------|---------|
| `ME <key> exp=<sec> la=<sec> cas=<u64> fetch=1 cls=1 size=<bytes>` | Hit |
| `EN`  | Miss |

Where:

- `exp` — seconds until expiry (`-1` = never)
- `la` — seconds since last access
- `cas` — current CAS token
- `cls` — synthetic slab class (always `1` in fastcached)
- `size` — value size in bytes

## Example

```text
> set k 0 60 5\r\nhello\r\n
< STORED
> me k
< ME k exp=60 la=0 cas=1 fetch=1 cls=1 size=5
```

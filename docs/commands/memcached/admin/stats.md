# stats

**Protocols:** memcached text · memcached binary (`0x10`)

Returns server statistics. Sub-commands narrow the output to a
specific category.

## Synopsis

```text
stats [<sub-command>]\r\n
```

## Sub-commands

| Sub-command  | Returns |
|--------------|---------|
| (empty)      | Default snapshot: counts, hit/miss, command totals |
| `settings`   | Synthetic subset of effective settings |
| `items`      | Synthetic single-class LRU summary |
| `slabs`      | Synthetic single-class slab summary |
| `sizes`      | Single-bucket size histogram |
| `conns`      | Empty (no connection registry) |
| `reset`      | Acknowledged but no-op; replies `RESET` |

## Default snapshot fields

```text
STAT version <version>
STAT curr_items <n>
STAT bytes <n>
STAT limit_maxbytes <n>
STAT evictions <n>
STAT cmd_get <n>
STAT cmd_set <n>
STAT cmd_touch <n>
STAT cmd_flush <n>
STAT get_hits <n>
STAT get_misses <n>
STAT delete_hits <n>
STAT delete_misses <n>
STAT incr_hits <n>
STAT incr_misses <n>
STAT decr_hits <n>
STAT decr_misses <n>
STAT touch_hits <n>
STAT touch_misses <n>
STAT cas_hits <n>
STAT cas_misses <n>
STAT cas_badval <n>
STAT evicted_unfetched <n>
STAT expired_unfetched <n>
END
```

## Example

```text
> stats
< STAT version fastcached-...
< ...
< END
> stats items
< STAT items:1:number 0
< ...
< END
```

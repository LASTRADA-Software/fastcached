# mg (meta get)

**Protocols:** memcached meta

Generic retrieval. Flags select what to return (value, CAS, flags,
size, TTL, last-access, hit-flag), what to do on miss (auto-vivify,
suppress response), and whether to refresh the TTL on hit.

## Synopsis

```text
mg <key> [flags...]\r\n
```

## Common flags

| Flag         | Effect |
|--------------|--------|
| `v`          | Return value (otherwise header-only) |
| `c`          | Return CAS |
| `f`          | Return client flags |
| `s`          | Return item size |
| `t`          | Return remaining TTL |
| `l`          | Return seconds since last access |
| `h`          | Return hit flag |
| `k`          | Echo key in response |
| `q`          | Quiet on miss (no `EN`) |
| `u`          | Do not bump LRU |
| `T(token)`   | Refresh TTL to `token` seconds |
| `N(token)`   | Auto-vivify with TTL on miss |
| `O(token)`   | Opaque correlator |

Full table: [Meta flags reference](../../../protocols/meta-flags-reference.md).

## Responses

| Token | Meaning |
|-------|---------|
| `HD`  | Hit (header only) |
| `VA <size>` | Hit with value (when `v` set); body follows |
| `EN`  | Miss |

## Examples

```text
> set k 7 0 5
> hello
< STORED
> mg k v c f
< VA 5 c1 f7
< hello
> mg absent
< EN
> mg absent N60 v
< VA 0
```

# ma (meta arithmetic)

**Protocols:** memcached meta

Increment or decrement an integer value, with optional auto-vivify
and TTL refresh in the same round-trip.

## Synopsis

```text
ma <key> [flags...]\r\n
```

## Common flags

| Flag         | Effect |
|--------------|--------|
| `M(mode)`    | `I` or `+` increment (default), `D` or `-` decrement |
| `D(token)`   | Delta (default 1) |
| `J(token)`   | Initial value for auto-vivified entries |
| `N(token)`   | Auto-vivify with TTL |
| `T(token)`   | Refresh TTL after operation |
| `v`          | Return new value as `VA <size>\r\n<value>\r\n` |
| `c`          | Return new CAS |
| `t`          | Return TTL |
| `k`          | Echo key |
| `q`          | Quiet — suppress only the success line; errors (`NF`/`CLIENT_ERROR`) are still sent |
| `O(token)`   | Opaque correlator |

Full table: [Meta flags reference](../../../protocols/meta-flags-reference.md).

## Responses

| Token | Meaning |
|-------|---------|
| `HD`  | Success (when `v` not set) |
| `VA <size>` | Success with new value (when `v` set) |
| `NF`  | Key does not exist and no `N` was set |
| `NS`  | Not stored |
| `EX`  | CAS mismatch |

## Examples

```text
> set c 0 0 2\r\n10\r\n
< STORED
> ma c v
< VA 2
< 11
> ma c M=D D5 v
< VA 1
< 6
> ma fresh N60 J42 v
< VA 2
< 42
```

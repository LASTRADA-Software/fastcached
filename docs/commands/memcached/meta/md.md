# md (meta delete)

**Protocols:** memcached meta

Removes an entry or marks it stale. Supports CAS-conditional delete.

## Synopsis

```text
md <key> [flags...]\r\n
```

## Common flags

| Flag         | Effect |
|--------------|--------|
| `C(token)`   | Delete only if CAS matches (atomic compare-and-delete) |
| `I`          | Mark stale rather than remove |
| `x`          | Remove value but keep the entry *(accepted but not yet implemented)* |
| `T(token)`   | Update TTL when marking stale (with `I`) |
| `k`          | Echo key |
| `q`          | Quiet — suppress the benign outcomes (`HD`/`NF`); a CAS mismatch (`EX`) is still sent |
| `O(token)`   | Opaque correlator |

Full table: [Meta flags reference](../../../protocols/meta-flags-reference.md).

## Responses

| Token | Meaning |
|-------|---------|
| `HD`  | Removed |
| `NF`  | Key did not exist |
| `EX`  | CAS mismatch |

## Examples

```text
> set k 0 0 1\r\nA\r\n
< STORED
> md k
< HD
> md k C999
< NF
```

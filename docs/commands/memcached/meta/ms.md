# ms (meta set)

**Protocols:** memcached meta

Generic storage. The mode (`M=S` set, `M=E` add, `M=A` append,
`M=P` prepend, `M=R` replace) selects which storage primitive runs.

## Synopsis

```text
ms <key> <bytes> [flags...]\r\n
<data>\r\n
```

## Common flags

| Flag         | Effect |
|--------------|--------|
| `M(mode)`    | `S` set (default), `E` add, `A` append, `P` prepend, `R` replace |
| `T(token)`   | TTL in seconds |
| `F(token)`   | Client flags (32-bit) |
| `C(token)`   | Compare-and-swap against `token` |
| `c`          | Return CAS on success |
| `k`          | Echo key |
| `s`          | Return stored size |
| `q`          | Quiet — suppress only the success line (`HD`); errors (`NS`/`EX`/`NF`) are still sent |
| `I`          | Mark stale instead of storing |
| `O(token)`   | Opaque correlator |

Full table: [Meta flags reference](../../../protocols/meta-flags-reference.md).

## Responses

| Token | Meaning |
|-------|---------|
| `HD`  | Success |
| `NS`  | Not stored (precondition failed, e.g. `M=E` on existing key) |
| `NF`  | Not found (e.g. `M=R` on missing key) |
| `EX`  | CAS mismatch (when `C(token)` is set) |

## Examples

```text
> ms greeting 5\r\nhello\r\n
< HD
> ms greeting 1 M=E\r\nA\r\n
< NS
> ms greeting 1 C999\r\nB\r\n
< EX
```

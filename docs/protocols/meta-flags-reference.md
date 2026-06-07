# Meta flags reference

The canonical table of every single-letter flag across `mg`, `ms`,
`md`, `ma`. Tokens shown as `(token)` take an inline argument.

## `mg` (meta get)

| Flag    | Effect |
|---------|--------|
| `b`     | Key is base64-encoded (spec); accepted but not decoded — the key is used verbatim |
| `c`     | Return CAS in response |
| `f`     | Return client flags |
| `h`     | Return hit status (`1` on hit) |
| `k`     | Echo key in response |
| `l`     | Return seconds since last access |
| `O(token)` | Opaque request/response correlator |
| `q`     | Quiet on miss (suppress `EN`) |
| `s`     | Return item size |
| `t`     | Return remaining TTL in seconds |
| `u`     | Don't bump LRU on hit |
| `v`     | Return value (otherwise header-only) |
| `T(token)` | Update TTL to `token` seconds |
| `N(token)` | Auto-vivify on miss with TTL `token` |
| `R(token)` | Conditional recache win threshold *(accepted but not yet implemented)* |
| `E(token)` | Override CAS to `token` *(accepted but not yet implemented)* |

## `ms` (meta set)

| Flag    | Effect |
|---------|--------|
| `b`     | Key is base64-encoded (spec); accepted but not decoded — key used verbatim |
| `c`     | Return CAS on success |
| `C(token)` | Compare-and-swap against `token` |
| `E(token)` | Override CAS to `token` *(accepted but not yet implemented)* |
| `F(token)` | Set client flags (32-bit) |
| `I`     | Mark stale instead of storing |
| `k`     | Echo key in response |
| `M(mode)` | Mode: `S` set (default), `E` add, `A` append, `P` prepend, `R` replace |
| `N(token)` | Auto-vivify on miss with TTL (for append modes) |
| `O(token)` | Opaque correlator |
| `q`     | Quiet — suppress only the success line (`HD`); errors (`NS`/`EX`/`NF`) are still returned |
| `s`     | Return stored size |
| `T(token)` | TTL in seconds |

## `md` (meta delete)

| Flag    | Effect |
|---------|--------|
| `b`     | Key is base64-encoded (spec); accepted but not decoded — key used verbatim |
| `C(token)` | Delete only if CAS matches |
| `E(token)` | Override CAS to `token` *(accepted but not yet implemented)* |
| `I`     | Mark stale rather than remove |
| `k`     | Echo key in response |
| `O(token)` | Opaque correlator |
| `q`     | Quiet — suppress the benign outcomes (`HD`/`NF`); a CAS mismatch (`EX`) is still returned |
| `T(token)` | Update TTL when marking stale |
| `x`     | Remove value but keep the metadata entry *(accepted but not yet implemented)* |

## `ma` (meta arithmetic)

| Flag    | Effect |
|---------|--------|
| `b`     | Key is base64-encoded (spec); accepted but not decoded — key used verbatim |
| `C(token)` | Compare-and-swap |
| `E(token)` | Override CAS to `token` *(accepted but not yet implemented)* |
| `N(token)` | Auto-vivify on miss with TTL |
| `J(token)` | Initial value for auto-vivified entries |
| `D(token)` | Delta (default 1) |
| `T(token)` | Update TTL |
| `M(mode)` | Mode: `I` or `+` increment, `D` or `-` decrement |
| `O(token)` | Opaque correlator |
| `q`     | Quiet — suppress only the success line; errors (`NF`/`CLIENT_ERROR`) are still returned |
| `t`     | Return TTL |
| `c`     | Return new CAS |
| `v`     | Return new value (as `VA <size>\r\n<value>\r\n`) |
| `k`     | Echo key in response |

## `me` (meta debug)

| Flag    | Effect |
|---------|--------|
| `b`     | Key is base64-encoded (spec); accepted but not decoded — key used verbatim |

`me` emits a single `ME <key> <field>=<value>...\r\n` line per entry
listing internal state (expiry, last access, CAS, size). Format is
debug-grade and may change between releases.

## `mn` (meta no-op)

No flags. Always returns `MN\r\n`.

## Response flags

These appear in the response after the two-letter token:

| Response flag | Meaning |
|---------------|---------|
| `c<token>`    | CAS                       |
| `f<token>`    | Client flags              |
| `s<token>`    | Size                      |
| `t<token>`    | TTL in seconds            |
| `k<token>`    | Echoed key                |
| `O<token>`    | Echoed opaque             |
| `l<token>`    | Last-access time          |
| `h<token>`    | Hit flag (`0` or `1`)     |
| `X`           | Item is stale             |

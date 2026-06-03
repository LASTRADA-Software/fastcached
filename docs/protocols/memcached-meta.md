# memcached (meta)

The meta protocol is the modern memcached interface, introduced in
1.6. It collapses the classic set / add / replace / append / prepend
/ cas / get / gets / delete / incr / decr / touch / gat / gats
commands into six generic commands governed by single-letter flags:

| Command | Purpose                              |
|---------|--------------------------------------|
| `mg`    | Meta get                             |
| `ms`    | Meta set (with payload on next line) |
| `md`    | Meta delete                          |
| `ma`    | Meta arithmetic (incr / decr)        |
| `me`    | Meta debug (dump metadata)           |
| `mn`    | Meta no-op (pipeline barrier)        |

## Framing

Meta commands are line-based like the classic text protocol. `ms` is
the only one with a payload — the value bytes follow the command line
on a separate line.

```text
ms greeting 5 T60\r\n
hello\r\n
< HD\r\n
```

## Flags

Each flag is a single ASCII letter, optionally followed immediately
by a token. Tokens are alphanumeric, except for `M=<mode>` which uses
an `=` separator.

```text
mg foo c k v O12345\r\n
```

Above: `c` (return CAS), `k` (echo key), `v` (return value), `O12345`
(opaque id).

See [Meta flags reference](meta-flags-reference.md) for the canonical
table covering all flags across all meta commands.

## Responses

| Token | Meaning                                    |
|-------|--------------------------------------------|
| `HD`  | Header-only success                        |
| `VA`  | Value response (header + size + value)     |
| `EN`  | Miss (no entry)                            |
| `NS`  | Not stored (precondition failed)           |
| `NF`  | Not found (no entry)                       |
| `EX`  | Exists / CAS mismatch                      |
| `ME`  | `me` metadata dump                         |
| `MN`  | `mn` no-op reply                           |

Response flags (appended after the two-letter token) include `c`, `f`,
`s`, `t`, `k`, `O`, `l`, `h`, and `X` (stale marker).

## Pipelining

`mn` is the canonical sync barrier. A client batches commands followed
by an `mn`; the `MN\r\n` reply confirms every preceding command has
been processed.

## Mixing with classic text

Meta and classic commands share a single connection. A client may
issue `mg`, `set`, `mg`, `delete`, `ma` on the same socket in any
order.

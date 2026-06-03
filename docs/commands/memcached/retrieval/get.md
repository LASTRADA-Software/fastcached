# get

**Protocols:** memcached text · memcached binary (`0x00` / `0x09` GETQ / `0x0c` GETK / `0x0d` GETKQ) · meta `mg v` · Redis `GET`

Fetches one or more entries. Missing keys are silently skipped — the
response is the list of hits followed by `END`.

## Synopsis

```text
get <key> [<key>...]\r\n
```

## Response

For each hit:

```text
VALUE <key> <flags> <bytes>\r\n
<data>\r\n
```

Followed by:

```text
END\r\n
```

## Example

```text
> set k 7 0 5
> hello
< STORED
> get k missing
< VALUE k 7 5
< hello
< END
```

## See also

- [gets](gets.md) — same plus CAS token
- [gat](gat.md) / [gats](gats.md) — get + refresh TTL
- meta equivalent: [mg](../meta/mg.md)

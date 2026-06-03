# add

**Protocols:** memcached text · memcached binary (`0x02` / `0x12` ADDQ) · meta `ms M=E`

Stores a value only if the key is currently absent. Returns
`NOT_STORED` if the key already exists.

## Synopsis

```text
add <key> <flags> <exptime> <bytes> [noreply]\r\n
<data>\r\n
```

## Responses

| Token        | Meaning |
|--------------|---------|
| `STORED`     | Value persisted |
| `NOT_STORED` | Key already exists |

## Example

```text
> add k 0 60 1
> A
< STORED
> add k 0 60 1
> B
< NOT_STORED
```

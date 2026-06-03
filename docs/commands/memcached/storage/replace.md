# replace

**Protocols:** memcached text · memcached binary (`0x03` / `0x13` REPLACEQ) · meta `ms M=R`

Stores a value only if the key currently exists. Returns `NOT_STORED`
on miss.

## Synopsis

```text
replace <key> <flags> <exptime> <bytes> [noreply]\r\n
<data>\r\n
```

## Responses

| Token        | Meaning |
|--------------|---------|
| `STORED`     | Value replaced |
| `NOT_STORED` | Key did not exist |

## Example

```text
> replace k 0 60 1
> A
< NOT_STORED
> set k 0 60 1
> A
< STORED
> replace k 0 60 1
> B
< STORED
```

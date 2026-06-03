# decr

**Protocols:** memcached text · memcached binary (`0x06` / `0x16` DECREMENTQ) · meta `ma M=D`

Atomically subtracts `delta` from the stored numeric value. Saturates
at zero per memcached convention.

## Synopsis

```text
decr <key> <delta> [noreply]\r\n
```

## Responses

| Token            | Meaning |
|------------------|---------|
| `<new-value>`    | The post-decrement value (≥ 0) |
| `NOT_FOUND`      | Key did not exist |
| `CLIENT_ERROR ...` | Stored value is not numeric |

## Example

```text
> set c 0 0 1
> 3
< STORED
> decr c 1
< 2
> decr c 10
< 0
```

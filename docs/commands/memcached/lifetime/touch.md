# touch

**Protocols:** memcached text · memcached binary (`0x1c`) · meta `mg T(token)`

Refreshes an entry's TTL without rewriting its value. CAS is bumped.

## Synopsis

```text
touch <key> <exptime> [noreply]\r\n
```

## Responses

| Token       | Meaning |
|-------------|---------|
| `TOUCHED`   | TTL updated |
| `NOT_FOUND` | Key did not exist |

## Example

```text
> set k 0 1 1
> A
< STORED
> touch k 120
< TOUCHED
```

The key's expiry is now 120 seconds from now (was 1 second).

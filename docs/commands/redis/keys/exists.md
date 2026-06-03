# EXISTS

**Protocols:** Redis RESP2

Returns the count of keys that exist (the same key counted multiple
times if listed multiple times, per Redis semantics).

## Synopsis

```text
EXISTS key [key ...]
```

## Response

Integer reply.

## Example

```sh
redis-cli> SET a 1
OK
redis-cli> EXISTS a a b
(integer) 2
```

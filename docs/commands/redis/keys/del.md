# DEL

**Protocols:** Redis RESP2

Removes one or more keys. Returns the number of keys that were
actually removed.

## Synopsis

```text
DEL key [key ...]
```

## Response

Integer reply — count of deleted keys.

## Example

```sh
redis-cli> SET a 1
OK
redis-cli> DEL a b c
(integer) 1
```

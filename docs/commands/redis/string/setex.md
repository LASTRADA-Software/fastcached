# SETEX

**Protocols:** Redis RESP2

Stores `value` at `key` with a TTL in seconds.

## Synopsis

```text
SETEX key seconds value
```

## Response

```text
+OK
```

## Example

```sh
redis-cli> SETEX k 60 hello
OK
```

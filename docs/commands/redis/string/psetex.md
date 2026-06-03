# PSETEX

**Protocols:** Redis RESP2

Stores `value` at `key` with a TTL in milliseconds.

## Synopsis

```text
PSETEX key milliseconds value
```

## Response

```text
+OK
```

## Example

```sh
redis-cli> PSETEX k 60000 hello
OK
```

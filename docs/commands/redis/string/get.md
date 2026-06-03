# GET

**Protocols:** Redis RESP2

Fetches the value at `key`.

## Synopsis

```text
GET key
```

## Response

- Bulk string with the value on hit
- Nil on miss

## Example

```sh
redis-cli> SET k hello
OK
redis-cli> GET k
"hello"
redis-cli> GET absent
(nil)
```

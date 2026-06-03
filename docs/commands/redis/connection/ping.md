# PING

**Protocols:** Redis RESP2

Heartbeat. Returns `+PONG` when called without an argument, or echoes
the argument as a bulk string when called with one.

## Synopsis

```text
PING [message]
```

## Response

- `+PONG` (no argument)
- Bulk string echoing `message`

## Example

```sh
redis-cli> PING
PONG
redis-cli> PING hello
"hello"
```

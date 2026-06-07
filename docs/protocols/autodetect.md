# Autodetection

When a client connects, fastcached peeks the first byte and routes the
connection to one of three protocol handlers — memcached binary,
Redis RESP2, or memcached text. (The meta commands are dispatched from
inside the text handler, so they need no first-byte rule of their own.)
The rule set is small and unambiguous; mis-classification would mean a
closed connection, not a wrong answer.

## Rules

| First byte(s)     | Routed to                          |
|-------------------|------------------------------------|
| `0x80`            | memcached binary handler           |
| `*`               | Redis RESP2 handler (array form)   |
| `+` `-` `:` `$`   | Redis RESP2 handler (inline form)  |
| anything else     | memcached text handler (line-based) |

`mg`, `ms`, `md`, `ma`, `me`, `mn` are dispatched from inside the
memcached text handler, so they don't need their own first-byte rule.

## Why these bytes?

- **`0x80`** is the request magic of the memcached binary protocol
  header. It's a non-printable byte, so no text-protocol command can
  start with it.
- **`*`** opens a RESP array; **`+`/`-`/`:`/`$`** are the simple-string,
  error, integer, and bulk-string markers respectively. None can begin
  a memcached text command.
- Everything else falls through to the text handler, which is
  tolerant: an unrecognised first token returns `ERROR\r\n` and the
  loop continues.

## Mixing protocols on one connection

A connection is bound to its detected protocol for its lifetime.
Switching mid-stream is not supported and would produce undefined
behavior on the wire. The exception is meta commands, which share
the text handler with the classic ASCII commands and can interleave
freely.

## What happens on an unparseable stream

If the bytes don't match a binary magic or a RESP marker, the text
handler is invoked, which will eventually see a malformed line and
respond `ERROR\r\n`. Persistently broken clients disconnect after
the line-too-long limit (4 KiB).

If the connection reaches EOF before sending a single byte, detection
returns a `NetErrorCode::Eof` error and the connection is closed
without dispatching any handler.

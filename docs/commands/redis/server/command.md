# COMMAND

**Protocols:** Redis RESP2

Returns a list of supported commands with their arities and flags, in
the format Redis clients use for introspection.

## Synopsis

```text
COMMAND
COMMAND COUNT
COMMAND DOCS
```

## Response

Always an empty array (`*0\r\n`), regardless of any sub-command or
arguments. fastcached does not ship a command table; this reply exists
only because `sccache` issues `COMMAND` as a connection sanity check
and needs a well-formed array back rather than an error.

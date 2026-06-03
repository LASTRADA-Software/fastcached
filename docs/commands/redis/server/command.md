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

An array of arrays describing each supported command. Clients that
use this for capability detection see only the commands fastcached
implements.

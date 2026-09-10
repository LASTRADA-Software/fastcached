# COMMAND

**Protocols:** Redis RESP2 / RESP3

Real introspection over fastcached's own dispatch table, in the format
Redis clients use: one descriptor per supported command, carrying the
name, arity and key positions the server actually enforces.

## Synopsis

```text
COMMAND
COMMAND COUNT
COMMAND INFO <name>...
COMMAND DOCS [<name>...]
```

## Response

| Form | Reply |
|------|-------|
| `COMMAND` | An array of one descriptor per row of the table |
| `COMMAND COUNT` | The row count as an integer |
| `COMMAND INFO <name>` | One descriptor, or nil for an unknown name |
| `COMMAND DOCS` | An empty array -- the human-readable metadata is not carried |

A descriptor is `[name, arity, flags, first-key, last-key, key-step]`,
with `flags` always an empty array: fastcached enforces arity and key
positions but publishes no flag set.

The row count is whatever `CommandTable` holds, so it moves when the
supported surface does -- read it from the server rather than from this
page.

## Notes

- `COMMAND` is what `sccache` issues as a connection sanity check, which
  is why it answered a bare `*0` before the table existed.
- `COMMAND DOCS` returning an empty array is deliberate rather than
  unimplemented: an empty array is a well-formed answer meaning *no
  documentation is carried*, which is true, where an error would tell a
  client the verb is unknown.

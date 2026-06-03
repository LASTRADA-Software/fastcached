# set

**Protocols:** memcached text · memcached binary (`0x01` / `0x11` SETQ) · meta `ms M=S` · Redis `SET`

Stores a value unconditionally. Overwrites any existing entry under
the same key and issues a fresh CAS token.

## Synopsis

```text
set <key> <flags> <exptime> <bytes> [noreply]\r\n
<data>\r\n
```

## Parameters

| Field   | Type | Description                                               |
|---------|------|-----------------------------------------------------------|
| key     | str  | 1–250 bytes, no whitespace                                |
| flags   | u32  | Opaque, returned verbatim on get                          |
| exptime | u32  | See [exptime semantics](../../../protocols/memcached-text.md#exptime-semantics) |
| bytes   | u32  | Payload byte count (on the next line)                     |
| noreply |      | Suppress the response                                     |

## Responses

| Token        | Meaning |
|--------------|---------|
| `STORED`     | Value persisted |
| `CLIENT_ERROR <msg>` | Malformed command line |
| `SERVER_ERROR <msg>` | Internal failure (e.g. payload exceeds `max_item_size`) |

## Example

```text
> set greeting 0 60 5
> hello
< STORED
```

## See also

- [add](add.md) · [replace](replace.md) · [cas](cas.md)
- meta equivalent: [ms](../meta/ms.md)
- Redis equivalent: [SET](../../redis/string/set.md)

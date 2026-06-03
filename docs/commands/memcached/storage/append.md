# append

**Protocols:** memcached text · memcached binary (`0x0e` / `0x19` APPENDQ) · meta `ms M=A`

Concatenates the new bytes to the existing value. Flags and TTL
are preserved; CAS is bumped.

## Synopsis

```text
append <key> <flags> <exptime> <bytes> [noreply]\r\n
<data>\r\n
```

Note: the `flags` and `exptime` fields are present on the wire for
parser-compat reasons but are not applied to the stored entry.

## Responses

| Token        | Meaning |
|--------------|---------|
| `STORED`     | Suffix appended |
| `NOT_STORED` | Key did not exist |

## Example

```text
> set k 0 60 3
> foo
< STORED
> append k 0 60 3
> bar
< STORED
> get k
< VALUE k 0 6
< foobar
< END
```

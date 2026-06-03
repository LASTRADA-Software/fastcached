# prepend

**Protocols:** memcached text · memcached binary (`0x0f` / `0x1a` PREPENDQ) · meta `ms M=P`

Inserts the new bytes at the front of the existing value. Flags and
TTL are preserved; CAS is bumped.

## Synopsis

```text
prepend <key> <flags> <exptime> <bytes> [noreply]\r\n
<data>\r\n
```

## Responses

| Token        | Meaning |
|--------------|---------|
| `STORED`     | Prefix prepended |
| `NOT_STORED` | Key did not exist |

## Example

```text
> set k 0 60 3
> bar
< STORED
> prepend k 0 60 3
> foo
< STORED
> get k
< VALUE k 0 6
< foobar
< END
```

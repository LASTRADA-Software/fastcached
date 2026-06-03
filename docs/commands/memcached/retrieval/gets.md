# gets

**Protocols:** memcached text · meta `mg c v`

Like [get](get.md), but each `VALUE` line includes the entry's CAS
token. Use this with [cas](../storage/cas.md) for lockless updates.

## Synopsis

```text
gets <key> [<key>...]\r\n
```

## Response

```text
VALUE <key> <flags> <bytes> <cas-token>\r\n
<data>\r\n
...
END\r\n
```

## Example

```text
> set k 0 0 1
> A
< STORED
> gets k
< VALUE k 0 1 1
< A
< END
```

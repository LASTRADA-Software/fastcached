# gat

**Protocols:** memcached text · memcached binary (`0x1d` / `0x1e` GATQ) · meta `mg v T(token)`

Get-and-touch: fetches the entry's value and resets its TTL in one
round-trip. Missing keys are skipped.

## Synopsis

```text
gat <exptime> <key> [<key>...]\r\n
```

The new exptime is applied to every key that hits.

## Response

Same shape as [get](get.md).

## Example

```text
> set k 0 1 5
> hello
< STORED
> gat 120 k
< VALUE k 0 5
< hello
< END
```

The key's TTL is now 120 seconds.

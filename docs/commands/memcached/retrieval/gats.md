# gats

**Protocols:** memcached text · memcached binary (`0x23` GATK / `0x24` GATKQ) · meta `mg c v T(token)`

Like [gat](gat.md), but each `VALUE` line includes the CAS token —
useful for lockless update workflows that also want to extend the TTL
during the read.

## Synopsis

```text
gats <exptime> <key> [<key>...]\r\n
```

## Response

Same shape as [gets](gets.md).

## Example

```text
> set k 0 60 1
> A
< STORED
> gats 120 k
< VALUE k 0 1 2
< A
< END
```

The CAS bumps because `touch` mutates the entry.

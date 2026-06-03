# UNLINK

**Protocols:** Redis RESP2

Alias of [DEL](del.md). Redis differentiates the two by lazy-freeing
the memory; fastcached's storage engine releases entries inline, so
the two are equivalent here.

## Synopsis

```text
UNLINK key [key ...]
```

## Response

Integer reply — count of removed keys.

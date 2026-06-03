# cas

**Protocols:** memcached text · meta `ms C(token)`

Compare-and-swap: stores the value only if the entry's current CAS
matches the one supplied. Use this with [gets](../retrieval/gets.md)
to do safe lockless updates.

## Synopsis

```text
cas <key> <flags> <exptime> <bytes> <cas-token> [noreply]\r\n
<data>\r\n
```

## Responses

| Token        | Meaning |
|--------------|---------|
| `STORED`     | CAS matched and value was replaced |
| `EXISTS`     | CAS did not match — someone else mutated the key |
| `NOT_FOUND`  | The key does not exist |

## Example

```text
> set k 0 60 1
> A
< STORED
> gets k
< VALUE k 0 1 1
< A
< END
> cas k 0 60 1 1
> B
< STORED
> cas k 0 60 1 1
> C
< EXISTS
```

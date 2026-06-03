# delete

**Protocols:** memcached text · memcached binary (`0x04` / `0x14` DELETEQ) · meta `md` · Redis `DEL` / `UNLINK`

Removes an entry. Returns `NOT_FOUND` if the key was not present.

## Synopsis

```text
delete <key> [noreply]\r\n
```

## Responses

| Token       | Meaning |
|-------------|---------|
| `DELETED`   | Entry removed |
| `NOT_FOUND` | Key did not exist |

## Example

```text
> set k 0 0 1
> A
< STORED
> delete k
< DELETED
> delete k
< NOT_FOUND
```

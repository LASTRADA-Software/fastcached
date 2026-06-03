# flush_all

**Protocols:** memcached text · memcached binary (`0x08` / `0x18` FLUSHQ) · Redis `FLUSHDB` / `FLUSHALL`

Marks every entry as invalid. Optional delay defers the flush by the
given number of seconds.

## Synopsis

```text
flush_all [<delay>] [noreply]\r\n
```

## Response

```text
OK\r\n
```

## Example

```text
> flush_all
< OK
```

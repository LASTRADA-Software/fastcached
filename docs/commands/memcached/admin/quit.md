# quit

**Protocols:** memcached text · memcached binary (`0x07` / `0x17` QUITQ) · Redis `QUIT`

Closes the connection. No response is emitted; the socket simply
closes.

## Synopsis

```text
quit\r\n
```

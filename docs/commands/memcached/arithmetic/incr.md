# incr

**Protocols:** memcached text · memcached binary (`0x05` / `0x15` INCREMENTQ) · meta `ma M=I`

Atomically interprets the stored value as an ASCII unsigned 64-bit
integer and adds `delta`. The new value is stored back as ASCII and
returned. The addition is unsigned 64-bit and wraps modulo 2⁶⁴ on
overflow, matching memcached (the complementary [decr](decr.md)
saturates at zero rather than wrapping).

## Synopsis

```text
incr <key> <delta> [noreply]\r\n
```

## Responses

| Token                            | Meaning |
|----------------------------------|---------|
| `<new-value>`                    | The post-increment value |
| `NOT_FOUND`                      | The key does not exist |
| `CLIENT_ERROR cannot increment or decrement non-numeric value` | The stored value is not a base-10 unsigned integer |

## Example

```text
> set counter 0 0 2
> 10
< STORED
> incr counter 5
< 15
> incr counter 1
< 16
```

## Auto-vivify (binary only)

The binary `Increment` opcode supports auto-vivify with an initial
value; see [Binary opcodes](../../../protocols/binary-opcodes.md).
The text `incr` command does not.

# mn (meta no-op)

**Protocols:** memcached meta

Pipeline barrier. Always returns `MN\r\n`. Use after a batch of quiet
meta commands to confirm they have all been processed.

## Synopsis

```text
mn\r\n
```

## Response

```text
MN\r\n
```

## Example

```text
> ms a 1 q\r\nA\r\n
> ms b 1 q\r\nB\r\n
> ms c 1 q\r\nC\r\n
> mn\r\n
< MN
```

The `MN` reply confirms all three `ms` commands have completed.

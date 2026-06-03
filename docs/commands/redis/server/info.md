# INFO

**Protocols:** Redis RESP2

Returns a bulk string containing the server version and a few basic
counters in the `field:value` format Redis clients expect.

## Synopsis

```text
INFO [section]
```

The section argument is accepted but the response always covers the
same minimal set today.

## Response

Bulk string containing:

```text
# Server
redis_version: <fastcached version>
...
# Clients
...
# Memory
used_memory: <bytes>
...
```

The exact fields are a subset of what Redis emits; clients that look
for `redis_version` and `used_memory` work as expected.

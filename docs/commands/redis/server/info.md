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

A bulk string with three sections and a fixed, minimal field set:

```text
# Server
fastcached_version:<version>
redis_version:6.0.0-fastcached
# Memory
used_memory:<bytes>
maxmemory:<byte limit>
# Stats
total_commands_processed:<count>
keyspace_hits:<count>
keyspace_misses:<count>
```

The `section` argument is accepted but ignored — the same fields are
always returned. There is no `# Clients` section. The values are drawn
from the same counters the memcached `stats` command exposes. Clients
that look for `redis_version` and `used_memory` work as expected.

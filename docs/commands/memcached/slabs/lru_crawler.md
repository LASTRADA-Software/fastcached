# lru_crawler (stub)

**Protocols:** memcached text · **Status:** Synthetic stub

fastcached does not implement a separate LRU crawler thread; expired
entries are purged on access and on demand via the engine's
`PurgeExpired` call. Recognises `lru_crawler enable / disable / sleep
/ tocrawl / crawl / metadump / mgdump` and replies `OK` so capability
probes succeed.

## Synopsis

```text
lru_crawler <subcommand> [args]\r\n
```

## Response

```text
OK\r\n
```

No state changes (with the eventual exception of `crawl` triggering
`PurgeExpired`, which is a planned follow-up — see the relevant
plan for status).

# Upgrading a persistent store

`--storage` (and the compile worker's `--cache-dir`) keeps cache entries in a
copy-on-write B+tree, and the layout of the records inside it carries a version
number. A build reads and writes exactly one version. When a release changes that
layout, a store written by an older build is **refused at startup** rather than
read under the new rules — silently mis-parsing on-disk records is the one
failure that would not be recoverable.

That refusal is not a report of damage. The store is intact.

## What it looks like

```
fastcached: failed to open storage '/var/lib/fastcached/cache':
StorageError(code=13 system=0 context=on-disk storage format version 3
(this build reads and writes 4): the store is intact and does not need to be
deleted -- convert it with `fastcached --migrate-storage` ...)
```

`code=13` is `UnsupportedFormatVersion`, and it exists specifically so that this
is distinguishable from `Corrupt` by something other than reading English. If you
are alerting on storage errors, alert on the two differently: `Corrupt` means the
bytes are damaged, this means they were written by a different build.

If what you are actually looking at is `Corrupt`, this is the wrong page —
[When a store reports `Corrupt`](corrupt-store.md) is the one.

!!! warning "`Corrupt` used to mean a third thing, and it does not any more"

    A set or a stream is an ordinary value distinguished only by its flags word,
    and the memcached text verbs let any client choose that word — so until
    [#296](https://github.com/LASTRADA-Software/fastcached/issues/296) a client
    could plant a few bytes, tag them as a set, read them back, and make the
    daemon report **`Corrupt` against a store whose every record still verified**.
    It moved `fastcached_write_errors_total` and wrote a `storage write failed`
    line, so an unprivileged client could drive the disk-failure signal at will.

    That condition is now `MalformedValue`, counted by

    ```
    fastcached_cache_malformed_values_total
    ```

    and it is **not** a persistence failure: it moves no write-error counter and
    writes no warning. Read a rise as *clients are sending malformed values*, and
    look at the client. `Corrupt` beside a rising `fastcached_write_errors_total`
    is what still means the disk.

    If you were alerting on `Corrupt` before this release, that alert was
    reachable from the network. Keep it — it is now only reachable by real damage.

## Converting

Stop the daemon, then run the conversion with **the same storage options the
daemon runs with**:

```bash
fastcached --migrate-storage --config /etc/fastcached/fastcached.yaml
```

The flag takes no path of its own. It converts exactly the files the daemon would
open, which matters for a sharded layout: `--storage` naming a directory means
`shard-00.cow`, `shard-01.cow` and so on beneath it, and each is converted and
reported separately.

For a compile worker, the equivalent is:

```bash
fastcache-compile-node --migrate-cache --cache-dir /var/cache/fastcache-node
```

Both print a line per store and exit non-zero if any of them could not be
converted.

## What it does, and what it costs

The conversion rewrites every leaf record in place. Values held out-of-line — the
large ones, which for a compile cache is most of the bytes — are **not** copied:
only the small descriptor that points at them is rewritten. Converting a 50 GB
cache therefore does not need 100 GB of free space to do it.

It commits in slices rather than as one transaction, and the file grows by about
one slice while it runs. That is a fixed cost rather than one that scales with
the store.

It is safe to run more than once. A store already in the current layout is
reported as such and not written to at all.

## If it is interrupted

Each slice records how far it got, in the same transaction as the records it
converted. So a conversion killed part-way leaves a store that says so:

```
an interrupted conversion left this store part-way from on-disk storage format
version 3 to 4: re-run `fastcached --migrate-storage` to finish it, which
resumes where it stopped. The store is intact and does not need to be deleted.
```

Run the same command again. It picks up from the last committed slice — nothing
is converted twice and nothing is skipped.

## When it cannot be done

- **The store is newer than the build.** There is no reader for a layout that did
  not exist when this binary was compiled, so the answer is to upgrade rather
  than to convert.
- **The store predates any reader this build still carries.** A format is
  convertible exactly as long as its reader is kept, and that decision is made
  deliberately, one table row per readable layout. Where the reader has been
  retired, the store is refused — and, importantly, **not modified**: the
  conversion parses every record before it writes any of them, so a store it
  cannot read is left exactly as it was found.

In both cases the cache has to be discarded and rebuilt. It is a cache; nothing
in it is unique. The point of everything above is that you should never have to
find that out by guessing.

!!! warning "Run it with the daemon stopped"

    Nothing enforces this yet — the store takes no inter-process lock
    ([issue #135](https://github.com/LASTRADA-Software/fastcached/issues/135)).
    Converting a store a running daemon has open will corrupt it.

# When a store reports `Corrupt`

[Upgrading a persistent store](upgrading-a-store.md) is about the refusal that
means the store is **fine**. This page is the other one.

`Corrupt` means the bytes on disk did not verify: a page failed its CRC32C, or a
structure that must hold did not. It is the one storage code where "this store is
damaged" is the correct reading — and it is deliberately narrow, so that reading
stays trustworthy.

The first three rows below are the things `Corrupt` is **not**. Each has a code of
its own, and the code — not the message text — is what monitoring should be
reading.

| Code | What happened | What to do |
|---|---|---|
| `UnsupportedFormatVersion` | A healthy store written by another build | [Convert it](upgrading-a-store.md). Never delete it. |
| `MalformedValue` | A client sent a value this build cannot parse | Look at the client. Counted by `fastcached_cache_malformed_values_total`. |
| `InUse` | Another process holds the store open | Stop it, or give this process its own path — see [Deployment](deployment.md). |
| `Corrupt` | The bytes on disk are damaged | Read on. |

!!! warning "`Corrupt` is not automatically a failing disk"

    Until [#296](https://github.com/LASTRADA-Software/fastcached/issues/296) an
    unprivileged client could plant a few bytes, tag them as a set, read them
    back, and make the daemon report `Corrupt` against a store whose every record
    still verified. That path is closed — those decoders answer `MalformedValue`
    now — but the history is why the first move on a `Corrupt` is to look, not to
    reach for a replacement disk.

## Does the process start?

That depends entirely on **where** the damage is, and the two answers are
different events.

Opening a store is not a scan. It reads the two meta slots, walks the free list,
and looks up two reserved keys — the in-flight-conversion marker and the format
marker. Nothing enumerates the records.

**Damage in what `Open` touches: the process refuses to start.** This is an
outage, not a degradation. Both binaries treat a store that will not open as
fatal, on purpose: the operator named a path, and carrying on without it would
silently deliver less than was configured.

```
failed to open storage '/var/lib/fastcached/cache': StorageError(code=Corrupt system=0 context=FilePageStore::Open)
```

```
--cache-dir cannot open /var/cache/fastcache-node/objects.cow: StorageError(code=Corrupt system=0 context=FilePageStore::Open); refusing to start
```

For a compile node this takes the **whole node** down, not just its cache tier.
That is about the store the operator *configured*, not about needing one: a node
started with no tier at all — no `--cache-dir`, and `--cache-memory 0` — serves
compiles perfectly well. What is fatal is a store that was asked for and will not
open, because coming up without it would silently deliver less than was
configured.

So if you need the worker back before you have finished with the file, **start it
without `--cache-dir` and leave the store where it is.** It compiles; it just
stores nothing locally, and the evidence survives.

With `--storage-shards`, `--storage` names a *directory* of `shard-NN.cow` and
each is opened separately, so the message says `failed to open shard '<path>'` and
names the one file that is damaged. Everything below applies to that shard, not to
the directory.

**Damage anywhere else: the process runs, and loses keys.** Nothing notices until
something reads the page the damage landed on. Then:

- a read of an affected key **fails** rather than reporting a miss;
- every key on an undamaged page keeps serving;
- on `fastcached`, a write that cannot be persisted logs `storage write failed` at
  `Warn` and moves `fastcached_write_errors_total`.

That second case is the common one, and it is why the answer below is not "delete
it".

!!! warning "A compile node does not raise the write-error signal"

    `fastcached_write_errors_total` is fed by a decorator only `fastcached` puts
    around its storage. `fastcache-compile-node` does not, so on a node that
    counter is exported and **can never rise**, and no `storage write failed`
    line is ever written. Do not build a node alert on either.

    What a node gives you is the startup refusal above — which is loud, because
    the process exits — and `Corrupt` in the log while it serves. Alert on the
    node being down, and read its log for the code.

!!! note "One damaged meta page is survived silently"

    The store keeps two meta slots and alternates between them, so a write
    interrupted part-way leaves the previous one standing. `Corrupt` at open
    means **both** failed — or the free list, or the marker's own path through
    the tree. The format is built so that an interrupted write is survivable,
    which is what makes hardware or the filesystem the leading hypothesis rather
    than the power cut.

    **Silently means you are told nothing, not that nothing was lost.** Which of
    the two slots was damaged decides what surviving costs. Neither outcome
    writes a log line — the storage layer has no logger to reach for — and the
    recovery itself moves no counter. But do not read that as *no trace at all*:
    when the **live** slot was the damaged one, the keys written after the
    surviving commit become ordinary **misses**, and misses are counted. That
    step is the only signal the event leaves, so it is the one to look at.

    - The **older** slot damaged: the store comes up on the newest commit, whole.
    - The **live** slot damaged: the store comes up on the previous durable
      commit. Keys written after it are simply **not there** — they read as
      ordinary misses, not as errors, because the store is internally
      consistent, just consistent with an earlier moment.

    **Whether ordinary use then repairs the file depends on
    `--storage-durability`, and under `fsync` it does the opposite.** Measured,
    on a store seeded through the cache's own open path:

    - **`batched`** (the default): the next flush writes the *other* slot, so it
      lands in the damaged one and the surviving slot is preserved. Ordinary use
      does repair the file.
    - **`fsync`**: the commit after recovery derives its slot from the
      transaction id and consults nothing the recovery recorded, so the
      **first** one overwrites the surviving slot rather than the damaged one.
      For the length of that one commit the store's only good meta copy is the
      one being rewritten, and a tear there leaves nothing to open. That write
      restores the slot alternation, so the *second* commit repairs the damaged
      slot and the file is back to two good copies. See
      [#726](https://github.com/LASTRADA-Software/fastcached/issues/726).

    So on `fsync` the dangerous moment is the **first write after a degraded
    start**, not the indefinite future. If you are restarting a damaged store to
    get service back, copy the file before you do. A cache that has quietly lost
    its most recent window refills either way; the reason it did is still worth
    the two minutes below, because the damage that caused it has not gone
    anywhere.

## Can it be repaired?

**No. Nothing ships that repairs or salvages a damaged store.** That is the
answer rather than an omission: do not go looking for a tool, and do not read the
conversion flags as one.

`--migrate-storage` and `--migrate-cache` convert a store of an older *vintage*.
They are not a repair, they refuse damage, and pointing them at a damaged store
achieves nothing.

## Deleting it

Deleting the store is safe, and it is the procedure. Nothing in it is unique:

- **For `fastcached`**, you lose the cached entries. Clients miss and repopulate.
- **For a compile node**, you lose that machine's accumulated build output. The
  tier is what the node serves back to **this** machine, so the cost is real —
  every compile that would have hit now runs — but it is time, not data.

There is no correctness consequence either way. A cache holds no last copy of
anything.

## Before you delete it

Two minutes, and it is the difference between a fixed cause and the same outage
next month.

1. **Copy the file somewhere else first.** Deleting it destroys the only evidence
   of what happened. It is usually mostly intact — partial loss is the normal
   shape of this failure, not total loss.
2. **Write down what the log said** — the code, the `context`, and whether it
   appeared at startup or while serving. Those are different faults.
3. **Check the neighbours.** Damage on one store on a machine is a store; damage
   on several is the machine. `dmesg`, the SMART attributes of the device, and
   whether the filesystem was remounted read-only are the three that pay.
4. **Check `fastcached_cache_malformed_values_total`.** If that is what is
   rising, the problem is a client sending nonsense, and it is not `Corrupt`
   at all.

Then delete the store and restart. It refills.

## Alerting

Alert on `Corrupt` and `UnsupportedFormatVersion` **separately**. They arrive
through the same field and call for opposite responses, and an alert that
collapses them is an alert that will eventually tell somebody to delete a healthy
cache.

On `fastcached`, `Corrupt` beside a rising `fastcached_write_errors_total` is the
disk. On a compile node that counter cannot move (see above), so the signal is the
process: `Corrupt` at startup on a node that will not come up is an outage, and
pages accordingly.

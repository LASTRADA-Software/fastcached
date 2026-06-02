# CowTree

A copy-on-write, persistent B+tree library. Standalone — no dependency
on FastCache. Pick it up by `add_subdirectory(src/CowTree)` and link
the `CowTree` target.

## What it provides

- Crash-consistent on-disk key→value store. A power loss at any point
  during a commit leaves the file in either the pre-commit or the
  post-commit state — never a hybrid.
- O(1) snapshot semantics: a `Snapshot()` is just a captured root +
  transaction id; no copying.
- Single-writer / multi-reader. Read transactions overlap with writers.
- Pluggable page backend via `IPageStore`. Two implementations ship:
  - `InMemoryPageStore` — deterministic, with rich failure injection
    for crash-consistency testing.
  - `FilePageStore` — `pread`/`pwrite` + `fsync` (or
    `ReadFile`/`WriteFile` + `FlushFileBuffers` on Windows).

## Commit protocol

```
1. Write all new data pages.
2. SyncData()             — flush data.
3. Encode new Meta (txnId = previous + 1, new root, new freeRoot).
4. WriteMeta(slot = txnId % 2, ...)   — single-page atomic write.
```

Recovery on `Open()` picks the meta slot with the higher `txnId` whose
CRC validates. A torn meta write leaves the previous slot intact, so
the failed transaction simply rolls back.

## Usage

```cpp
#include <CowTree/CowTree.hpp>
#include <CowTree/FilePageStore.hpp>

CowTree::FilePageStore::Options opts;
opts.path = "/var/lib/example/store.cow";
opts.durability = CowTree::FilePageStore::Durability::Batched;

auto store = CowTree::FilePageStore::Open(opts).value();
CowTree::CowTree tree { *store };
tree.Open().value();

// Write.
{
    auto txn = tree.BeginWrite();
    (void) txn.Put(CowTree::AsBytes("hello"), CowTree::AsBytes("world"));
    (void) txn.Commit();
}

// Read.
{
    auto reader = tree.BeginRead();
    auto got = reader.Get(CowTree::AsBytes("hello"));
    if (got.has_value() && got->has_value())
        // *got holds the value bytes.
}
```

## Known limitations (v1)

- No merge on underflow. Erase may leave a page sparse; the tree stays
  correct but grows less dense over time. Compaction is future work.
- Single-page values only. Keys ≤ 1024 bytes, values ≤ 2048 bytes by
  default. Larger values rejected with `ValueTooLarge`.
- Held read snapshots may be invalidated by a subsequent commit that
  reuses pages from the snapshot's chain. The library's primary
  consumer (FastCache) never holds a snapshot across a writer call.
- The on-disk free list is not yet persisted across restarts; freed
  pages remain in the file until a future compaction phase removes
  them.

# Storage primitives

`IStorage` is the atomic-operation interface every backend implements
and every protocol handler ultimately calls. Each method's atomicity
boundary is the keys it touches; concurrent calls on different keys
need not synchronise (and don't, when fronted by `ShardedStorage`).

## The interface

```cpp
class IStorage {
public:
    virtual std::expected<GetResult, StorageError> Get(string_view key, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Set(string_view key, vector<byte> value, uint32_t flags, TimePoint expiry) = 0;
    virtual std::expected<CasToken, StorageError> Add(string_view key, vector<byte> value, uint32_t flags, TimePoint expiry, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Replace(string_view key, vector<byte> value, uint32_t flags, TimePoint expiry, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Append(string_view key, span<byte const> suffix, CasToken expected, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Prepend(string_view key, span<byte const> prefix, CasToken expected, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> CompareAndSwap(string_view key, CasToken expected, vector<byte> value, uint32_t flags, TimePoint expiry, TimePoint now) = 0;
    // magnitude is unsigned; `decrement` picks the direction (a signed delta
    // could not represent magnitudes >= 2^63).
    virtual std::expected<IncrResult, StorageError> IncrementOrInitialize(string_view key, uint64_t magnitude, bool decrement, TimePoint now) = 0;
    virtual std::expected<void, StorageError> Delete(string_view key, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Touch(string_view key, TimePoint newExpiry, TimePoint now) = 0;

    // Non-mutating read: like Get but does not touch lastAccess, LRU position, or hit/miss stats.
    virtual std::expected<GetResult, StorageError> Peek(string_view key, TimePoint now) = 0;
    // Mark the entry stale (meta `md I` / `ms I`) without removing it; optionally refresh expiry.
    virtual std::expected<CasToken, StorageError> MarkStale(string_view key, optional<TimePoint> newExpiry, TimePoint now) = 0;
    // Atomic get-and-touch (memcached gat); defaults to Touch + Get, lock-owning backends override.
    virtual std::expected<GetResult, StorageError> GetAndTouch(string_view key, TimePoint newExpiry, TimePoint now);
    // Atomic delete-if-CAS-matches (meta `md C(token)`); defaults to Peek + Delete.
    virtual std::expected<void, StorageError> CompareAndDelete(string_view key, CasToken expected, TimePoint now);

    virtual void FlushWithGeneration(TimePoint effectiveAt) = 0;
    virtual std::size_t PurgeExpired(TimePoint now) = 0;
    virtual void Resize(std::size_t newMaxBytes) = 0;
    virtual StorageStats Snapshot() const noexcept = 0;
};
```

## Implementations

| Class                | Purpose |
|----------------------|---------|
| `InMemoryLruStorage` | In-memory LRU with a soft byte budget. Single-threaded by contract. |
| `CowTreeStorage`     | Persistent copy-on-write B-tree backing with an in-memory LRU mirror for eviction accounting. |
| `LayeredStorage`     | Two-tier composition: L1 = `InMemoryLruStorage`, L2 = any `IStorage`. Reads hit L1 first, writes are write-through. |
| `ShardedStorage`     | Hash-based sharding across N inner storages. Each shard holds a `std::shared_mutex` for concurrency. |
| `TracingStorage`     | Decorator that emits one Trace log line per call. |

## Wire → primitive mapping

This is the table the protocol handlers follow. For the full per-
operation mapping across all four protocols, see
[Coverage matrix](../protocols/coverage-matrix.md).

| Operation                    | Primitive |
|------------------------------|-----------|
| Store unconditionally        | `Set` |
| Store if absent              | `Add` |
| Store if present             | `Replace` |
| Append / prepend             | `Append` / `Prepend` |
| Compare-and-swap             | `CompareAndSwap` |
| Increment / decrement        | `IncrementOrInitialize` |
| Delete                       | `Delete` |
| Compare-and-delete (meta `md C`) | `CompareAndDelete` |
| Refresh TTL                  | `Touch` |
| Get-and-touch (`gat` / `gats`)   | `GetAndTouch` |
| Mark stale (meta `md I` / `ms I`) | `MarkStale` |
| Drop all entries             | `FlushWithGeneration` |
| Rebudget                     | `Resize` |
| Snapshot stats               | `Snapshot` |

## CacheEntry

Each stored value has the metadata recorded in `CacheEntry`:

| Field         | Purpose |
|---------------|---------|
| `value`       | Payload bytes |
| `flags`       | 32-bit opaque, returned on get |
| `cas`         | 64-bit monotonically increasing CAS token |
| `expiry`      | Absolute steady-clock deadline (or `TimePoint::max`) |
| `generation`  | For `flush_all` — entries older than the storage's live generation are invisible |
| `lastAccess`  | Updated on every successful `Get`; surfaced via meta `l` flag |
| `stale`       | Set by meta `md I` / `ms I`; surfaced via meta `X` response flag |
| `fetched`     | Set once the entry has been returned by a successful `Get`. Drives the `evicted_unfetched` / `expired_unfetched` stats (entries discarded before any client read them). Reset on insertion and on every value-rewriting mutation. |

## StorageStats

The counter set returned by `Snapshot()`. See
[stats command](../commands/memcached/admin/stats.md) for the wire
exposure.

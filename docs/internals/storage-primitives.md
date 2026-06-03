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
    virtual std::expected<CasToken, StorageError> Append(string_view key, span<byte const> suffix, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Prepend(string_view key, span<byte const> prefix, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> CompareAndSwap(string_view key, CasToken expected, vector<byte> value, uint32_t flags, TimePoint expiry, TimePoint now) = 0;
    virtual std::expected<IncrResult, StorageError> IncrementOrInitialize(string_view key, int64_t delta, TimePoint now) = 0;
    virtual std::expected<void, StorageError> Delete(string_view key, TimePoint now) = 0;
    virtual std::expected<CasToken, StorageError> Touch(string_view key, TimePoint newExpiry, TimePoint now) = 0;
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
| Refresh TTL                  | `Touch` |
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

## StorageStats

The counter set returned by `Snapshot()`. See
[stats command](../commands/memcached/admin/stats.md) for the wire
exposure.

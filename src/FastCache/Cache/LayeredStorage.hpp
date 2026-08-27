// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IReclaimLog.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Two-tier storage that fronts a canonical lower tier with an in-memory
/// LRU cache. Reads hit L1 first; on a miss they fall through to L2 and
/// the returned entry is mirrored into L1 with **L2's CAS preserved**, so
/// the in-memory mirror always agrees with the canonical store on
/// identity. Writes are write-through: L2 is mutated first (canonical CAS
/// issued there) and the resulting entry is mirrored into L1 verbatim.
///
/// L1 is held as a concrete `InMemoryLruStorage` (not a polymorphic
/// `IStorage`) so this class can use `InsertVerbatim` to mirror an entry
/// with a pre-chosen CAS — the public `IStorage::Set` always issues a
/// fresh CAS, which would break coherency. Future tiering generalisations
/// can templatise on the L1 type.
///
/// ## Operation semantics
///
/// | Op | Behaviour |
/// |---|---|
/// | `Get`            | L1 first; on L1 miss, try L2 and on hit mirror to L1 preserving L2's CAS. |
/// | `Set`            | L2 first (canonical CAS); mirror to L1 verbatim. |
/// | `Add` / `Replace`| Routed to L2 (the canonical store) for existence checks; mirror result to L1. |
/// | `Append`/`Prepend`/`CompareAndSwap`/`IncrementOrInitialize` | Same — L2 enforces semantics; mirror outcome to L1. |
/// | `Delete`         | L2.Delete; always drop from L1 (best effort). |
/// | `FlushWithGeneration` | Forward to both (each maintains its own gen). |
/// | `PurgeExpired`   | Forward to both; return L2's count (canonical). |
/// | `Snapshot`       | itemCount/bytesUsed/bytesLimit from L2; LayeredStorage tracks its own
/// cmdGet/cmdSet/getHits/getMisses. |
///
/// ## Concurrency
///
/// Not internally thread-safe. Intended to live inside one shard of a
/// `ShardedStorage`, whose per-shard `unique_lock` serialises all calls
/// into this object. Sharding **outside** the layered pair keeps the (L1,
/// L2) atomic unit under a single lock so a `Get` populating L1 from L2
/// or a `Set` writing through both happens without inversion.
class LayeredStorage final: public IStorage
{
  public:
    /// Construct over a concrete in-memory LRU cache (L1) and any IStorage
    /// (L2). Ownership of both is taken.
    /// @param l1Cache In-memory cache; may be configured with its own byte budget.
    /// @param l2Backing Canonical lower-tier storage (typically a `CowTreeStorage`).
    explicit LayeredStorage(std::unique_ptr<InMemoryLruStorage> l1Cache, std::unique_ptr<IStorage> l2Backing) noexcept;

    LayeredStorage(LayeredStorage const&) = delete;
    LayeredStorage(LayeredStorage&&) = delete;
    LayeredStorage& operator=(LayeredStorage const&) = delete;
    LayeredStorage& operator=(LayeredStorage&&) = delete;
    ~LayeredStorage() override = default;

    [[nodiscard]] std::expected<GetResult, StorageError> Get(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            TimePoint expiry) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Add(
        std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Replace(
        std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Append(std::string_view key,
                                                               std::span<std::byte const> suffix,
                                                               CasToken expected,
                                                               TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                CasToken expected,
                                                                TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                       CasToken expected,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       TimePoint expiry,
                                                                       TimePoint now) override;

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                          std::uint64_t magnitude,
                                                                                          bool decrement,
                                                                                          TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Touch(std::string_view key,
                                                              TimePoint newExpiry,
                                                              TimePoint now) override;

    [[nodiscard]] std::expected<GetResult, StorageError> Peek(std::string_view key, TimePoint now) override;

    /// Warm `key` from L2 into the L1 mirror without any client-read side
    /// effect (no hit/miss stat change, no LRU-as-hit promotion). Reuses the
    /// same L2→L1 mirror path as a read-through Get but skips the stat
    /// bookkeeping. See IStorage::Prefetch.
    [[nodiscard]] std::expected<bool, StorageError> Prefetch(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now) override;

    // Explicit compound-op overrides (rather than the IStorage defaults) so
    // the two-tier get-and-touch / compare-and-delete behaviour is spelled
    // out and directly unit-tested. The single-critical-section guarantee is
    // the enclosing ShardedStorage's per-shard lock; on the unwrapped reactor
    // there is no concurrent writer to exclude.
    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now) override;

    /// PERSIST primitive across both tiers. Forwards to L2 (the canonical
    /// store), then refreshes the L1 mirror so a subsequent Get sees the
    /// cleared TTL. See IStorage::ClearExpiry for the return contract.
    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key, TimePoint now) override;

    /// Read-modify-write across both tiers. Reads the canonical entry
    /// from L2 (the inherited default would consult L1's stale mirror),
    /// hands it to the callback, then writes the outcome through L2
    /// and mirrors into L1. The atomicity boundary is the enclosing
    /// ShardedStorage's per-shard lock. Without this override, the
    /// IStorage default would (1) read a possibly-stale L1 mirror and
    /// (2) wipe the entry's TTL because it cannot see UpdateOutcome's
    /// newExpiry-vs-preserve semantics through the L1+L2 mirror dance.
    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;

    /// Route reclaim reporting to **L2 only, and only its expiries**.
    ///
    /// Neither half of that is arbitrary. L1 gets nothing because it is a
    /// mirror: when it drops an entry the key is still in L2 and the very next
    /// read serves it, so reporting it would publish `evicted` for a key that
    /// never left the cache — and dirty every WATCH on it, aborting
    /// transactions over a demotion no client can observe.
    ///
    /// L2's *evictions* are dropped for the mirror image of that reason. `Get`
    /// serves an L1 hit without ever consulting L2, so a key L2 evicted stays
    /// retrievable until L1 drops it too — and L1 is exactly where the key will
    /// still be, because L1 absorbing the hits is what left L2's recency stale
    /// enough to evict it. The alternative, erasing it from L1 to make the
    /// event true, would throw a hot key out of RAM to justify a notification.
    /// So in a layered cache no single tier's eviction is total, and none is
    /// reported.
    ///
    /// L2's expiries **are** reported, because an expiry is total: both tiers
    /// hold the same TTL, so the entry the mirror still has is one it will
    /// refuse on the next lookup for the same reason L2 reclaimed it.
    /// @param log Sink for reclaimed keys, or nullptr to stop reporting.
    void SetReclaimLog(IReclaimLog* log) override;

    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// Both tiers' statistics, unmerged.
    ///
    /// What `Snapshot()` above cannot answer: it reports L2's item count, bytes and
    /// budget with this composite's own hit/miss patched over them, so L1 leaves no
    /// trace beyond its share of the eviction total. An operator asking whether a
    /// node's RAM tier is populated has no other way to find out.
    /// @return L1's entry and L2's, each at the tier that store reports itself as.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override;

    /// Reconfigure the L1 byte budget. L2's budget is its own concern
    /// (typically unbounded for disk-backed tiers).
    /// @param newL1MaxBytes New L1 byte budget.
    void Resize(std::size_t newL1MaxBytes) override;

    /// Test-only access to the inner cache (the L1 mirror).
    [[nodiscard]] InMemoryLruStorage& L1() noexcept
    {
        return *_l1;
    }

    /// Test-only access to the backing tier.
    [[nodiscard]] IStorage& L2() noexcept
    {
        return *_l2;
    }

  private:
    /// Reload `key` from L2 and mirror the result into L1 if found.
    /// @return The L2 GetResult on success (or error from L2).
    [[nodiscard]] std::expected<GetResult, StorageError> LoadFromL2AndMirror(std::string_view key, TimePoint now);

    /// Mirror an entry into L1 with the lower tier's CAS preserved.
    void MirrorIntoL1(std::string_view key, CacheEntry entry);

    /// Drop `key` from L1 regardless of presence. Used when an L2
    /// mutation might invalidate the L1 mirror.
    void DropFromL1(std::string_view key);

    /// Refresh L1 with the canonical entry the lower tier returned from
    /// a Get(key, now) call. Returns the result for the caller to
    /// forward. Caller wraps the L2 IO error into LayeredStorage's
    /// caller-visible error path.
    [[nodiscard]] std::expected<CasToken, StorageError> MirrorL2WriteResult(std::string_view key,
                                                                            CasToken l2Cas,
                                                                            TimePoint now);

    /// The log L2 is actually handed: forwards expiries and swallows evictions.
    ///
    /// A filter rather than a flag on the tier, because "is my eviction total"
    /// is not something a tier can answer about itself — it depends entirely on
    /// what is layered over it. The same `CowTreeStorage` standing alone is
    /// authoritative and reports both.
    class ExpiriesOnlyLog final: public IReclaimLog
    {
      public:
        /// Point the filter at the log to forward to.
        /// @param inner Destination, or nullptr to discard.
        void SetInner(IReclaimLog* inner) noexcept
        {
            _inner = inner;
        }

        void Record(MutationKind kind, std::string_view key) noexcept override;
        void Drain(std::vector<ReclaimedKey>& out) override;
        [[nodiscard]] bool HasPending() const noexcept override;
        [[nodiscard]] bool IsRecording() const noexcept override;

      private:
        IReclaimLog* _inner { nullptr };
    };

    std::unique_ptr<InMemoryLruStorage> _l1;
    std::unique_ptr<IStorage> _l2;
    ExpiriesOnlyLog _l2Log;
    mutable StorageStats _stats;
};

} // namespace FastCache

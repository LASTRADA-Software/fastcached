// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// IStorage that fans every call across N inner shards by `std::hash(key)`.
///
/// Each shard is itself an IStorage and is owned by this object. A
/// `std::shared_mutex` per shard provides per-shard serialisation:
///
/// - Every call into a shard's storage takes an *exclusive* lock on the
///   relevant shard. The wrapped backends (InMemoryLruStorage,
///   CowTreeStorage) mutate state on `Get` and `Snapshot` too (LRU
///   ordering, stats counters, mutable `_stats` member), so a shared
///   lock would race concurrent same-shard operations.
/// - A call to shard N never blocks any operation on a different shard,
///   so distinct keys distributed across shards run in parallel on up to
///   `shards.size()` cores at once. The shared_mutex type is retained
///   in case a future split of Get into Lookup + deferred-promote
///   recovers read parallelism on a single shard.
///
/// Sharding is purely by key, which the cache engine maps to a single
/// shard for each operation. Operations that cross shards by definition
/// (`FlushWithGeneration`, `PurgeExpired`, `Snapshot`) iterate the
/// shards under each shard's appropriate lock.
class ShardedStorage final: public IStorage
{
  public:
    /// Construct over a non-empty vector of inner storages. Each inner
    /// storage becomes one shard. The vector must contain at least one
    /// element.
    /// @param shards Inner storage instances; ownership taken.
    explicit ShardedStorage(std::vector<std::unique_ptr<IStorage>> shards);

    ShardedStorage(ShardedStorage const&) = delete;
    ShardedStorage(ShardedStorage&&) = delete;
    ShardedStorage& operator=(ShardedStorage const&) = delete;
    ShardedStorage& operator=(ShardedStorage&&) = delete;
    ~ShardedStorage() override = default;

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

    /// Warm `key` into its owning shard's in-memory tier under that shard's
    /// exclusive lock. Forwards to the shard storage's Prefetch. See
    /// IStorage::Prefetch.
    [[nodiscard]] std::expected<bool, StorageError> Prefetch(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now) override;

    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now) override;

    /// PERSIST primitive — clear the entry's TTL atomically under the
    /// shard lock. Holds the shard's exclusive lock across the Peek +
    /// Touch sequence so a concurrent SETEX cannot slip in between.
    /// See IStorage::ClearExpiry for the return-value contract.
    [[nodiscard]] std::expected<bool, StorageError> ClearExpiry(std::string_view key, TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Update(
        std::string_view key,
        std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
        TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    PurgeOutcome PurgeExpired(TimePoint now, PurgeBudget budget) override;

    /// Hand `log` to every shard, each under its own exclusive lock.
    ///
    /// One log serves all of them: it is thread-safe, and which shard evicted a
    /// key is not something a keyspace event has anywhere to say.
    /// @param log Sink for reclaimed keys, or nullptr to stop reporting.
    void SetReclaimLog(IReclaimLog* log) override;

    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// Every shard's tiers, summed tier by tier.
    ///
    /// Taken under each shard's exclusive lock, exactly as `Snapshot()` is and for
    /// the same reason: an inner backend's own snapshot writes its `mutable _stats`,
    /// so a shared lock would race a concurrent reader. That lock is also what makes
    /// this callable at all from a thread that does not own the storage -- which is
    /// why the daemon wraps a metrics-enabled backend here and why the compile
    /// node's cache tier does the same.
    /// @return The per-tier sums; a tier no shard has stays absent.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override;

    /// @return Number of shards owned by this storage.
    [[nodiscard]] std::size_t ShardCount() const noexcept
    {
        return _shards.size();
    }

    /// Forward a Resize call to every shard, splitting the budget evenly.
    /// The budget passed here is the total across all shards; each shard
    /// receives `newTotalBytes / ShardCount()`.
    /// @param newTotalBytes New total byte budget (sum across all shards).
    void Resize(std::size_t newTotalBytes) override;

    /// @return Index of the shard responsible for `key`. Useful for tests
    ///         that need to verify hash partitioning.
    [[nodiscard]] std::size_t ShardIndexFor(std::string_view key) const noexcept;

  private:
    /// One shard's storage + its read-write mutex.
    struct Shard
    {
        std::unique_ptr<IStorage> storage;
        mutable std::shared_mutex mu;
    };

    std::vector<std::unique_ptr<Shard>> _shards;

    /// Which shard the next bounded `PurgeExpired` starts on.
    ///
    /// Each shard resumes its own pass from its own cursor, but a budget
    /// smaller than the number of shards is spent before the loop reaches the
    /// far ones -- so without a rotating start, shard 0 would be swept every
    /// cycle and the last shard never. Relaxed because a race between two
    /// concurrent sweeps costs a repeated shard, not a missed one, and this is
    /// not worth a lock of its own.
    std::atomic<std::size_t> _sweepShard { 0 };
};

} // namespace FastCache

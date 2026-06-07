// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/StringHash.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastCache
{

/// In-memory LRU storage. Single-threaded by contract — a reactor instance
/// owns one storage and is the only thread that touches it. CAS and
/// generation logic live here so the cache engine never has to re-derive
/// them.
///
/// Byte budget: bytesUsed = sum of value.size() across live entries. When
/// an insert/replace would push bytesUsed above maxBytes, the LRU tail is
/// evicted until under budget. The key string and per-entry overhead are
/// not counted — the budget is approximate.
class InMemoryLruStorage final: public IStorage
{
  public:
    /// Construct with the given byte budget and per-value size cap.
    /// `maxBytes == 0` disables eviction entirely; `maxValueBytes == 0`
    /// disables the per-value limit. Both default to 0 (useful for unit
    /// tests).
    /// @param maxBytes Soft cap on total value bytes.
    /// @param maxValueBytes Hard cap on a single value's size in bytes; a
    ///                      Set/Add/Replace/CompareAndSwap/Append/Prepend
    ///                      that would exceed it returns
    ///                      StorageErrorCode::ValueTooLarge.
    /// @param lruMode Recency policy. `Approximate` (default) makes `Get`
    ///                shared-read-safe (no promotion on the read path); `Strict`
    ///                promotes on every read and is served under an exclusive
    ///                lock by `ShardedStorage`.
    explicit InMemoryLruStorage(std::size_t maxBytes = 0,
                                std::size_t maxValueBytes = 0,
                                LruMode lruMode = LruMode::Approximate) noexcept;

    [[nodiscard]] std::expected<GetResult, StorageError> Get(std::string_view key, TimePoint now) override;

    /// In `Approximate` mode a shared-locked `Get` performs no structural
    /// mutation, so concurrent reads on one shard are race-free.
    /// @return True in Approximate mode, false in Strict mode.
    [[nodiscard]] bool SupportsSharedRead() const noexcept override
    {
        return _lruMode == LruMode::Approximate;
    }

    void PromoteOnRead(std::string_view key, TimePoint now) override;

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

    [[nodiscard]] std::expected<CasToken, StorageError> MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;

    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// Reconfigure the byte budget at runtime. Used by ConfigReloader on
    /// SIGHUP. Triggers eviction until under the new budget.
    void Resize(std::size_t newMaxBytes) override;

    /// Insert / overwrite an entry verbatim, preserving its `cas`, `flags`,
    /// `expiry`, and `generation` exactly as supplied — no fresh CAS token
    /// is issued. Used by `LayeredStorage` to mirror an entry observed in a
    /// lower tier (canonical CAS) into the in-memory cache. Promotes to the
    /// LRU front, updates `_bytesUsed`, and triggers eviction-to-fit.
    /// @param key   Insertion key.
    /// @param entry Source-of-truth CacheEntry to store as-is.
    void InsertVerbatim(std::string_view key, CacheEntry entry);

    /// Drop the entry under `key` if present. No error if absent. Used by
    /// `LayeredStorage` to keep the L1 mirror in sync with an L2 delete.
    void EraseIfPresent(std::string_view key);

  private:
    struct Node
    {
        std::string key;
        CacheEntry entry;
    };

    using LruList = std::list<Node>;
    using Iterator = LruList::iterator;

    /// Return iterator to the (non-expired, current-generation) entry, or
    /// end() on miss. Mutates the LRU on hits (moves to front).
    Iterator FindAlive(std::string_view key, TimePoint now);

    /// Read-only lookup for the shared (Approximate) read path: locates a live
    /// entry **without** mutating `_lru`/`_index` (no splice, no lazy erase) and
    /// without writing the node, so concurrent shared-locked callers don't race.
    /// Returns nullptr on miss or expiry (expired entries are reclaimed later by
    /// the writer-locked PurgeExpired / a subsequent write).
    /// @param key Lookup key.
    /// @param now Current clock value.
    /// @return Pointer to the live entry, or nullptr.
    [[nodiscard]] CacheEntry const* FindAliveReadOnly(std::string_view key, TimePoint now) const;

    /// Insert a new entry; evicts as needed to stay under the byte budget.
    /// The value bytes are copied into a fresh immutable buffer.
    /// @return CAS token of the inserted entry.
    CasToken InsertNew(std::string key, std::span<std::byte const> value, std::uint32_t flags, TimePoint expiry);

    /// Mutate the existing entry in-place; updates byte accounting and
    /// promotes the entry to the front of the LRU. Bumps CAS. The value bytes
    /// are copied into a fresh immutable buffer (copy-on-write).
    /// @return New CAS token.
    CasToken MutateExisting(Iterator it, std::span<std::byte const> value, std::uint32_t flags, TimePoint expiry);

    /// Evict from the LRU tail until bytesUsed <= maxBytes.
    void EvictToFit();

    /// Erase the entry pointed at by `it`. Updates accounting and stats.
    void EraseAt(Iterator it);

    /// True if a value of `size` bytes exceeds the configured per-value
    /// cap. Always false when the cap is disabled (`_maxValueBytes == 0`).
    /// @param size Candidate value size in bytes.
    /// @return Whether the value is too large to store.
    [[nodiscard]] bool ExceedsValueLimit(std::size_t size) const noexcept
    {
        return _maxValueBytes != 0 && size > _maxValueBytes;
    }

    std::size_t _maxBytes;
    std::size_t _maxValueBytes;
    LruMode _lruMode;
    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    TimePoint _flushEffectiveAt { TimePoint::min() };
    CasToken _nextCas { 1 };

    LruList _lru;
    std::unordered_map<std::string, Iterator, TransparentStringHash, std::equal_to<>> _index;

    mutable StorageStats _stats;

    /// Read-path counters bumped by the shared-locked `Get` (`cmd_get`,
    /// `get_hits`, `get_misses`). Atomic so concurrent reads on one shard don't
    /// race; folded into `_stats` (relaxed) by `Snapshot`. Kept separate from
    /// `_stats` because `StorageStats` is a plain, copyable value type.
    mutable std::atomic<std::uint64_t> _readCmdGet { 0 };
    mutable std::atomic<std::uint64_t> _readGetHits { 0 };
    mutable std::atomic<std::uint64_t> _readGetMisses { 0 };
};

} // namespace FastCache

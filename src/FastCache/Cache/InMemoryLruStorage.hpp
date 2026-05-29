// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
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
    /// Construct with the given byte budget. `maxBytes == 0` disables
    /// eviction entirely (useful for unit tests).
    /// @param maxBytes Soft cap on total value bytes.
    explicit InMemoryLruStorage(std::size_t maxBytes = 0) noexcept;

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
                                                               TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                TimePoint now) override;

    [[nodiscard]] std::expected<CasToken, StorageError> CompareAndSwap(std::string_view key,
                                                                       CasToken expected,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       TimePoint expiry,
                                                                       TimePoint now) override;

    [[nodiscard]] std::expected<IStorage::IncrResult, StorageError> IncrementOrInitialize(std::string_view key,
                                                                                          std::int64_t delta,
                                                                                          TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> Delete(std::string_view key, TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;

    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// Reconfigure the byte budget at runtime. Used by ConfigReloader on
    /// SIGHUP. Triggers eviction until under the new budget.
    void Resize(std::size_t newMaxBytes);

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

    /// Insert a new entry; evicts as needed to stay under the byte budget.
    /// @return CAS token of the inserted entry.
    CasToken InsertNew(std::string key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry);

    /// Mutate the existing entry in-place; updates byte accounting and
    /// promotes the entry to the front of the LRU. Bumps CAS.
    /// @return New CAS token.
    CasToken MutateExisting(Iterator it, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry);

    /// Evict from the LRU tail until bytesUsed <= maxBytes.
    void EvictToFit();

    /// Erase the entry pointed at by `it`. Updates accounting and stats.
    void EraseAt(Iterator it);

    std::size_t _maxBytes;
    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    TimePoint _flushEffectiveAt { TimePoint::min() };
    CasToken _nextCas { 1 };

    /// Transparent hasher so the map can be looked up by string_view without
    /// constructing a temporary std::string.
    struct TransparentStringHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view> {}(sv);
        }
        [[nodiscard]] std::size_t operator()(std::string const& s) const noexcept
        {
            return std::hash<std::string_view> {}(std::string_view { s });
        }
        [[nodiscard]] std::size_t operator()(char const* s) const noexcept
        {
            return std::hash<std::string_view> {}(std::string_view { s });
        }
    };

    LruList _lru;
    std::unordered_map<std::string, Iterator, TransparentStringHash, std::equal_to<>> _index;

    mutable StorageStats _stats;
};

} // namespace FastCache

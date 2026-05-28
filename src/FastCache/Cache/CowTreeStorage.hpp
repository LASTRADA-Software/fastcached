// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <CowTree/CowTree.hpp>
#include <CowTree/FilePageStore.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastCache
{

/// IStorage backed by a persistent CowTree.
///
/// Each call (Set, Add, ...) opens a write transaction, encodes the
/// `CacheEntry` into a flat byte string, commits, and updates an in-
/// memory LRU mirror used purely for eviction accounting. Reads go
/// through `BeginRead` so they never block a writer.
///
/// The LRU mirror is rebuilt on Open by scanning the tree (one
/// passthrough); the encoded entry carries every field needed to
/// reconstruct the in-memory shadow.
///
/// Encoding (little-endian on disk):
/// ```
/// u32 flags
/// u64 cas
/// i64 expiry_us            (steady-clock microseconds; INT64_MAX = never)
/// u64 generation
/// u32 value_len
/// [value bytes]
/// ```
class CowTreeStorage final: public IStorage
{
  public:
    struct Options
    {
        /// Backing file path.
        std::filesystem::path path;

        /// Soft cap on total value bytes held; 0 disables eviction.
        std::size_t maxBytes { 0 };

        /// Durability mode for the page store.
        CowTree::FilePageStore::Durability durability {
            CowTree::FilePageStore::Durability::Batched
        };

        /// Page size for newly created files. Ignored when the file
        /// already exists (its on-disk page size wins). When zero,
        /// CowTreeStorage::Open picks a power-of-two large enough to
        /// hold `maxValueBytes` plus per-entry / page-header overhead.
        std::size_t pageSize { 0 };

        /// Maximum size in bytes of a single cache value (excluding the
        /// 32-byte per-entry header CowTreeStorage adds for metadata).
        /// Set/Add/Replace/Append/Prepend that would exceed this limit
        /// return StorageErrorCode::ValueTooLarge. Default: 1 MiB,
        /// which fits typical sccache compile-cache values.
        std::size_t maxValueBytes { 1 * 1024 * 1024 };
    };

    /// Open or create the storage. Replays existing entries into the
    /// in-memory LRU mirror.
    [[nodiscard]] static std::expected<std::unique_ptr<CowTreeStorage>, StorageError> Open(Options options);

    CowTreeStorage(CowTreeStorage const&) = delete;
    CowTreeStorage(CowTreeStorage&&) = delete;
    CowTreeStorage& operator=(CowTreeStorage const&) = delete;
    CowTreeStorage& operator=(CowTreeStorage&&) = delete;
    ~CowTreeStorage() override;

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

    /// Reconfigure the byte budget at runtime. Evicts as needed.
    void Resize(std::size_t newMaxBytes);

  private:
    explicit CowTreeStorage(Options options) noexcept;

    /// Result of a tree lookup with the encoded entry materialised.
    struct LoadedEntry
    {
        CacheEntry entry;
    };

    /// Load and decode an entry by key (no LRU side-effect).
    [[nodiscard]] std::expected<std::optional<LoadedEntry>, StorageError> LoadEntry(std::string_view key) const;

    /// Persist the entry to the tree.
    [[nodiscard]] std::expected<void, StorageError> StoreEntry(std::string_view key, CacheEntry const& entry);

    /// Erase the entry from the tree.
    [[nodiscard]] std::expected<void, StorageError> EraseEntry(std::string_view key);

    /// Encode a CacheEntry into a flat byte string for tree storage.
    [[nodiscard]] static std::vector<std::byte> Encode(CacheEntry const& entry);

    /// Decode a tree value back into a CacheEntry.
    [[nodiscard]] static std::expected<CacheEntry, StorageError> Decode(CowTree::BytesView raw);

    /// Replay the tree into the LRU mirror at Open.
    [[nodiscard]] std::expected<void, StorageError> Replay();

    /// Promote the key to the front of the LRU (or insert it).
    void TouchOrInsert(std::string_view key, std::size_t valueSize);

    /// Drop the entry from the LRU mirror.
    void EraseFromLru(std::string_view key);

    /// Evict from the LRU tail until bytesUsed <= maxBytes (best effort).
    void EvictToFit();

    Options _options;
    std::unique_ptr<CowTree::FilePageStore> _store;
    std::unique_ptr<CowTree::CowTree> _tree;

    struct LruNode
    {
        std::string key;
        std::size_t bytes { 0 };
    };
    using LruList = std::list<LruNode>;
    using Iterator = LruList::iterator;

    LruList _lru;
    std::unordered_map<std::string, Iterator> _index;

    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    TimePoint _flushEffectiveAt { TimePoint::min() };
    CasToken _nextCas { 1 };
    mutable StorageStats _stats;
};

} // namespace FastCache

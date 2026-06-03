// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/StringHash.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <CowTree/CowTree.hpp>
#include <CowTree/FilePageStore.hpp>

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
/// // --- v2 trailer (optional; absent in legacy files) ---
/// i64 lastAccess_us        (INT64_MIN = never accessed)
/// u8  stale                (0 = live, 1 = stale)
/// ```
/// Older entries (without the v2 trailer) are decoded with `lastAccess`
/// defaulted to `TimePoint::min()` and `stale = false`, so on-disk
/// upgrades are seamless.
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
        CowTree::FilePageStore::Durability durability { CowTree::FilePageStore::Durability::Batched };

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

    // Explicit compound-op overrides (rather than the IStorage defaults) so
    // the get-and-touch / compare-and-delete behaviour of the persistent
    // tier is spelled out and directly unit-tested. The single-critical-
    // section guarantee is provided by the enclosing ShardedStorage's
    // per-shard lock (this tier is never the lock owner); on the unwrapped
    // single-threaded reactor there is no concurrent writer to exclude.
    [[nodiscard]] std::expected<GetResult, StorageError> GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now) override;

    [[nodiscard]] std::expected<void, StorageError> CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now) override;

    void FlushWithGeneration(TimePoint effectiveAt) override;
    std::size_t PurgeExpired(TimePoint now) override;
    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// Reconfigure the byte budget at runtime. Evicts as needed.
    void Resize(std::size_t newMaxBytes) override;

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

    /// Why `TouchOrInsert` is being called, which decides how the LRU
    /// mirror's `fetched` bit is set.
    enum class AccessKind : std::uint8_t
    {
        Write,    ///< Value-rewriting mutation: the entry counts as unread.
        Read,     ///< Client read (`Get`): record that a client has read it.
        Preserve, ///< TTL-only change (`Touch`/`MarkStale`): keep `fetched` as-is.
    };

    /// Promote the key to the front of the LRU (or insert it).
    /// @param key       Entry key.
    /// @param valueSize New byte size to account for the entry.
    /// @param access    `AccessKind::Read` sets the `fetched` bit so the LRU
    ///                  mirror records a client access; `AccessKind::Write`
    ///                  (the default) clears it, since a value-rewriting
    ///                  mutation produces an entry nobody has read yet.
    void TouchOrInsert(std::string_view key, std::size_t valueSize, AccessKind access = AccessKind::Write);

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
        /// True once a client has read this key since it was last written.
        /// In-memory only (not persisted): drives the evicted_unfetched /
        /// expired_unfetched counters and resets on restart along with the
        /// rest of the LRU mirror.
        bool fetched { false };
    };
    using LruList = std::list<LruNode>;
    using Iterator = LruList::iterator;

    LruList _lru;
    std::unordered_map<std::string, Iterator, TransparentStringHash, std::equal_to<>> _index;

    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    TimePoint _flushEffectiveAt { TimePoint::min() };
    CasToken _nextCas { 1 };
    mutable StorageStats _stats;
};

} // namespace FastCache

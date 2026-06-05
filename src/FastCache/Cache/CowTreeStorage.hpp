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
#include <CowTree/IPageStore.hpp>

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
/// The page size is a small fixed value (`DefaultStoragePageSize`), decoupled
/// from `maxValueBytes` — so a tiny write never shuffles a multi-megabyte page.
/// A value larger than `PageSize()/4` is stored out-of-line as a chain of
/// overflow pages; the leaf then holds only a small descriptor.
///
/// Leaf-record encoding (little-endian on disk; format v3):
/// ```
/// u8  kind                 (0 = inline, 1 = overflow)
/// u32 flags
/// u64 cas
/// i64 expiry_us            (steady-clock microseconds; INT64_MAX = never)
/// u64 generation
/// i64 lastAccess_us        (INT64_MIN = never accessed)
/// u8  stale                (0 = live, 1 = stale)
/// -- inline:   u32 value_len ; [value bytes]
/// -- overflow: u64 total_len ; u64 root_page_id
/// ```
/// Each overflow page is `[u64 next_page_id][u32 chunk_len][u32 crc32c][chunk]`
/// (next == 0 marks the last page; the per-page CRC closes the torn-write gap,
/// since data pages otherwise carry no read-time checksum). v3 breaks the older
/// on-disk format (acceptable pre-release).
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

    /// Test seam: open over an injected page store (e.g. an InMemoryPageStore
    /// with fault injection) instead of a FilePageStore on disk. Used by the
    /// crash-consistency tests.
    /// @param options Storage options (path/durability ignored; the store is supplied).
    /// @param store   The page store to drive (ownership transferred).
    [[nodiscard]] static std::expected<std::unique_ptr<CowTreeStorage>, StorageError> OpenWithStore(
        Options options, std::unique_ptr<CowTree::IPageStore> store);

    /// Test seam: open over a BORROWED page store the caller owns and outlives
    /// this object — so a test can reopen a fresh CowTreeStorage over the same
    /// (in-memory) store to simulate a restart after an injected fault.
    /// @param options Storage options (path/durability ignored).
    /// @param store   Borrowed page store; must outlive the returned object.
    [[nodiscard]] static std::expected<std::unique_ptr<CowTreeStorage>, StorageError> OpenBorrowing(
        Options options, CowTree::IPageStore& store);

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

    /// Build the tree over `_store`, replay, and set stats. Shared by the
    /// owning and borrowing Open paths.
    [[nodiscard]] std::expected<void, StorageError> Initialize();

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

    /// A leaf record parsed into its header plus either inline value bounds or
    /// an overflow descriptor (the value itself is materialised separately).
    struct ParsedRecord
    {
        CacheEntry entry;                                 ///< All fields except `value`.
        bool overflow { false };                          ///< True when the value is out-of-line.
        std::uint32_t inlineLen { 0 };                    ///< Inline value length (inline records).
        std::size_t inlineOffset { 0 };                   ///< Offset of the inline value in `raw`.
        std::uint64_t totalLen { 0 };                     ///< Full value length (overflow records).
        CowTree::PageId root { CowTree::PageId::None() }; ///< Overflow chain head.
    };

    /// A lightweight reference to a stored value's out-of-line backing, used to
    /// reclaim the old chain after an overwrite/erase.
    struct StoredRef
    {
        bool overflow { false };
        CowTree::PageId root { CowTree::PageId::None() };
    };

    /// Encode an entry whose value is stored inline in the leaf.
    [[nodiscard]] static std::vector<std::byte> EncodeInline(CacheEntry const& entry);

    /// Encode an entry whose value lives in an overflow chain; the leaf holds
    /// only the descriptor (total length + chain head).
    [[nodiscard]] static std::vector<std::byte> EncodeOverflowDescriptor(CacheEntry const& entry,
                                                                         CowTree::PageId root,
                                                                         std::uint64_t totalLen);

    /// Parse a leaf record's header (everything but the materialised value).
    [[nodiscard]] static std::expected<ParsedRecord, StorageError> ParseRecord(CowTree::BytesView raw);

    /// Write `value` as a chain of overflow pages; returns the chain head.
    [[nodiscard]] std::expected<CowTree::PageId, StorageError> WriteOverflowChain(std::span<std::byte const> value);

    /// Read back an overflow chain into a contiguous buffer (verifying CRCs).
    [[nodiscard]] std::expected<std::vector<std::byte>, StorageError> ReadOverflowChain(CowTree::PageId root,
                                                                                        std::uint64_t totalLen) const;

    /// Free every page of an overflow chain (best effort; in-memory free list).
    void FreeChain(CowTree::PageId root);

    /// Read just the stored descriptor for `key` (no value materialisation), so
    /// an overwrite/erase can reclaim a pre-existing overflow chain.
    [[nodiscard]] std::expected<std::optional<StoredRef>, StorageError> ReadStoredRef(std::string_view key) const;

    /// Value-size boundary at/below which a value is stored inline in the leaf.
    [[nodiscard]] std::size_t InlineValueLimit() const noexcept;

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
    /// Holds the page store when this object owns it (FilePageStore in
    /// production, or an injected store); null when the store is borrowed.
    std::unique_ptr<CowTree::IPageStore> _ownedStore;
    /// The active page store — points at `_ownedStore` or a borrowed store.
    CowTree::IPageStore* _store { nullptr };
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

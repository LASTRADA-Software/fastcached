// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Compression.hpp>
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
/// Leaf-record encoding (little-endian on disk; format v4):
/// ```
/// u8  kind                 (0 = inline, 1 = overflow)
/// u8  codec                (CompressionCodec id: 0 = none, 1 = lz4, 2 = zstd)
/// u32 flags
/// u64 cas
/// i64 expiry_us            (steady-clock microseconds; INT64_MAX = never)
/// u64 generation
/// i64 lastAccess_us        (INT64_MIN = never accessed)
/// u8  stale                (0 = live, 1 = stale)
/// -- inline:   u32 stored_len ; u32 original_len ; [stored bytes]
/// -- overflow: u64 stored_len ; u64 original_len ; u64 root_page_id
/// ```
/// The value bytes are stored **compressed** when `codec != none`; `stored_len`
/// (inline `value_len` / overflow `total_len`) is the on-disk byte count and
/// `original_len` is the pre-compression length used to size the decompress
/// buffer. `codec == none` stores the value verbatim and `original_len ==
/// stored_len`. Each record carries its own codec, so a store may freely mix
/// codecs; changing the configured codec only affects subsequent writes and no
/// migration is ever required.
///
/// Each overflow page is `[u32 crc32c][u64 next_page_id][u32 chunk_len][chunk]`
/// (next == 0 marks the last page). The CRC is computed over everything that
/// follows it — the `next` link, `chunk_len`, and the chunk — so a torn write
/// to the chain linkage (not just the payload) is detected on read; data pages
/// otherwise carry no read-time checksum. v4 breaks the older v3 on-disk
/// format; a store of any other vintage is rejected on Open with
/// `StorageErrorCode::UnsupportedFormatVersion` rather than mis-parsed.
class CowTreeStorage final: public IStorage
{
  public:
    /// On-disk record-layout version this build WRITES. Bumped from 3 to 4 when
    /// the per-entry compression codec + original-length fields were added (see
    /// the layout above). A store carrying any other version is refused on Open
    /// rather than mis-parsed under this layout.
    ///
    /// Public because it is part of the on-disk contract, not an implementation
    /// detail: the diagnostics quote it, and anything that has to build or
    /// inspect a store of a known vintage needs the same number this class
    /// stamps rather than a second copy of it.
    static constexpr std::uint32_t CurrentFormatVersion = 4;

    /// Reserved tree key holding the format-version sentinel, whose value is
    /// `[u32 version]` little-endian.
    ///
    /// Chosen to be unreachable by the memcached-family wire protocols: it
    /// leads with control bytes (NUL + SOH), which those protocols forbid in
    /// keys. A RESP binary key could in principle collide, but a client would
    /// have to store this exact magic; even then only the sentinel read is
    /// affected, never the integrity of other data. Built via an explicit
    /// length so the embedded NULs are not truncated.
    static constexpr char FormatMarkerKeyBytes[] = { '\0', '\1', 'f', 'a', 's', 't', 'c', 'a', 'c',
                                                     'h',  'e',  'd', '.', 'f', 'm', 't', '\0' };
    static constexpr std::string_view FormatMarkerKey { FormatMarkerKeyBytes, sizeof(FormatMarkerKeyBytes) };

    /// Reserved tree key recording an in-flight conversion, whose value is
    /// `[u32 fromVersion][last converted key]`.
    ///
    /// Unreachable by the wire protocols for the same reason as
    /// `FormatMarkerKey`, and separate from it because the two state different
    /// things: the format marker says what the records ARE, this says what they
    /// are part-way through becoming. Its presence is what turns an interrupted
    /// conversion from a store nobody can read into one the next run finishes.
    static constexpr char MigrationMarkerKeyBytes[] = { '\0', '\1', 'f', 'a', 's', 't', 'c', 'a', 'c',
                                                        'h',  'e',  'd', '.', 'm', 'i', 'g', '\0' };
    static constexpr std::string_view MigrationMarkerKey { MigrationMarkerKeyBytes, sizeof(MigrationMarkerKeyBytes) };

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

        /// Codec applied to value bytes before they are written to disk.
        /// Reads always return plaintext (decompression happens on the
        /// on-disk-miss path only). Default: Zstd. A value is only stored
        /// compressed when it is at least `compressionMinBytes` and the
        /// compressed form is actually smaller (see the shrink-check in
        /// StoreEntry); otherwise it is stored verbatim under `Identity`.
        CompressionCodec compression { CompressionCodec::Zstd };

        /// Codec effort level for `compression` (higher = smaller/slower).
        /// Ignored by codecs without a level. Default: zstd level 3.
        int compressionLevel { 3 };

        /// Values smaller than this are never compressed (the CPU cost is not
        /// worth it and tiny values rarely shrink). Default: 256 bytes.
        std::size_t compressionMinBytes { 256 };
    };

    /// What a conversion did, so a caller can say it out loud.
    struct MigrationReport
    {
        /// The version the store carried on the way in.
        std::uint32_t fromVersion { 0 };

        /// The version it carries now — always `CurrentFormatVersion`.
        std::uint32_t toVersion { 0 };

        /// Leaf records rewritten BY THIS RUN. Zero when the store was already
        /// current, which `fromVersion == toVersion` is the reliable way to
        /// test: an empty store of the current version also converts nothing.
        /// A resumed run counts what it finished, not what the interrupted one
        /// had already done — nothing on disk records that total.
        std::uint64_t recordsConverted { 0 };

        /// True when this run picked up an interrupted conversion rather than
        /// starting one. Worth saying out loud: the operator ran the same
        /// command and got a different amount of work than they expected.
        bool resumed { false };
    };

    /// Open or create the storage. Replays existing entries into the
    /// in-memory LRU mirror.
    ///
    /// The backing file is claimed exclusively for the life of the returned
    /// object, so a second process on one path is refused with
    /// StorageErrorCode::InUse rather than left to corrupt it.
    [[nodiscard]] static std::expected<std::unique_ptr<CowTreeStorage>, StorageError> Open(Options options);

    /// Convert the store at `options.path` to the record layout this build
    /// writes, in place.
    ///
    /// OFFLINE, and deliberately not something `Open` does for itself: the
    /// conversion rewrites every leaf record in the store, which on a cache
    /// worth migrating is long enough that a supervisor would time out the
    /// start it was hiding inside. An operator asks for it, once, and watches
    /// it finish.
    ///
    /// **Interruptible, not atomic** — and that is a decision rather than a
    /// shortcoming. Converting in one transaction would be atomic and would
    /// also inflate the file by one page per record per tree level, permanently,
    /// because a CoW commit frees replaced pages only at the commit and the
    /// free list is not persisted (see `MigrationChunkRecords`). So the work
    /// commits in slices, and each slice records how far it got in the same
    /// transaction as the records it converted.
    ///
    /// A run that is interrupted therefore leaves a store that is part-way, and
    /// says so: `Open` refuses it by name rather than mis-parsing it, and
    /// running the conversion again RESUMES from the last committed slice.
    /// Nothing is ever converted twice and nothing is skipped.
    ///
    /// Idempotent: a store already at `CurrentFormatVersion` is reported as
    /// such and not written to at all.
    ///
    /// @param options Storage options; `path` and `pageSize` are used.
    ///                Durability is chosen here rather than taken from the
    ///                caller — see the implementation.
    /// @return What was converted; `UnsupportedFormatVersion` when the store is
    ///         NEWER than this build can read, since there is no converting
    ///         forwards from a layout nothing here knows; `IoError` when the
    ///         path names no store at all.
    [[nodiscard]] static std::expected<MigrationReport, StorageError> Migrate(Options const& options);

    /// `Migrate` over an injected page store, so the conversion can be driven
    /// against a synthesised store of a known vintage rather than only against
    /// a file somebody still has.
    /// @param store Borrowed page store; must outlive the call.
    /// @return As `Migrate`.
    [[nodiscard]] static std::expected<MigrationReport, StorageError> MigrateStore(CowTree::IPageStore& store);

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
    PurgeOutcome PurgeExpired(TimePoint now, PurgeBudget budget) override;

    /// Report this tier's reclaims — TTLs found lapsed during a sweep, and the
    /// LRU tail dropped to stay under the on-disk budget — to `log`.
    ///
    /// This is the authoritative tier of a `LayeredStorage`, so it is the one
    /// that gets the log: when it drops a key, the key is gone.
    /// @param log Sink for reclaimed keys, or nullptr to stop reporting.
    void SetReclaimLog(IReclaimLog* log) override;

    [[nodiscard]] StorageStats Snapshot() const noexcept override;

    /// This tier's statistics, reported as the DISK tier.
    ///
    /// Overridden rather than left to the default, which describes a backend that
    /// is one in-memory tier. A CoW tree that reported itself as memory would put a
    /// node's whole on-disk cache under the label an operator reads as RAM.
    /// @return One entry, at `StorageTier::Disk`.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept override;

    /// Reconfigure the byte budget at runtime. Evicts as needed.
    void Resize(std::size_t newMaxBytes) override;

    /// Whether the backing file is held under an exclusive inter-process claim.
    ///
    /// `std::nullopt` when this storage was opened over a caller-supplied page
    /// store, which need not be a file at all — absent is not zero, and a test
    /// driving an `InMemoryPageStore` has no claim to report rather than a
    /// missing one. Deliberately NOT on `IStorage`: it is a `FilePageStore`
    /// fact, and putting it on the interface would oblige every decorator
    /// (`LayeredStorage`, `ShardedStorage`, `TracingStorage`, ...) to forward
    /// it, which is how a decorator quietly answers for a tier it is not.
    /// @return The claim's state, or nullopt when there is no file behind this.
    [[nodiscard]] std::optional<CowTree::FilePageStore::LockState> StoreLockState() const noexcept;

  private:
    explicit CowTreeStorage(Options options) noexcept;

    /// Build the tree over `_store`, replay, and set stats. Shared by the
    /// owning and borrowing Open paths.
    [[nodiscard]] std::expected<void, StorageError> Initialize();

    /// Validate (or, for a fresh store, stamp) the on-disk record-format
    /// version. A store carrying a marker for a different version — or an
    /// older, pre-marker store that already holds data — is rejected with
    /// StorageErrorCode::UnsupportedFormatVersion so its records are never
    /// mis-parsed under the current layout. An empty store is stamped with
    /// `CurrentFormatVersion`.
    ///
    /// The refusal is NOT `Corrupt`: the store is intact, and telling an
    /// operator otherwise is what makes them delete a cache they could have
    /// kept. A marker too short to hold a version is the one case that really
    /// is damage, and that one keeps `Corrupt`.
    /// @return Empty on success; UnsupportedFormatVersion on a version
    ///         mismatch or a pre-marker store; Corrupt on a damaged marker.
    [[nodiscard]] std::expected<void, StorageError> EnsureFormatVersion();

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

    /// Apply a metadata-only mutation (TTL / stale / lastAccess) to the stored
    /// record for `key`, rewriting ONLY the leaf record and REUSING any existing
    /// overflow chain in place — no chain materialisation and no chain rewrite,
    /// so a touch of a large value is O(record), not O(value). CAS is bumped.
    /// @param key    Entry key.
    /// @param now    Current clock value (drives the existence/expiry check).
    /// @param mutate Callback adjusting the parsed entry's metadata in place.
    /// @return The new CAS token, or KeyNotFound if absent/expired/flushed.
    [[nodiscard]] std::expected<CasToken, StorageError> UpdateRecordMetadata(std::string_view key,
                                                                             TimePoint now,
                                                                             std::function<void(CacheEntry&)> const& mutate);

    /// A leaf record parsed into its header plus either inline value bounds or
    /// an overflow descriptor (the value itself is materialised separately).
    struct ParsedRecord
    {
        CacheEntry entry;                                      ///< All fields except `value`.
        bool overflow { false };                               ///< True when the value is out-of-line.
        CompressionCodec codec { CompressionCodec::Identity }; ///< Codec of the stored bytes.
        std::uint64_t originalLen { 0 };                       ///< Pre-compression value length.
        std::uint32_t inlineLen { 0 };                         ///< Inline STORED (on-disk) length.
        std::size_t inlineOffset { 0 };                        ///< Offset of the inline stored bytes in `raw`.
        std::uint64_t totalLen { 0 };                          ///< Overflow STORED (on-disk) length.
        CowTree::PageId root { CowTree::PageId::None() };      ///< Overflow chain head.
    };

    /// A lightweight reference to a stored value's out-of-line backing, used to
    /// reclaim the old chain after an overwrite/erase.
    struct StoredRef
    {
        bool overflow { false };
        CowTree::PageId root { CowTree::PageId::None() };
    };

    /// One validated overflow page: a copy of its bytes (the store's read
    /// buffer is reused on the next Read), the `next` link, and the chunk
    /// length. Returned only after the per-page CRC and bounds have passed.
    struct OverflowPage
    {
        std::vector<std::byte> bytes;
        std::uint64_t next { 0 };
        std::uint32_t chunkLen { 0 };
    };

    /// Read overflow page `id`, copy it out, and validate its header bounds and
    /// chunk CRC. Returns Corrupt if the page is malformed. Both the read path
    /// and the reclaim path go through this so neither ever trusts a `next`
    /// link from an unverified page.
    [[nodiscard]] std::expected<OverflowPage, StorageError> ReadOverflowPage(CowTree::PageId id) const;

    /// Encode an entry whose value is stored inline in the leaf.
    /// @param entry      Entry metadata (value bytes are ignored; `stored` wins).
    /// @param codec      Codec the `stored` bytes were produced with.
    /// @param stored     The bytes to write inline (possibly compressed).
    /// @param originalLen Pre-compression length of the value.
    [[nodiscard]] static std::vector<std::byte> EncodeInline(CacheEntry const& entry,
                                                             CompressionCodec codec,
                                                             std::span<std::byte const> stored,
                                                             std::uint64_t originalLen);

    /// Encode an entry whose value lives in an overflow chain; the leaf holds
    /// only the descriptor (codec + stored/original length + chain head).
    /// @param entry      Entry metadata (value bytes are ignored).
    /// @param codec      Codec the chain's bytes were produced with.
    /// @param root       Overflow chain head.
    /// @param storedLen  On-disk (compressed) byte count of the chain.
    /// @param originalLen Pre-compression length of the value.
    [[nodiscard]] static std::vector<std::byte> EncodeOverflowDescriptor(CacheEntry const& entry,
                                                                         CompressionCodec codec,
                                                                         CowTree::PageId root,
                                                                         std::uint64_t storedLen,
                                                                         std::uint64_t originalLen);

    /// Parse a leaf record's header (everything but the materialised value)
    /// under the layout this build writes. The serving path calls this
    /// directly rather than through `RecordFormats()`, because a store that
    /// carries any other version never opens (see `EnsureFormatVersion`) and a
    /// per-read table lookup would buy nothing.
    [[nodiscard]] static std::expected<ParsedRecord, StorageError> ParseRecord(CowTree::BytesView raw);

    /// Parse a leaf record written under format v3 — the layout before the
    /// per-entry compression codec and original-length fields existed.
    ///
    /// It fills `codec = Identity` and `originalLen` from the stored length,
    /// which is what makes the conversion format-agnostic: whatever reads an
    /// old record hands back the same `ParsedRecord` the current encoders
    /// consume, so `Migrate` never learns which version it is converting from.
    /// @param raw The encoded leaf record.
    /// @return The parsed record, or Corrupt when it does not decode.
    [[nodiscard]] static std::expected<ParsedRecord, StorageError> ParseRecordV3(CowTree::BytesView raw);

    /// A reader for one historical on-disk record layout.
    using RecordParser = std::expected<ParsedRecord, StorageError> (*)(CowTree::BytesView raw);

    /// One layout this build can still READ.
    struct RecordFormat
    {
        std::uint32_t version;
        RecordParser parse;
    };

    /// Every layout this build can read, oldest first, ending at the one it
    /// writes.
    ///
    /// This table IS the migration policy: a format is convertible exactly
    /// when its reader is still here, so bumping `CurrentFormatVersion`
    /// without adding a row is the decision to throw every existing store
    /// away — and a decision that has to be made by deleting a line is one
    /// somebody makes on purpose. `Migrate` walks it and needs no per-version
    /// code of its own.
    /// @return A view of the static table; never empty.
    [[nodiscard]] static std::span<RecordFormat const> RecordFormats() noexcept;

    /// Does this build still carry a reader for `version`?
    ///
    /// What a refusal's advice turns on: a store older than every remaining
    /// reader cannot be converted, and telling its operator to run the
    /// conversion sends them to a command that refuses them in the same words.
    /// @param version An on-disk format version.
    /// @return True when `RecordFormats()` has a row for it.
    [[nodiscard]] static bool CanRead(std::uint32_t version) noexcept;

    /// Re-encode a parsed record under the current layout.
    ///
    /// An overflow record keeps its chain: only the leaf descriptor is
    /// rewritten, so converting a store never rewrites a value page and never
    /// needs room for a second copy of the data.
    /// @param parsed The record, as some version's reader returned it.
    /// @param raw    The bytes it was parsed from (the inline value lives there).
    /// @return The record under `CurrentFormatVersion`.
    [[nodiscard]] static std::vector<std::byte> ReEncodeRecord(ParsedRecord const& parsed, CowTree::BytesView raw);

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

    /// Where reclaims are reported, or nullptr when nobody routed a log here.
    IReclaimLog* _reclaim { nullptr };

    Options _options;
    /// Holds the page store when this object owns it (FilePageStore in
    /// production, or an injected store); null when the store is borrowed.
    std::unique_ptr<CowTree::IPageStore> _ownedStore;
    /// The active page store — points at `_ownedStore` or a borrowed store.
    CowTree::IPageStore* _store { nullptr };
    std::unique_ptr<CowTree::CowTree> _tree;

    /// What the `FilePageStore` reported about its claim, recorded by `Open`
    /// alone. Empty on the borrowing and injected-store paths, where there is
    /// no file and so nothing to have claimed.
    std::optional<CowTree::FilePageStore::LockState> _storeLockState;

    struct LruNode
    {
        std::string key;
        std::size_t bytes { 0 };
        /// True once a client has read this key since it was last written.
        /// In-memory only (not persisted): drives the evicted_unfetched /
        /// expired_unfetched counters and resets on restart along with the
        /// rest of the LRU mirror.
        bool fetched { false };
        /// The tier's live generation when this node was stamped. Lets the
        /// eviction path tell "memory pressure took a live key" from "a flush
        /// already made this invisible and eviction is just the bookkeeping",
        /// without a disk read to fetch the stored entry's own generation.
        std::uint64_t generation { 0 };
    };
    using LruList = std::list<LruNode>;
    using Iterator = LruList::iterator;

    /// Drop the mirror node `it` names: byte accounting, index, list.
    ///
    /// The single erase point for the mirror, which `_sweepCursor` requires:
    /// it is the one iterator that outlives the call which produced it, so a
    /// second place that erased a node would be a second place that could
    /// leave the cursor dangling -- and the one to forget the fix-up would be
    /// whichever is written next.
    /// @param it Mirror node to drop. Must be dereferenceable.
    void EraseNode(Iterator it);

    LruList _lru;

    /// Where the next bounded `PurgeExpired` resumes. See the identically
    /// named member on `InMemoryLruStorage` for why a sweep needs one; this
    /// tier additionally pays a disk read per entry examined, so the budget
    /// that cursor serves matters more here, not less.
    ///
    /// Declared after `_lru` so the default member initialiser below reads an
    /// already-constructed list.
    Iterator _sweepCursor { _lru.end() };

    std::unordered_map<std::string, Iterator, TransparentStringHash, std::equal_to<>> _index;

    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    TimePoint _flushEffectiveAt { TimePoint::min() };
    CasToken _nextCas { 1 };
    mutable StorageStats _stats;
};

/// One line telling an operator what a conversion did to one store.
///
/// Both binaries that expose the conversion print the same sentence, so it is
/// written once. Two copies of an operator-facing string diverge the first time
/// either is improved, and an operator running `fastcache-compile-node
/// --migrate-cache` after `fastcached --migrate-storage` should not have to work
/// out whether two differently-worded lines mean the same thing.
///
/// Takes the whole outcome rather than a report, because the failure line is
/// half of what has to be said and the callers differ only in which stream they
/// send it to.
/// @param path    The store the conversion acted on.
/// @param outcome What `CowTreeStorage::Migrate` returned for it.
/// @return The line, without a trailing newline and without a program prefix.
[[nodiscard]] std::string DescribeMigration(std::filesystem::path const& path,
                                            std::expected<CowTreeStorage::MigrationReport, StorageError> const& outcome);

} // namespace FastCache

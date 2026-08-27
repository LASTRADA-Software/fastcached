// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Compression.hpp>
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
///
/// **Optional in-memory compression.** With a codec configured, values are held
/// compressed and the budget counts their *compressed* size, so the tier holds far
/// more entries per byte of RAM. Compile-cache objects built with embedded debug
/// info compress ~4.5x (measured on 400 MB of real `/Z7` objects), turning a
/// 27 GB working set into ~6 GB. The cost is a decompress on every read —
/// ~3.5 ms for a 3.3 MB object, against the ~200 ms a cache hit already takes —
/// so it is opt-in and off by default for latency-sensitive workloads.
class InMemoryLruStorage final: public IStorage
{
  public:
    /// In-memory compression settings. `codec == Identity` (the default) stores
    /// values verbatim and keeps reads allocation-free.
    struct CompressionOptions
    {
        CompressionCodec codec { CompressionCodec::Identity };
        int level { 3 };               ///< Codec effort; 3 is zstd's speed/ratio knee.
        std::size_t minBytes { 4096 }; ///< Below this, compression rarely pays for itself.
    };

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

    /// Enable in-memory compression.
    ///
    /// Safe to call at any time: whether a value is compressed is recorded per
    /// entry (in storage-private node state, not in `CacheEntry`), so values stored
    /// under an earlier setting stay readable and small values may be kept verbatim.
    /// @param options Codec, level and minimum size.
    void SetCompression(CompressionOptions const& options) noexcept
    {
        _compression = options;
    }

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

    /// Report this tier's reclaims — a lapsed TTL noticed during a lookup or a
    /// sweep, and the LRU tail dropped to stay under budget — to `log`.
    ///
    /// Off unless something routes a log here, which is what keeps a tier
    /// nobody subscribes to free of the per-victim key copy. Note that an
    /// instance used as a `LayeredStorage` L1 mirror must NOT be given one: its
    /// evictions are demotions, and the key is still in L2.
    /// @param log Sink for reclaimed keys, or nullptr to stop reporting.
    void SetReclaimLog(IReclaimLog* log) override;

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

        /// Codec the stored bytes are in. `Identity` means `entry.value` already
        /// holds plaintext — the case for small values and for a tier with
        /// compression disabled. Kept here rather than in `CacheEntry` so the
        /// compression scheme stays private to this tier and does not widen a
        /// struct that every protocol handler constructs.
        CompressionCodec storedCodec { CompressionCodec::Identity };

        /// Plaintext length, needed to size the decompression buffer. Meaningful
        /// only when `storedCodec != Identity`.
        std::size_t plainSize { 0 };
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

    /// Read-only lookup that also reports the node's codec state, so a caller can
    /// decompress. Returns nullptr on miss.
    /// @param key The key to find.
    /// @param now Current time, for expiry.
    /// @return The node, or nullptr.
    [[nodiscard]] Node const* FindNodeReadOnly(std::string_view key, TimePoint now) const;

    /// Compress `value` when the configured codec applies, else return it verbatim.
    /// @param value      Plaintext bytes.
    /// @param storedCodec [out] Codec the returned bytes are in.
    /// @param plainSize   [out] Original plaintext length.
    /// @return The bytes to store.
    [[nodiscard]] SharedValue EncodeForStorage(std::span<std::byte const> value,
                                               CompressionCodec& storedCodec,
                                               std::size_t& plainSize) const;

    /// Materialize a node's plaintext entry, decompressing when needed.
    /// @param node The node to read.
    /// @return A copy of the entry whose value is plaintext.
    [[nodiscard]] CacheEntry PlaintextEntry(Node const& node) const;

    /// A node's value as plaintext bytes, for the in-place mutators (append,
    /// prepend, incr/decr) that must read the current value before rewriting it.
    /// Empty when a compressed value cannot be decoded.
    /// @param node The node to read.
    /// @return The plaintext bytes.
    [[nodiscard]] std::vector<std::byte> PlaintextBytes(Node const& node) const;

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

    /// Where reclaims are reported, or nullptr when nobody routed a log here.
    IReclaimLog* _reclaim { nullptr };

    std::size_t _maxBytes;
    std::size_t _maxValueBytes;
    LruMode _lruMode;
    CompressionOptions _compression {};
    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    TimePoint _flushEffectiveAt { TimePoint::min() };
    CasToken _nextCas { 1 };

    LruList _lru;
    std::unordered_map<std::string, Iterator, TransparentStringHash, std::equal_to<>> _index;

    mutable StorageStats _stats;

    /// Cache-line stride used to keep independently-bumped counters apart.
    /// Spelled as a constant rather than `std::hardware_destructive_-
    /// interference_size` because that constant makes GCC emit
    /// `-Winterference-size` unless the build pins `-mtune`, and this project
    /// builds with warnings as errors.
    static constexpr std::size_t CacheLineBytes = 64;

    /// Read-path counters bumped by the shared-locked `Get`; folded into
    /// `_stats` (relaxed) by `Snapshot`. Atomic because `Approximate` mode
    /// serves `Get` under a *shared* lock, so several threads may be inside it
    /// on one shard at once. Kept out of `_stats` because `StorageStats` is a
    /// plain, copyable value type.
    ///
    /// Only hits and misses are stored — `cmd_get` is their sum. Counting it
    /// separately meant a second locked read-modify-write on every lookup for a
    /// number that was already derivable, and that cost was measurable: with
    /// the third counter in place the `Approximate` read path benchmarked
    /// *slower* than the `Strict` one, which does strictly more work (it splices
    /// the LRU on every read) but bumps plain non-atomic members.
    ///
    /// A lookup is either a hit or a miss, never both, so the two counters sit
    /// on separate cache lines: sharing one would make concurrent readers on a
    /// single shard contend for that line on any mixed workload.
    alignas(CacheLineBytes) mutable std::atomic<std::uint64_t> _readGetHits { 0 };
    alignas(CacheLineBytes) mutable std::atomic<std::uint64_t> _readGetMisses { 0 };
};

} // namespace FastCache

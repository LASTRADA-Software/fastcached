// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

/// Durability policy for the append-only log.
enum class Durability : std::uint8_t
{
    /// fsync (or FlushFileBuffers) after every write. Slowest, safest.
    Fsync,
    /// Buffer writes; periodic batched flushes. Default — sccache cares
    /// about throughput, not the last few ms of cache.
    Batched,
    /// No flushing — OS write cache only. Fastest, least safe.
    None,
};

/// On-disk persistence layer. Append-only log with CRC32C per record;
/// torn writes are detected at startup and the log is truncated at the
/// first bad record. All live entries are loaded into memory at startup;
/// reads serve from memory and writes append to the log before returning.
/// Compaction (atomic file swap) keeps the log bounded over time.
///
/// The on-disk representation is hand-rolled (no external storage library
/// pulled in); see DiskStorage.cpp for the record format.
class DiskStorage final: public IStorage
{
  public:
    struct Options
    {
        /// Filesystem path of the append-only log file.
        std::filesystem::path logPath;

        /// Soft cap on total value bytes held in memory (0 disables eviction).
        std::size_t maxBytes { 0 };

        /// Durability policy.
        Durability durability { Durability::Batched };
    };

    /// Open or create the log at the configured path. Replays any existing
    /// records into in-memory state; on bad CRC, truncates the log at the
    /// start of the offending record (rolling back partial writes from a
    /// previous crash).
    /// @param options Storage options.
    /// @return Owning DiskStorage on success; StorageError on I/O failure.
    [[nodiscard]] static std::expected<std::unique_ptr<DiskStorage>, StorageError> Open(Options options);

    DiskStorage(DiskStorage const&) = delete;
    DiskStorage(DiskStorage&&) = delete;
    DiskStorage& operator=(DiskStorage const&) = delete;
    DiskStorage& operator=(DiskStorage&&) = delete;
    ~DiskStorage() override;

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

    /// Compact the log: write a new file containing only live entries,
    /// fsync it, atomically rename over the old file. Safe to invoke at
    /// any time; concurrent reads/writes are NOT safe (single-threaded
    /// contract per IStorage).
    /// @return Number of records written to the new log.
    [[nodiscard]] std::expected<std::size_t, StorageError> Compact();

    /// Reconfigure the byte budget at runtime.
    void Resize(std::size_t newMaxBytes);

    /// @return Current durability mode.
    [[nodiscard]] Durability DurabilityMode() const noexcept
    {
        return _options.durability;
    }

    /// Change durability at runtime (for SIGHUP reload).
    void SetDurability(Durability mode) noexcept
    {
        _options.durability = mode;
    }

  private:
    /// On-disk record kinds.
    enum class RecordType : std::uint8_t
    {
        Set = 1,
        Delete = 2,
        Flush = 3,
    };

    struct Node
    {
        std::string key;
        CacheEntry entry;
    };

    using LruList = std::list<Node>;
    using Iterator = LruList::iterator;

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
    };

    explicit DiskStorage(Options options) noexcept;

    /// Open the log file at _options.logPath, replay existing records,
    /// truncate at the first bad record.
    [[nodiscard]] std::expected<void, StorageError> OpenAndReplay();

    /// Append a record to the open log file. Optionally syncs based on
    /// durability mode.
    [[nodiscard]] std::expected<void, StorageError> AppendRecord(RecordType type,
                                                                 std::string_view key,
                                                                 CacheEntry const& entry);

    /// Append a tombstone for `key`.
    [[nodiscard]] std::expected<void, StorageError> AppendDelete(std::string_view key);

    /// Append a flush marker bumping the generation counter.
    [[nodiscard]] std::expected<void, StorageError> AppendFlush();

    /// Sync the log to disk respecting durability mode.
    [[nodiscard]] std::expected<void, StorageError> SyncIfRequired();

    Iterator FindAlive(std::string_view key, TimePoint now);
    void EraseAt(Iterator it);
    void EvictToFit();

    /// Apply a Set record to in-memory state (used by both replay and
    /// fresh writes). Does not append to the log.
    CasToken ApplySet(std::string key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, CasToken cas);

    /// Apply a Delete record to in-memory state.
    void ApplyDelete(std::string_view key);

    /// Apply a Flush record (bump live generation).
    void ApplyFlush();

    Options _options;

    // Native log file handle; we use a C FILE* so we can fflush+fsync
    // portably enough for MVP.
    std::FILE* _log { nullptr };

    std::size_t _bytesUsed { 0 };
    std::uint64_t _liveGeneration { 1 };
    CasToken _nextCas { 1 };

    LruList _lru;
    std::unordered_map<std::string, Iterator, TransparentStringHash, std::equal_to<>> _index;

    mutable StorageStats _stats;
};

} // namespace FastCache

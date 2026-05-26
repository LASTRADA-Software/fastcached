// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/DiskStorage.hpp>

#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Crc32c.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <io.h>
    #define FASTCACHED_FSYNC(fd) _commit(fd)
    #define FASTCACHED_FILENO(fp) _fileno(fp)
#else
    #include <unistd.h>
    #define FASTCACHED_FSYNC(fd) ::fsync(fd)
    #define FASTCACHED_FILENO(fp) ::fileno(fp)
#endif

namespace FastCache
{

namespace
{

    constexpr std::array<char, 4> Magic = { 'F', 'C', 'A', 'S' };
    constexpr std::uint32_t FormatVersion = 1;

    /// File header: magic + version + reserved padding.
    /// Layout (16 bytes total):
    ///   bytes 0..3:  'F' 'C' 'A' 'S'
    ///   bytes 4..7:  format version (u32 little-endian)
    ///   bytes 8..15: reserved (zero)
    constexpr std::size_t HeaderSize = 16;

    /// Per-record on-disk layout (little-endian):
    ///   u32 total_len     (size in bytes of payload+crc, not counting these 4 bytes)
    ///   u32 crc32c        (over the payload only)
    ///   u8  type          (1=Set, 2=Delete, 3=Flush)
    ///   u32 flags
    ///   u64 cas
    ///   i64 expiry_us     (microseconds since steady_clock epoch; INT64_MAX = never)
    ///   u64 generation
    ///   u32 key_len
    ///   u32 value_len
    ///   key (key_len bytes)
    ///   value (value_len bytes)
    constexpr std::size_t RecordHeaderSize = 1 + 4 + 8 + 8 + 8 + 4 + 4;

    [[nodiscard]] StorageError MakeIoError(int code, std::string ctx)
    {
        return StorageError {
            .code = StorageErrorCode::IoError, .systemCode = code, .context = std::move(ctx),
        };
    }

    [[nodiscard]] StorageError MakeCorrupt(std::string ctx)
    {
        return StorageError {
            .code = StorageErrorCode::Corrupt, .systemCode = 0, .context = std::move(ctx),
        };
    }

    template <typename T>
    void PushLe(std::vector<std::byte>& buf, T value) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>);
        std::array<std::byte, sizeof(T)> tmp {};
        std::memcpy(tmp.data(), &value, sizeof(T));
        buf.insert(buf.end(), tmp.begin(), tmp.end());
    }

    template <typename T>
    [[nodiscard]] bool ReadTrivial(std::span<std::byte const>& src, T& out) noexcept
    {
        if (src.size() < sizeof(T))
            return false;
        std::memcpy(&out, src.data(), sizeof(T));
        src = src.subspan(sizeof(T));
        return true;
    }

    /// Convert a TimePoint to microseconds since steady_clock epoch (stable
    /// across the same process run; not portable across reboots, but we
    /// only need it consistent for replay within the current execution).
    /// On replay after a reboot, expiry semantics shift — we accept this
    /// for MVP since sccache TTLs are typically short.
    [[nodiscard]] std::int64_t TimePointToMicros(TimePoint tp) noexcept
    {
        if (tp == TimePoint::max())
            return std::numeric_limits<std::int64_t>::max();
        return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    }

    [[nodiscard]] TimePoint MicrosToTimePoint(std::int64_t micros) noexcept
    {
        if (micros == std::numeric_limits<std::int64_t>::max())
            return TimePoint::max();
        return TimePoint { std::chrono::microseconds { micros } };
    }

    [[nodiscard]] bool IsAlive(CacheEntry const& entry, std::uint64_t liveGen, TimePoint now) noexcept
    {
        if (entry.generation < liveGen)
            return false;
        if (entry.expiry <= now)
            return false;
        return true;
    }

} // namespace

DiskStorage::DiskStorage(Options options) noexcept: _options { std::move(options) } {}

DiskStorage::~DiskStorage()
{
    if (_log)
    {
        std::fflush(_log);
        std::fclose(_log);
        _log = nullptr;
    }
}

std::expected<std::unique_ptr<DiskStorage>, StorageError> DiskStorage::Open(Options options)
{
    std::unique_ptr<DiskStorage> storage { new DiskStorage { std::move(options) } };
    auto const result = storage->OpenAndReplay();
    if (!result.has_value())
        return std::unexpected(result.error());
    return storage;
}

std::expected<void, StorageError> DiskStorage::OpenAndReplay()
{
    namespace fs = std::filesystem;

    auto const& path = _options.logPath;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec); // best-effort

    auto const exists = fs::exists(path, ec);

    // Read existing log first (if any), then truncate at first corrupt
    // record and reopen for append.
    std::vector<std::byte> raw;
    std::uintmax_t goodPrefix = 0;

    if (exists)
    {
        auto* fp = std::fopen(path.string().c_str(), "rb");
        if (!fp)
            return std::unexpected(MakeIoError(errno, "fopen for replay"));

        // Slurp the whole file. For sccache-style use the log is bounded
        // by compaction; if it grows unbounded the user can compact.
        std::fseek(fp, 0, SEEK_END);
        auto const len = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        raw.resize(static_cast<std::size_t>(len));
        if (len > 0)
        {
            auto const got = std::fread(raw.data(), 1, raw.size(), fp);
            if (got != raw.size())
            {
                std::fclose(fp);
                return std::unexpected(MakeIoError(errno, "fread on replay"));
            }
        }
        std::fclose(fp);

        // Validate header.
        if (raw.size() < HeaderSize)
        {
            // File too short — treat as empty, will rewrite header.
            raw.clear();
        }
        else
        {
            if (std::memcmp(raw.data(), Magic.data(), Magic.size()) != 0)
                return std::unexpected(MakeCorrupt("bad magic"));
            std::uint32_t version = 0;
            std::memcpy(&version, raw.data() + 4, sizeof(version));
            if (version != FormatVersion)
                return std::unexpected(MakeCorrupt(std::format("unsupported version {}", version)));
        }

        // Replay records.
        std::span<std::byte const> cursor { raw.data() + (raw.empty() ? 0 : HeaderSize),
                                            raw.empty() ? 0 : raw.size() - HeaderSize };
        goodPrefix = raw.empty() ? 0 : HeaderSize;

        while (!cursor.empty())
        {
            std::uint32_t totalLen = 0;
            std::uint32_t storedCrc = 0;
            if (!ReadTrivial(cursor, totalLen))
                break; // truncated header
            if (!ReadTrivial(cursor, storedCrc))
                break;

            if (totalLen > cursor.size())
                break; // truncated body

            auto const payload = cursor.first(totalLen);
            cursor = cursor.subspan(totalLen);

            auto const actualCrc = Crc32c::Compute(payload);
            if (actualCrc != storedCrc)
                break; // first bad record — stop here, truncate below

            // Parse payload.
            auto p = payload;
            std::uint8_t typeByte = 0;
            std::uint32_t flags = 0;
            CasToken cas = 0;
            std::int64_t expiryMicros = 0;
            std::uint64_t generation = 0;
            std::uint32_t keyLen = 0;
            std::uint32_t valueLen = 0;

            if (!ReadTrivial(p, typeByte) || !ReadTrivial(p, flags) || !ReadTrivial(p, cas)
                || !ReadTrivial(p, expiryMicros) || !ReadTrivial(p, generation) || !ReadTrivial(p, keyLen)
                || !ReadTrivial(p, valueLen))
                break;

            if (p.size() < std::size_t { keyLen } + valueLen)
                break;

            auto const keyBytes = p.first(keyLen);
            auto const valueBytes = p.subspan(keyLen, valueLen);
            std::string keyStr;
            keyStr.reserve(keyLen);
            for (auto const b : keyBytes)
                keyStr.push_back(static_cast<char>(b));

            switch (static_cast<RecordType>(typeByte))
            {
                case RecordType::Set:
                {
                    std::vector<std::byte> valueVec { valueBytes.begin(), valueBytes.end() };
                    ApplySet(std::move(keyStr), std::move(valueVec), flags, MicrosToTimePoint(expiryMicros), cas);
                    _nextCas = std::max<CasToken>(_nextCas, cas + 1);
                    _liveGeneration = std::max<std::uint64_t>(_liveGeneration, generation);
                    break;
                }
                case RecordType::Delete: ApplyDelete(keyStr); break;
                case RecordType::Flush:
                    ApplyFlush();
                    _liveGeneration = std::max<std::uint64_t>(_liveGeneration, generation);
                    break;
                default: break; // unknown record type — stop replay
            }

            // Advance the "good prefix" marker so corruption truncation
            // happens at the right offset.
            goodPrefix += sizeof(std::uint32_t) + sizeof(std::uint32_t) + totalLen;
        }
    }

    // (Re)open in append mode, truncating to goodPrefix if needed.
    if (exists && raw.size() != goodPrefix)
    {
        std::error_code rc;
        fs::resize_file(path, goodPrefix, rc);
        if (rc)
            return std::unexpected(MakeIoError(rc.value(), "truncate to good prefix"));
    }

    _log = std::fopen(path.string().c_str(), exists && goodPrefix > 0 ? "rb+" : "wb+");
    if (!_log)
        return std::unexpected(MakeIoError(errno, "fopen for append"));

    if (!exists || goodPrefix == 0)
    {
        // Write header.
        std::array<std::byte, HeaderSize> hdr {};
        std::memcpy(hdr.data(), Magic.data(), Magic.size());
        std::memcpy(hdr.data() + 4, &FormatVersion, sizeof(FormatVersion));
        if (std::fwrite(hdr.data(), 1, hdr.size(), _log) != hdr.size())
            return std::unexpected(MakeIoError(errno, "fwrite header"));
        std::fflush(_log);
        if (_options.durability == Durability::Fsync)
            FASTCACHED_FSYNC(FASTCACHED_FILENO(_log));
    }
    else
    {
        std::fseek(_log, 0, SEEK_END);
    }

    EvictToFit();
    return {};
}

DiskStorage::Iterator DiskStorage::FindAlive(std::string_view key, TimePoint now)
{
    auto const it = _index.find(key);
    if (it == _index.end())
        return _lru.end();
    auto const nodeIt = it->second;
    if (!IsAlive(nodeIt->entry, _liveGeneration, now))
    {
        EraseAt(nodeIt);
        return _lru.end();
    }
    _lru.splice(_lru.begin(), _lru, nodeIt);
    return nodeIt;
}

void DiskStorage::EraseAt(Iterator it)
{
    _bytesUsed -= it->entry.value.size();
    _index.erase(it->key);
    _lru.erase(it);
}

void DiskStorage::EvictToFit()
{
    if (_options.maxBytes == 0)
        return;
    while (_bytesUsed > _options.maxBytes && !_lru.empty())
    {
        EraseAt(std::prev(_lru.end()));
        ++_stats.evictions;
    }
}

CasToken DiskStorage::ApplySet(std::string key,
                               std::vector<std::byte> value,
                               std::uint32_t flags,
                               TimePoint expiry,
                               CasToken cas)
{
    auto const indexIt = _index.find(key);
    if (indexIt != _index.end())
    {
        auto nodeIt = indexIt->second;
        _bytesUsed -= nodeIt->entry.value.size();
        nodeIt->entry.value = std::move(value);
        nodeIt->entry.flags = flags;
        nodeIt->entry.expiry = expiry;
        nodeIt->entry.generation = _liveGeneration;
        nodeIt->entry.cas = cas;
        _bytesUsed += nodeIt->entry.value.size();
        _lru.splice(_lru.begin(), _lru, nodeIt);
    }
    else
    {
        Node node;
        node.key = std::move(key);
        node.entry.value = std::move(value);
        node.entry.flags = flags;
        node.entry.cas = cas;
        node.entry.expiry = expiry;
        node.entry.generation = _liveGeneration;
        _bytesUsed += node.entry.value.size();
        _lru.push_front(std::move(node));
        _index.emplace(_lru.front().key, _lru.begin());
    }
    EvictToFit();
    return cas;
}

void DiskStorage::ApplyDelete(std::string_view key)
{
    auto const it = _index.find(key);
    if (it == _index.end())
        return;
    EraseAt(it->second);
}

void DiskStorage::ApplyFlush()
{
    ++_liveGeneration;
}

std::expected<void, StorageError>
DiskStorage::AppendRecord(RecordType type, std::string_view key, CacheEntry const& entry)
{
    std::vector<std::byte> payload;
    payload.reserve(RecordHeaderSize + key.size() + entry.value.size());

    PushLe<std::uint8_t>(payload, static_cast<std::uint8_t>(type));
    PushLe<std::uint32_t>(payload, entry.flags);
    PushLe<std::uint64_t>(payload, entry.cas);
    PushLe<std::int64_t>(payload, TimePointToMicros(entry.expiry));
    PushLe<std::uint64_t>(payload, entry.generation);
    PushLe<std::uint32_t>(payload, static_cast<std::uint32_t>(key.size()));
    PushLe<std::uint32_t>(payload, static_cast<std::uint32_t>(entry.value.size()));
    for (auto const c : key)
        payload.push_back(static_cast<std::byte>(c));
    for (auto const b : entry.value)
        payload.push_back(b);

    auto const totalLen = static_cast<std::uint32_t>(payload.size());
    auto const crc = Crc32c::Compute(std::span<std::byte const> { payload.data(), payload.size() });

    if (std::fwrite(&totalLen, 1, sizeof(totalLen), _log) != sizeof(totalLen))
        return std::unexpected(MakeIoError(errno, "fwrite totalLen"));
    if (std::fwrite(&crc, 1, sizeof(crc), _log) != sizeof(crc))
        return std::unexpected(MakeIoError(errno, "fwrite crc"));
    if (std::fwrite(payload.data(), 1, payload.size(), _log) != payload.size())
        return std::unexpected(MakeIoError(errno, "fwrite payload"));

    return SyncIfRequired();
}

std::expected<void, StorageError> DiskStorage::AppendDelete(std::string_view key)
{
    CacheEntry dummy {};
    dummy.cas = _nextCas++;
    dummy.generation = _liveGeneration;
    return AppendRecord(RecordType::Delete, key, dummy);
}

std::expected<void, StorageError> DiskStorage::AppendFlush()
{
    CacheEntry dummy {};
    dummy.cas = _nextCas++;
    dummy.generation = _liveGeneration;
    return AppendRecord(RecordType::Flush, std::string_view {}, dummy);
}

std::expected<void, StorageError> DiskStorage::SyncIfRequired()
{
    if (_options.durability == Durability::Fsync)
    {
        std::fflush(_log);
        if (FASTCACHED_FSYNC(FASTCACHED_FILENO(_log)) != 0)
            return std::unexpected(MakeIoError(errno, "fsync"));
    }
    else if (_options.durability == Durability::Batched)
    {
        std::fflush(_log); // OS buffer only; no fsync.
    }
    // Durability::None — nothing.
    return {};
}

// -- IStorage overrides ----------------------------------------------------

std::expected<GetResult, StorageError> DiskStorage::Get(std::string_view key, TimePoint now)
{
    ++_stats.cmdGet;
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
    {
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    ++_stats.getHits;
    return GetResult { .found = true, .entry = it->entry };
}

std::expected<CasToken, StorageError>
DiskStorage::Set(std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry)
{
    ++_stats.cmdSet;
    auto const cas = _nextCas++;
    auto const newCas = ApplySet(std::string { key }, value, flags, expiry, cas);
    auto const& nodeIt = _index.find(key)->second;
    auto const result = AppendRecord(RecordType::Set, key, nodeIt->entry);
    if (!result.has_value())
        return std::unexpected(result.error());
    return newCas;
}

std::expected<CasToken, StorageError> DiskStorage::Add(std::string_view key,
                                                       std::vector<std::byte> value,
                                                       std::uint32_t flags,
                                                       TimePoint expiry,
                                                       TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it != _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyExists });
    return Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> DiskStorage::Replace(std::string_view key,
                                                           std::vector<std::byte> value,
                                                           std::uint32_t flags,
                                                           TimePoint expiry,
                                                           TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyNotFound });
    return Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError>
DiskStorage::Append(std::string_view key, std::span<std::byte const> suffix, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyNotFound });
    auto combined = it->entry.value;
    combined.insert(combined.end(), suffix.begin(), suffix.end());
    return Set(key, std::move(combined), it->entry.flags, it->entry.expiry);
}

std::expected<CasToken, StorageError>
DiskStorage::Prepend(std::string_view key, std::span<std::byte const> prefix, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyNotFound });
    std::vector<std::byte> combined;
    combined.reserve(prefix.size() + it->entry.value.size());
    combined.insert(combined.end(), prefix.begin(), prefix.end());
    combined.insert(combined.end(), it->entry.value.begin(), it->entry.value.end());
    return Set(key, std::move(combined), it->entry.flags, it->entry.expiry);
}

std::expected<CasToken, StorageError> DiskStorage::CompareAndSwap(std::string_view key,
                                                                  CasToken expected,
                                                                  std::vector<std::byte> value,
                                                                  std::uint32_t flags,
                                                                  TimePoint expiry,
                                                                  TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyNotFound });
    if (it->entry.cas != expected)
        return std::unexpected(StorageError { .code = StorageErrorCode::CasMismatch });
    return Set(key, std::move(value), flags, expiry);
}

std::expected<IStorage::IncrResult, StorageError>
DiskStorage::IncrementOrInitialize(std::string_view key, std::int64_t delta, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyNotFound });

    auto const& bytes = it->entry.value;
    std::string asText;
    asText.reserve(bytes.size());
    for (auto const b : bytes)
        asText.push_back(static_cast<char>(b));

    std::uint64_t current = 0;
    auto const [_, ec] = std::from_chars(asText.data(), asText.data() + asText.size(), current);
    if (ec != std::errc {} || asText.empty())
        return std::unexpected(StorageError { .code = StorageErrorCode::InvalidArgument });

    std::uint64_t updated = current;
    if (delta >= 0)
        updated += static_cast<std::uint64_t>(delta);
    else
    {
        auto const absDelta = static_cast<std::uint64_t>(-(delta + 1)) + 1;
        updated = current > absDelta ? current - absDelta : 0;
    }
    auto newText = std::to_string(updated);
    std::vector<std::byte> newValue;
    newValue.reserve(newText.size());
    for (auto const c : newText)
        newValue.push_back(static_cast<std::byte>(c));

    auto const setResult = Set(key, std::move(newValue), it->entry.flags, it->entry.expiry);
    if (!setResult.has_value())
        return std::unexpected(setResult.error());
    return IStorage::IncrResult { .value = updated, .cas = *setResult };
}

std::expected<void, StorageError> DiskStorage::Delete(std::string_view key, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(StorageError { .code = StorageErrorCode::KeyNotFound });
    ApplyDelete(key);
    auto const result = AppendDelete(key);
    if (!result.has_value())
        return std::unexpected(result.error());
    return {};
}

void DiskStorage::FlushWithGeneration(TimePoint)
{
    ApplyFlush();
    (void) AppendFlush(); // best-effort: a failed flush record only means
                          // restart-after-crash won't bump the generation.
}

std::size_t DiskStorage::PurgeExpired(TimePoint now)
{
    std::size_t purged = 0;
    auto it = _lru.begin();
    while (it != _lru.end())
    {
        auto const next = std::next(it);
        if (!IsAlive(it->entry, _liveGeneration, now))
        {
            EraseAt(it);
            ++purged;
        }
        it = next;
    }
    return purged;
}

StorageStats DiskStorage::Snapshot() const noexcept
{
    _stats.itemCount = _lru.size();
    _stats.bytesUsed = _bytesUsed;
    _stats.bytesLimit = _options.maxBytes;
    return _stats;
}

std::expected<std::size_t, StorageError> DiskStorage::Compact()
{
    namespace fs = std::filesystem;
    auto const newPath = _options.logPath.string() + ".new";

    auto* newLog = std::fopen(newPath.c_str(), "wb+");
    if (!newLog)
        return std::unexpected(MakeIoError(errno, "fopen .new"));

    // Header.
    std::array<std::byte, HeaderSize> hdr {};
    std::memcpy(hdr.data(), Magic.data(), Magic.size());
    std::memcpy(hdr.data() + 4, &FormatVersion, sizeof(FormatVersion));
    if (std::fwrite(hdr.data(), 1, hdr.size(), newLog) != hdr.size())
    {
        std::fclose(newLog);
        return std::unexpected(MakeIoError(errno, "fwrite new header"));
    }

    // Walk LRU and write a Set record for each live entry.
    std::size_t written = 0;
    for (auto const& node : _lru)
    {
        std::vector<std::byte> payload;
        payload.reserve(RecordHeaderSize + node.key.size() + node.entry.value.size());
        PushLe<std::uint8_t>(payload, static_cast<std::uint8_t>(RecordType::Set));
        PushLe<std::uint32_t>(payload, node.entry.flags);
        PushLe<std::uint64_t>(payload, node.entry.cas);
        PushLe<std::int64_t>(payload, TimePointToMicros(node.entry.expiry));
        PushLe<std::uint64_t>(payload, node.entry.generation);
        PushLe<std::uint32_t>(payload, static_cast<std::uint32_t>(node.key.size()));
        PushLe<std::uint32_t>(payload, static_cast<std::uint32_t>(node.entry.value.size()));
        for (auto const c : node.key)
            payload.push_back(static_cast<std::byte>(c));
        for (auto const b : node.entry.value)
            payload.push_back(b);

        auto const totalLen = static_cast<std::uint32_t>(payload.size());
        auto const crc = Crc32c::Compute(std::span<std::byte const> { payload.data(), payload.size() });
        if (std::fwrite(&totalLen, 1, sizeof(totalLen), newLog) != sizeof(totalLen)
            || std::fwrite(&crc, 1, sizeof(crc), newLog) != sizeof(crc)
            || std::fwrite(payload.data(), 1, payload.size(), newLog) != payload.size())
        {
            std::fclose(newLog);
            return std::unexpected(MakeIoError(errno, "fwrite during compact"));
        }
        ++written;
    }

    std::fflush(newLog);
    FASTCACHED_FSYNC(FASTCACHED_FILENO(newLog));
    std::fclose(newLog);

    // Swap files: close old, rename new over it, reopen.
    std::fclose(_log);
    _log = nullptr;

    std::error_code rc;
    fs::rename(newPath, _options.logPath, rc);
    if (rc)
    {
        // Try to recover by reopening the old log; the new file is left
        // behind on disk for forensics.
        _log = std::fopen(_options.logPath.string().c_str(), "rb+");
        return std::unexpected(MakeIoError(rc.value(), "rename .new over log"));
    }

    _log = std::fopen(_options.logPath.string().c_str(), "rb+");
    if (!_log)
        return std::unexpected(MakeIoError(errno, "reopen after compact"));
    std::fseek(_log, 0, SEEK_END);
    return written;
}

void DiskStorage::Resize(std::size_t newMaxBytes)
{
    _options.maxBytes = newMaxBytes;
    EvictToFit();
}

} // namespace FastCache

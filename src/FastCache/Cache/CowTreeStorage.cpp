// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/CowTree.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/FilePageStore.hpp>

namespace FastCache
{

namespace
{

    /// Build a StorageError with just a code (and an optional context).
    /// Spelled out as a helper because gcc -Wmissing-field-initializers
    /// rejects designated initialisers that omit fields, and writing
    /// `{ .code = X, .systemCode = 0, .context = {} }` at every call
    /// site is noisy.
    [[nodiscard]] StorageError MakeError(StorageErrorCode code, std::string context = {}) noexcept
    {
        return StorageError { .code = code, .systemCode = 0, .context = std::move(context) };
    }

    /// Map CowTreeError into FastCache::StorageError.
    [[nodiscard]] StorageError TranslateError(CowTree::CowTreeError e, std::string context = {})
    {
        StorageErrorCode code = StorageErrorCode::IoError;
        switch (e)
        {
            case CowTree::CowTreeError::ValueTooLarge:
                code = StorageErrorCode::ValueTooLarge;
                break;
            case CowTree::CowTreeError::Corrupt:
            case CowTree::CowTreeError::CorruptMetas:
                code = StorageErrorCode::Corrupt;
                break;
            case CowTree::CowTreeError::InvalidArg:
            case CowTree::CowTreeError::OutOfRange:
                code = StorageErrorCode::InvalidArgument;
                break;
            case CowTree::CowTreeError::NotFound:
                code = StorageErrorCode::KeyNotFound;
                break;
            default:
                code = StorageErrorCode::IoError;
                break;
        }
        return StorageError {
            .code = code,
            .systemCode = 0,
            .context = std::move(context),
        };
    }

    template <typename T>
    void AppendLe(std::vector<std::byte>& buf, T value)
    {
        if constexpr (std::endian::native != std::endian::little)
            value = std::byteswap(value);
        auto const offset = buf.size();
        buf.resize(offset + sizeof(T));
        std::memcpy(buf.data() + offset, &value, sizeof(T));
    }

    template <typename T>
    bool ReadLe(CowTree::BytesView& cursor, T& out) noexcept
    {
        if (cursor.size() < sizeof(T))
            return false;
        T raw {};
        std::memcpy(&raw, cursor.data(), sizeof(T));
        cursor = cursor.subspan(sizeof(T));
        if constexpr (std::endian::native != std::endian::little)
            raw = std::byteswap(raw);
        out = raw;
        return true;
    }

    /// Convert a steady-clock TimePoint to a microsecond count for storage.
    /// `TimePoint::max()` (never expires) is stored as INT64_MAX.
    [[nodiscard]] std::int64_t TimePointToMicros(TimePoint tp)
    {
        if (tp == TimePoint::max())
            return std::numeric_limits<std::int64_t>::max();
        if (tp == TimePoint::min())
            return std::numeric_limits<std::int64_t>::min();
        return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    }

    [[nodiscard]] TimePoint MicrosToTimePoint(std::int64_t v)
    {
        if (v == std::numeric_limits<std::int64_t>::max())
            return TimePoint::max();
        if (v == std::numeric_limits<std::int64_t>::min())
            return TimePoint::min();
        return TimePoint { std::chrono::microseconds { v } };
    }

    [[nodiscard]] CowTree::BytesView KeyView(std::string_view sv) noexcept
    {
        return CowTree::BytesView { reinterpret_cast<std::byte const*>(sv.data()), sv.size() };
    }

    /// Compute a power-of-two page size large enough to hold a single
    /// entry of `maxValueBytes` plus the 32-byte CowTreeStorage entry
    /// header, an upper-bound key (1 KiB), and CowTree per-entry +
    /// page-header overhead. Floor of `DefaultPageSize`.
    [[nodiscard]] std::size_t DerivePageSize(std::size_t maxValueBytes) noexcept
    {
        constexpr std::size_t EntryHeader = 32;      ///< CowTreeStorage's encoded entry header.
        constexpr std::size_t LeafEntryOverhead = 6; ///< u16 keyLen + u32 valueLen.
        constexpr std::size_t PageHeader = 16;       ///< CowTree page header.
        constexpr std::size_t KeyBudget = 1024;      ///< typical max sccache key.
        auto const needed = maxValueBytes + EntryHeader + KeyBudget + LeafEntryOverhead + PageHeader;

        std::size_t pageSize = CowTree::DefaultPageSize;
        while (pageSize < needed && pageSize < CowTree::MaxPageSize)
            pageSize <<= 1;
        return pageSize;
    }

    [[nodiscard]] std::expected<std::uint64_t, StorageError> ParseUnsigned(std::span<std::byte const> bytes)
    {
        if (bytes.empty())
            return std::unexpected(MakeError(StorageErrorCode::InvalidArgument));
        std::string_view const sv { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
        std::uint64_t value = 0;
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        if (ec != std::errc {} || ptr != sv.data() + sv.size())
            return std::unexpected(MakeError(StorageErrorCode::InvalidArgument));
        return value;
    }

} // namespace

CowTreeStorage::CowTreeStorage(Options options) noexcept:
    _options { std::move(options) }
{
}

CowTreeStorage::~CowTreeStorage() = default;

std::expected<std::unique_ptr<CowTreeStorage>, StorageError> CowTreeStorage::Open(Options options)
{
    auto self = std::unique_ptr<CowTreeStorage> { new CowTreeStorage { std::move(options) } };

    CowTree::FilePageStore::Options pageOpts;
    pageOpts.path = self->_options.path;
    pageOpts.pageSize =
        self->_options.pageSize != 0 ? self->_options.pageSize : DerivePageSize(self->_options.maxValueBytes);
    pageOpts.durability = self->_options.durability;

    auto store = CowTree::FilePageStore::Open(pageOpts);
    if (!store.has_value())
        return std::unexpected(TranslateError(store.error(), "FilePageStore::Open"));
    self->_store = std::move(*store);

    self->_tree = std::make_unique<CowTree::CowTree>(*self->_store);
    if (auto const r = self->_tree->Open(); !r.has_value())
        return std::unexpected(TranslateError(r.error(), "CowTree::Open"));

    if (auto const r = self->Replay(); !r.has_value())
        return std::unexpected(r.error());

    self->_stats.bytesLimit = self->_options.maxBytes;
    return self;
}

std::vector<std::byte> CowTreeStorage::Encode(CacheEntry const& entry)
{
    std::vector<std::byte> out;
    out.reserve(4 + 8 + 8 + 8 + 4 + entry.value.size());
    AppendLe<std::uint32_t>(out, entry.flags);
    AppendLe<std::uint64_t>(out, entry.cas);
    AppendLe<std::int64_t>(out, TimePointToMicros(entry.expiry));
    AppendLe<std::uint64_t>(out, entry.generation);
    AppendLe<std::uint32_t>(out, static_cast<std::uint32_t>(entry.value.size()));
    auto const offset = out.size();
    out.resize(offset + entry.value.size());
    if (!entry.value.empty())
        std::memcpy(out.data() + offset, entry.value.data(), entry.value.size());
    return out;
}

std::expected<CacheEntry, StorageError> CowTreeStorage::Decode(CowTree::BytesView raw)
{
    auto cursor = raw;
    CacheEntry e;
    if (!ReadLe<std::uint32_t>(cursor, e.flags))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    if (!ReadLe<std::uint64_t>(cursor, e.cas))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    std::int64_t expiryUs = 0;
    if (!ReadLe<std::int64_t>(cursor, expiryUs))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    e.expiry = MicrosToTimePoint(expiryUs);
    if (!ReadLe<std::uint64_t>(cursor, e.generation))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    std::uint32_t valueLen = 0;
    if (!ReadLe<std::uint32_t>(cursor, valueLen))
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    if (cursor.size() < valueLen)
        return std::unexpected(MakeError(StorageErrorCode::Corrupt));
    e.value.assign(cursor.begin(), cursor.begin() + valueLen);
    return e;
}

std::expected<void, StorageError> CowTreeStorage::Replay()
{
    // For now, we don't enumerate the tree on Open — we trust that
    // Get() will fetch entries on demand. The LRU mirror starts empty,
    // and entries enter it when they're first accessed or written.
    // This means restart loses eviction order (LRU resets) but data is
    // preserved.
    //
    // Tracking CAS continuity across restarts: scan the tree to find
    // the max CAS used and set _nextCas accordingly. For simplicity we
    // skip this; new entries get fresh CAS tokens, which is acceptable
    // since CAS only needs to be unique within a session.
    return {};
}

std::expected<std::optional<CowTreeStorage::LoadedEntry>, StorageError> CowTreeStorage::LoadEntry(std::string_view key) const
{
    if (_tree == nullptr)
        return std::unexpected(MakeError(StorageErrorCode::IoError, "not open"));
    auto reader = _tree->BeginRead();
    auto got = reader.Get(KeyView(key));
    if (!got.has_value())
        return std::unexpected(TranslateError(got.error()));
    if (!got->has_value())
        return std::optional<LoadedEntry> {};
    auto entry = Decode(CowTree::BytesView { (*got)->data(), (*got)->size() });
    if (!entry.has_value())
        return std::unexpected(entry.error());
    return LoadedEntry { std::move(*entry) };
}

std::expected<void, StorageError> CowTreeStorage::StoreEntry(std::string_view key, CacheEntry const& entry)
{
    auto encoded = Encode(entry);
    auto txn = _tree->BeginWrite();
    if (auto const r = txn.Put(KeyView(key), CowTree::BytesView { encoded.data(), encoded.size() }); !r.has_value())
        return std::unexpected(TranslateError(r.error()));
    if (auto const r = txn.Commit(); !r.has_value())
        return std::unexpected(TranslateError(r.error()));
    return {};
}

std::expected<void, StorageError> CowTreeStorage::EraseEntry(std::string_view key)
{
    auto txn = _tree->BeginWrite();
    auto r = txn.Erase(KeyView(key));
    if (!r.has_value())
        return std::unexpected(TranslateError(r.error()));
    if (auto const c = txn.Commit(); !c.has_value())
        return std::unexpected(TranslateError(c.error()));
    return {};
}

void CowTreeStorage::TouchOrInsert(std::string_view key, std::size_t valueSize)
{
    auto it = _index.find(std::string { key });
    if (it != _index.end())
    {
        _bytesUsed -= it->second->bytes;
        it->second->bytes = valueSize;
        _bytesUsed += valueSize;
        _lru.splice(_lru.begin(), _lru, it->second);
        return;
    }
    _lru.push_front(LruNode { .key = std::string { key }, .bytes = valueSize });
    _index[std::string { key }] = _lru.begin();
    _bytesUsed += valueSize;
}

void CowTreeStorage::EraseFromLru(std::string_view key)
{
    auto it = _index.find(std::string { key });
    if (it == _index.end())
        return;
    _bytesUsed -= it->second->bytes;
    _lru.erase(it->second);
    _index.erase(it);
}

void CowTreeStorage::EvictToFit()
{
    if (_options.maxBytes == 0)
        return;
    while (_bytesUsed > _options.maxBytes && !_lru.empty())
    {
        auto victim = std::prev(_lru.end());
        auto keyCopy = victim->key;
        _bytesUsed -= victim->bytes;
        _index.erase(victim->key);
        _lru.erase(victim);
        std::ignore = EraseEntry(keyCopy);
        ++_stats.evictions;
    }
}

std::expected<GetResult, StorageError> CowTreeStorage::Get(std::string_view key, TimePoint now)
{
    ++_stats.cmdGet;
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value())
    {
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    auto& entry = (*loaded)->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
    {
        // Expired or flushed.
        std::ignore = EraseEntry(key);
        EraseFromLru(key);
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    TouchOrInsert(key, entry.value.size());
    ++_stats.getHits;
    GetResult result;
    result.found = true;
    result.entry = std::move(entry);
    return result;
}

std::expected<CasToken, StorageError> CowTreeStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
    ++_stats.cmdSet;
    if (value.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    CacheEntry e;
    e.value = std::move(value);
    e.flags = flags;
    e.cas = _nextCas++;
    e.expiry = expiry;
    e.generation = _liveGeneration;

    if (auto const r = StoreEntry(key, e); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, e.value.size());
    EvictToFit();
    return e.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (loaded->has_value() && (*loaded)->entry.expiry > now && (*loaded)->entry.generation >= _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyExists));
    return Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> CowTreeStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    return Set(key, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> CowTreeStorage::Append(std::string_view key,
                                                             std::span<std::byte const> suffix,
                                                             TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    if (entry.value.size() + suffix.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    entry.value.insert(entry.value.end(), suffix.begin(), suffix.end());
    entry.cas = _nextCas++;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, entry.value.size());
    EvictToFit();
    return entry.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    if (entry.value.size() + prefix.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    std::vector<std::byte> merged;
    merged.reserve(prefix.size() + entry.value.size());
    merged.insert(merged.end(), prefix.begin(), prefix.end());
    merged.insert(merged.end(), entry.value.begin(), entry.value.end());
    entry.value = std::move(merged);
    entry.cas = _nextCas++;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, entry.value.size());
    EvictToFit();
    return entry.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::CompareAndSwap(std::string_view key,
                                                                     CasToken expected,
                                                                     std::vector<std::byte> value,
                                                                     std::uint32_t flags,
                                                                     TimePoint expiry,
                                                                     TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    if ((*loaded)->entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    return Set(key, std::move(value), flags, expiry);
}

std::expected<IStorage::IncrResult, StorageError> CowTreeStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::int64_t delta,
                                                                                        TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    std::uint64_t current = 0;
    bool exists = false;
    if (loaded->has_value() && (*loaded)->entry.expiry > now && (*loaded)->entry.generation >= _liveGeneration)
    {
        exists = true;
        auto parsed =
            ParseUnsigned(std::span<std::byte const> { (*loaded)->entry.value.data(), (*loaded)->entry.value.size() });
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        current = *parsed;
    }

    std::uint64_t next = 0;
    if (delta >= 0)
    {
        next = current + static_cast<std::uint64_t>(delta);
    }
    else
    {
        auto const sub = static_cast<std::uint64_t>(-delta);
        next = (sub >= current) ? 0U : (current - sub);
    }
    auto const s = std::to_string(next);
    std::vector<std::byte> bytes;
    bytes.reserve(s.size());
    for (auto const c: s)
        bytes.push_back(static_cast<std::byte>(c));

    auto const expiryToUse = exists ? (*loaded)->entry.expiry : TimePoint::max();
    auto const flagsToUse = exists ? (*loaded)->entry.flags : 0U;
    auto cas = Set(key, std::move(bytes), flagsToUse, expiryToUse);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return IStorage::IncrResult { .value = next, .cas = *cas };
}

std::expected<void, StorageError> CowTreeStorage::Delete(std::string_view key, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    if (auto const r = EraseEntry(key); !r.has_value())
        return std::unexpected(r.error());
    EraseFromLru(key);
    return {};
}

void CowTreeStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    ++_liveGeneration;
    _flushEffectiveAt = effectiveAt;
}

std::size_t CowTreeStorage::PurgeExpired(TimePoint /*now*/)
{
    // No-op for v1; expired entries are purged on Get/Delete/Replace path.
    return 0;
}

StorageStats CowTreeStorage::Snapshot() const noexcept
{
    _stats.itemCount = _index.size();
    _stats.bytesUsed = _bytesUsed;
    _stats.bytesLimit = _options.maxBytes;
    return _stats;
}

void CowTreeStorage::Resize(std::size_t newMaxBytes)
{
    _options.maxBytes = newMaxBytes;
    _stats.bytesLimit = newMaxBytes;
    EvictToFit();
}

} // namespace FastCache

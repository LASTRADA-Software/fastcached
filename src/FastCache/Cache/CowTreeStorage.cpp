// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Profiling.hpp>

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
    out.reserve(4 + 8 + 8 + 8 + 4 + entry.value.size() + 8 + 1);
    AppendLe<std::uint32_t>(out, entry.flags);
    AppendLe<std::uint64_t>(out, entry.cas);
    AppendLe<std::int64_t>(out, TimePointToMicros(entry.expiry));
    AppendLe<std::uint64_t>(out, entry.generation);
    AppendLe<std::uint32_t>(out, static_cast<std::uint32_t>(entry.value.size()));
    auto const offset = out.size();
    out.resize(offset + entry.value.size());
    if (!entry.value.empty())
        std::memcpy(out.data() + offset, entry.value.data(), entry.value.size());
    // v2 trailer (always written on new entries; absent in legacy files).
    AppendLe<std::int64_t>(out, TimePointToMicros(entry.lastAccess));
    AppendLe<std::uint8_t>(out, entry.stale ? std::uint8_t { 1 } : std::uint8_t { 0 });
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
    cursor = cursor.subspan(valueLen);
    // v2 trailer: present in entries written by this version, absent in
    // legacy files. Both fields are optional and default to "unread, not
    // stale" when missing.
    std::int64_t lastAccessUs = 0;
    if (ReadLe<std::int64_t>(cursor, lastAccessUs))
        e.lastAccess = MicrosToTimePoint(lastAccessUs);
    std::uint8_t staleByte = 0;
    if (ReadLe<std::uint8_t>(cursor, staleByte))
        e.stale = staleByte != 0;
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

void CowTreeStorage::TouchOrInsert(std::string_view key, std::size_t valueSize, AccessKind access)
{
    auto it = _index.find(key);
    if (it != _index.end())
    {
        // A read records the access, a value-rewriting Write clears the bit
        // (the new value is unread), and a TTL-only Preserve (Touch /
        // MarkStale) leaves the existing bit alone — matching
        // InMemoryLruStorage, where Touch never disturbs `fetched`.
        bool const fetched = [&] {
            if (access == AccessKind::Read)
                return true;
            if (access == AccessKind::Write)
                return false;
            return it->second->fetched;
        }();
        _bytesUsed -= it->second->bytes;
        it->second->bytes = valueSize;
        it->second->fetched = fetched;
        _bytesUsed += valueSize;
        _lru.splice(_lru.begin(), _lru, it->second);
        return;
    }
    // A brand-new mirror node has never been read; only an explicit Read
    // marks it fetched.
    _lru.push_front(LruNode { .key = std::string { key }, .bytes = valueSize, .fetched = access == AccessKind::Read });
    _index.emplace(_lru.front().key, _lru.begin());
    _bytesUsed += valueSize;
}

void CowTreeStorage::EraseFromLru(std::string_view key)
{
    auto it = _index.find(key);
    if (it == _index.end())
        return;
    _bytesUsed -= it->second->bytes;
    _lru.erase(it->second);
    _index.erase(it);
}

void CowTreeStorage::EvictToFit()
{
    FC_ZONE_SCOPED_N("CowTreeStorage::EvictToFit");
    if (_options.maxBytes == 0)
        return;
    // Track remaining attempts so a stuck disk (e.g. ENOSPC on every
    // commit) walks the LRU once and bails rather than spinning. Better
    // to leave the soft cap violated than to mutate the in-memory mirror
    // out of sync with the tree on disk.
    auto remainingAttempts = _lru.size();
    while (_bytesUsed > _options.maxBytes && !_lru.empty() && remainingAttempts != 0)
    {
        auto victim = std::prev(_lru.end());
        auto const keyCopy = victim->key;
        if (auto const r = EraseEntry(keyCopy); !r.has_value())
        {
            // Disk delete failed; rotate the stuck victim out of the
            // tail so the next iteration tries a different key. Once
            // every entry has rotated through unsuccessfully we bail.
            _lru.splice(_lru.begin(), _lru, victim);
            --remainingAttempts;
            continue;
        }
        if (!victim->fetched)
            ++_stats.evictedUnfetched;
        _bytesUsed -= victim->bytes;
        _index.erase(keyCopy);
        _lru.erase(victim);
        ++_stats.evictions;
        remainingAttempts = _lru.size();
    }
}

std::expected<GetResult, StorageError> CowTreeStorage::Get(std::string_view key, TimePoint now)
{
    FC_ZONE_SCOPED_N("CowTreeStorage::Get");
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
        // Expired or flushed. Treat as a miss; do NOT mutate the tree
        // from a read path — under ShardedStorage::Get the caller
        // holds only a shared_lock, so opening a write transaction
        // would violate CowTree's single-writer contract and race
        // concurrent expired-Gets on the same shard. Defer the on-
        // disk cleanup to PurgeExpired (writer-locked).
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    entry.lastAccess = now;
    TouchOrInsert(key, entry.value.size(), AccessKind::Read);
    ++_stats.getHits;
    // Deliberately do NOT persist the lastAccess advance here: a read must
    // not open a write transaction (CoW page churn + log growth on every
    // hit would cripple read-heavy workloads, and under ShardedStorage the
    // single-writer contract must hold). The returned copy carries the
    // fresh lastAccess for the caller; the on-disk value only advances on
    // the next genuine write (Set / Touch / MarkStale / ...).
    GetResult result;
    result.found = true;
    result.entry = std::move(entry);
    return result;
}

std::expected<CasToken, StorageError> CowTreeStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    ++_stats.cmdTouch;
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
    {
        ++_stats.touchMisses;
        return std::unexpected(loaded.error());
    }
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
    {
        ++_stats.touchMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    auto& entry = (*loaded)->entry;
    // A touch refreshes TTL/CAS/lastAccess but rewrites no value bytes, so the
    // LRU `fetched` bit is preserved (AccessKind::Preserve) rather than
    // cleared — matching InMemoryLruStorage::Touch, which leaves it alone.
    entry.expiry = newExpiry;
    entry.cas = _nextCas++;
    entry.lastAccess = now;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, entry.value.size(), AccessKind::Preserve);
    ++_stats.touchHits;
    return entry.cas;
}

std::expected<GetResult, StorageError> CowTreeStorage::Peek(std::string_view key, TimePoint now)
{
    // Non-mutating: no LRU promotion, no lastAccess advance, no stats, and
    // crucially no write transaction. Expired / flushed entries read as a
    // miss but are left for the writer-locked PurgeExpired to reclaim.
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value())
        return GetResult { .found = false, .entry = {} };
    auto& entry = (*loaded)->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
        return GetResult { .found = false, .entry = {} };
    GetResult result;
    result.found = true;
    result.entry = std::move(entry);
    return result;
}

std::expected<CasToken, StorageError> CowTreeStorage::MarkStale(std::string_view key,
                                                                std::optional<TimePoint> newExpiry,
                                                                TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    entry.stale = true;
    if (newExpiry.has_value())
        entry.expiry = *newExpiry;
    entry.cas = _nextCas++;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    // Marking stale rewrites no value bytes, so keep the `fetched` bit.
    TouchOrInsert(key, entry.value.size(), AccessKind::Preserve);
    return entry.cas;
}

std::expected<GetResult, StorageError> CowTreeStorage::GetAndTouch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    // Refresh the expiry, then read the refreshed entry back. The atomicity
    // boundary is the enclosing ShardedStorage's per-shard lock (this tier
    // never owns a lock); on the unwrapped single-threaded reactor there is
    // no concurrent writer to interleave between the touch and the read.
    auto const touched = Touch(key, newExpiry, now);
    if (!touched.has_value())
        return std::unexpected(touched.error());
    return Get(key, now);
}

std::expected<void, StorageError> CowTreeStorage::CompareAndDelete(std::string_view key, CasToken expected, TimePoint now)
{
    auto const peeked = Peek(key, now);
    if (!peeked.has_value())
        return std::unexpected(peeked.error());
    if (!peeked->found)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    if (peeked->entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    return Delete(key, now);
}

std::expected<CasToken, StorageError> CowTreeStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
    FC_ZONE_SCOPED_N("CowTreeStorage::Set");
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
                                                             CasToken expected,
                                                             TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    if (expected != 0 && entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    if (entry.value.size() + suffix.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    entry.value.insert(entry.value.end(), suffix.begin(), suffix.end());
    entry.cas = _nextCas++;
    // A value-rewriting mutation produces a fresh item nobody has read yet
    // (mirrors InMemoryLruStorage::MutateExisting and the CacheEntry contract).
    entry.stale = false;
    if (auto const r = StoreEntry(key, entry); !r.has_value())
        return std::unexpected(r.error());
    TouchOrInsert(key, entry.value.size());
    EvictToFit();
    return entry.cas;
}

std::expected<CasToken, StorageError> CowTreeStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              CasToken expected,
                                                              TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    auto& entry = (*loaded)->entry;
    if (expected != 0 && entry.cas != expected)
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    if (entry.value.size() + prefix.size() > _options.maxValueBytes)
        return std::unexpected(MakeError(StorageErrorCode::ValueTooLarge));
    std::vector<std::byte> merged;
    merged.reserve(prefix.size() + entry.value.size());
    merged.insert(merged.end(), prefix.begin(), prefix.end());
    merged.insert(merged.end(), entry.value.begin(), entry.value.end());
    entry.value = std::move(merged);
    entry.cas = _nextCas++;
    // A value-rewriting mutation produces a fresh item nobody has read yet
    // (mirrors InMemoryLruStorage::MutateExisting and the CacheEntry contract).
    entry.stale = false;
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
    {
        ++_stats.casMisses;
        return std::unexpected(loaded.error());
    }
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
    {
        ++_stats.casMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    if ((*loaded)->entry.cas != expected)
    {
        ++_stats.casBadval;
        return std::unexpected(MakeError(StorageErrorCode::CasMismatch));
    }
    ++_stats.casHits;
    return Set(key, std::move(value), flags, expiry);
}

std::expected<IStorage::IncrResult, StorageError> CowTreeStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::uint64_t magnitude,
                                                                                        bool decrement,
                                                                                        TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(loaded.error());
    }
    // Miss = KeyNotFound, matching InMemoryLruStorage. The "initialize"
    // half of the name is the protocol layer's job: it owns the spec
    // semantics (binary `initial`/`expiration`, meta `J`/`N`) and re-issues
    // a Set on KeyNotFound. Auto-vivifying here with current=0 would ignore
    // those flags and silently bypass the binary "do not create" sentinel.
    if (!loaded->has_value() || (*loaded)->entry.expiry <= now || (*loaded)->entry.generation < _liveGeneration)
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }

    auto& existing = (*loaded)->entry;
    std::uint64_t current = 0;
    {
        auto parsed = ParseUnsigned(std::span<std::byte const> { existing.value.data(), existing.value.size() });
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        current = *parsed;
    }

    // memcached: increment wraps modulo 2^64 (natural for uint64), decrement
    // saturates at 0. The full unsigned `magnitude` is honoured, so deltas in
    // [2^63, 2^64) add/subtract correctly instead of aliasing direction.
    std::uint64_t const next = [&]() -> std::uint64_t {
        if (!decrement)
            return current + magnitude;
        return magnitude >= current ? 0U : current - magnitude;
    }();
    auto const s = std::to_string(next);
    std::vector<std::byte> bytes;
    bytes.reserve(s.size());
    for (auto const c: s)
        bytes.push_back(static_cast<std::byte>(c));

    auto cas = Set(key, std::move(bytes), existing.flags, existing.expiry);
    if (!cas.has_value())
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(cas.error());
    }
    if (decrement)
        ++_stats.decrHits;
    else
        ++_stats.incrHits;
    return IStorage::IncrResult { .value = next, .cas = *cas };
}

std::expected<void, StorageError> CowTreeStorage::Delete(std::string_view key, TimePoint now)
{
    auto loaded = LoadEntry(key);
    if (!loaded.has_value())
    {
        ++_stats.deleteMisses;
        return std::unexpected(loaded.error());
    }
    if (!loaded->has_value())
    {
        ++_stats.deleteMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    auto const& entry = (*loaded)->entry;
    if (entry.expiry <= now || entry.generation < _liveGeneration)
    {
        // The on-disk record is stale (expired or older than the
        // current flush generation); still clean it up so a subsequent
        // restart doesn't replay the dead bytes. Caller's view is the
        // same as if the key was absent — KeyNotFound.
        std::ignore = EraseEntry(key);
        EraseFromLru(key);
        ++_stats.deleteMisses;
        return std::unexpected(MakeError(StorageErrorCode::KeyNotFound));
    }
    if (auto const r = EraseEntry(key); !r.has_value())
        return std::unexpected(r.error());
    EraseFromLru(key);
    ++_stats.deleteHits;
    return {};
}

void CowTreeStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    ++_liveGeneration;
    _flushEffectiveAt = effectiveAt;
    ++_stats.cmdFlush;
}

std::size_t CowTreeStorage::PurgeExpired(TimePoint now)
{
    // Walk the LRU mirror, collect keys whose stored entry is expired
    // or stale-generation, then erase them on disk + drop them from
    // the mirror. Two-phase to avoid iterator invalidation by
    // EraseFromLru. Bound to keys present in the mirror — entries on
    // disk that the LRU does not know about (post-restart, with Replay
    // a no-op) are reachable only via Get, which the reader-side fix
    // now leaves alone.
    std::vector<std::string> victims;
    victims.reserve(_lru.size());
    for (auto const& node: _lru)
    {
        auto loaded = LoadEntry(node.key);
        if (!loaded.has_value())
            continue;
        if (!loaded->has_value())
        {
            // Disk and mirror out of sync — drop the orphan from the
            // mirror; nothing to erase on disk.
            victims.push_back(node.key);
            continue;
        }
        auto const& entry = (*loaded)->entry;
        if (entry.expiry <= now)
        {
            if (!node.fetched)
                ++_stats.expiredUnfetched;
            victims.push_back(node.key);
        }
        else if (entry.generation < _liveGeneration)
        {
            victims.push_back(node.key);
        }
    }
    std::size_t purged = 0;
    for (auto const& key: victims)
    {
        auto loaded = LoadEntry(key);
        if (loaded.has_value() && loaded->has_value())
        {
            if (auto const r = EraseEntry(key); !r.has_value())
                continue;
        }
        EraseFromLru(key);
        ++purged;
    }
    return purged;
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

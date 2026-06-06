// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache
{

namespace
{

    [[nodiscard]] StorageError MakeKeyNotFound()
    {
        return StorageError { .code = StorageErrorCode::KeyNotFound, .systemCode = 0, .context = {} };
    }

    [[nodiscard]] StorageError MakeKeyExists()
    {
        return StorageError { .code = StorageErrorCode::KeyExists, .systemCode = 0, .context = {} };
    }

    [[nodiscard]] StorageError MakeCasMismatch()
    {
        return StorageError { .code = StorageErrorCode::CasMismatch, .systemCode = 0, .context = {} };
    }

    [[nodiscard]] StorageError MakeInvalidArgument(std::string context)
    {
        return StorageError { .code = StorageErrorCode::InvalidArgument, .context = std::move(context) };
    }

    /// True if the entry is alive at `now` given the current liveGeneration.
    [[nodiscard]] bool IsAlive(CacheEntry const& entry, std::uint64_t liveGen, TimePoint now) noexcept
    {
        if (entry.generation < liveGen)
            return false;
        if (entry.expiry <= now)
            return false;
        return true;
    }

} // namespace

InMemoryLruStorage::InMemoryLruStorage(std::size_t maxBytes, std::size_t maxValueBytes) noexcept:
    _maxBytes { maxBytes },
    _maxValueBytes { maxValueBytes }
{
}

InMemoryLruStorage::Iterator InMemoryLruStorage::FindAlive(std::string_view key, TimePoint now)
{
    auto const indexIt = _index.find(key);
    if (indexIt == _index.end())
        return _lru.end();

    auto const nodeIt = indexIt->second;
    if (!IsAlive(nodeIt->entry, _liveGeneration, now))
    {
        // Lazy reclaim during lookup. Attribute a never-fetched expiry to
        // the expired_unfetched counter (generation flushes are not
        // "expiry" and are excluded).
        if (nodeIt->entry.expiry <= now && !nodeIt->entry.fetched)
            ++_stats.expiredUnfetched;
        EraseAt(nodeIt);
        return _lru.end();
    }

    // Move to front (most recently used).
    _lru.splice(_lru.begin(), _lru, nodeIt);
    return nodeIt;
}

void InMemoryLruStorage::EraseAt(Iterator it)
{
    _bytesUsed -= it->entry.ValueSize();
    _index.erase(it->key);
    _lru.erase(it);
}

void InMemoryLruStorage::EvictToFit()
{
    FC_ZONE_SCOPED_N("LruStorage::EvictToFit");
    if (_maxBytes == 0)
        return;
    while (_bytesUsed > _maxBytes && !_lru.empty())
    {
        auto const victim = std::prev(_lru.end());
        if (!victim->entry.fetched)
            ++_stats.evictedUnfetched;
        EraseAt(victim);
        ++_stats.evictions;
    }
    // Memory-pressure timeline so the Tracy viewer shows how close the cache
    // runs to its byte cap over the course of a workload.
    FC_PLOT("lru.bytesUsed", static_cast<std::int64_t>(_bytesUsed));
}

CasToken InMemoryLruStorage::InsertNew(std::string key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry)
{
    FC_ZONE_SCOPED_N("LruStorage::InsertNew");
    auto const cas = _nextCas++;
    auto const size = value.size();

    Node node;
    node.key = std::move(key);
    node.entry.value = MakeSharedValue(std::move(value));
    node.entry.flags = flags;
    node.entry.cas = cas;
    node.entry.expiry = expiry;
    node.entry.generation = _liveGeneration;

    _lru.push_front(std::move(node));
    _index.emplace(_lru.front().key, _lru.begin());
    _bytesUsed += size;

    EvictToFit();
    return cas;
}

CasToken InMemoryLruStorage::MutateExisting(Iterator it, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry)
{
    FC_ZONE_SCOPED_N("LruStorage::MutateExisting");
    _bytesUsed -= it->entry.ValueSize();
    // Rebind to a fresh immutable buffer (copy-on-write): any reader still
    // holding the previous SharedValue keeps a valid, unchanged payload.
    it->entry.value = MakeSharedValue(std::move(value));
    it->entry.flags = flags;
    it->entry.expiry = expiry;
    it->entry.generation = _liveGeneration;
    it->entry.cas = _nextCas++;
    // A value-rewriting mutation produces a fresh item nobody has read yet.
    it->entry.stale = false;
    it->entry.fetched = false;
    _bytesUsed += it->entry.ValueSize();
    _lru.splice(_lru.begin(), _lru, it);
    EvictToFit();
    return it->entry.cas;
}

std::expected<GetResult, StorageError> InMemoryLruStorage::Get(std::string_view key, TimePoint now)
{
    FC_ZONE_SCOPED_N("LruStorage::Get");
    ++_stats.cmdGet;
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
    {
        ++_stats.getMisses;
        return GetResult { .found = false, .entry = {} };
    }
    ++_stats.getHits;
    // Snapshot the entry as it was *before* this read so the meta `l` flag
    // reports seconds since the PREVIOUS access; then advance the stored
    // lastAccess and mark it fetched for the next reader.
    GetResult result { .found = true, .entry = it->entry };
    it->entry.lastAccess = now;
    it->entry.fetched = true;
    return result;
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Set(std::string_view key,
                                                              std::vector<std::byte> value,
                                                              std::uint32_t flags,
                                                              TimePoint expiry)
{
    FC_ZONE_SCOPED_N("LruStorage::Set");
    ++_stats.cmdSet;
    if (ExceedsValueLimit(value.size()))
        return std::unexpected(MakeStorageError(StorageErrorCode::ValueTooLarge));
    auto const indexIt = _index.find(key);
    if (indexIt != _index.end())
        return MutateExisting(indexIt->second, std::move(value), flags, expiry);
    return InsertNew(std::string { key }, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    if (ExceedsValueLimit(value.size()))
        return std::unexpected(MakeStorageError(StorageErrorCode::ValueTooLarge));
    auto const it = FindAlive(key, now);
    if (it != _lru.end())
        return std::unexpected(MakeKeyExists());
    return InsertNew(std::string { key }, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    if (ExceedsValueLimit(value.size()))
        return std::unexpected(MakeStorageError(StorageErrorCode::ValueTooLarge));
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());
    return MutateExisting(it, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Append(std::string_view key,
                                                                 std::span<std::byte const> suffix,
                                                                 CasToken expected,
                                                                 TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());
    if (expected != 0 && it->entry.cas != expected)
        return std::unexpected(MakeCasMismatch());
    if (ExceedsValueLimit(it->entry.ValueSize() + suffix.size()))
        return std::unexpected(MakeStorageError(StorageErrorCode::ValueTooLarge));

    auto const existing = it->entry.ValueBytes();
    std::vector<std::byte> combined;
    combined.reserve(existing.size() + suffix.size());
    combined.insert(combined.end(), existing.begin(), existing.end());
    combined.insert(combined.end(), suffix.begin(), suffix.end());
    return MutateExisting(it, std::move(combined), it->entry.flags, it->entry.expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Prepend(std::string_view key,
                                                                  std::span<std::byte const> prefix,
                                                                  CasToken expected,
                                                                  TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());
    if (expected != 0 && it->entry.cas != expected)
        return std::unexpected(MakeCasMismatch());
    if (ExceedsValueLimit(it->entry.ValueSize() + prefix.size()))
        return std::unexpected(MakeStorageError(StorageErrorCode::ValueTooLarge));

    auto const existing = it->entry.ValueBytes();
    std::vector<std::byte> combined;
    combined.reserve(prefix.size() + existing.size());
    combined.insert(combined.end(), prefix.begin(), prefix.end());
    combined.insert(combined.end(), existing.begin(), existing.end());
    return MutateExisting(it, std::move(combined), it->entry.flags, it->entry.expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::CompareAndSwap(std::string_view key,
                                                                         CasToken expected,
                                                                         std::vector<std::byte> value,
                                                                         std::uint32_t flags,
                                                                         TimePoint expiry,
                                                                         TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
    {
        ++_stats.casMisses;
        return std::unexpected(MakeKeyNotFound());
    }
    if (it->entry.cas != expected)
    {
        ++_stats.casBadval;
        return std::unexpected(MakeCasMismatch());
    }
    if (ExceedsValueLimit(value.size()))
        return std::unexpected(MakeStorageError(StorageErrorCode::ValueTooLarge));
    ++_stats.casHits;
    return MutateExisting(it, std::move(value), flags, expiry);
}

std::expected<IStorage::IncrResult, StorageError> InMemoryLruStorage::IncrementOrInitialize(std::string_view key,
                                                                                            std::uint64_t magnitude,
                                                                                            bool decrement,
                                                                                            TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
    {
        if (decrement)
            ++_stats.decrMisses;
        else
            ++_stats.incrMisses;
        return std::unexpected(MakeKeyNotFound());
    }

    // Decode the existing value as ASCII unsigned 64-bit *before* booking a
    // hit: a non-numeric value is a client error, not a successful incr/decr.
    auto const bytes = it->entry.ValueBytes();
    std::string asText;
    asText.reserve(bytes.size());
    for (auto const b: bytes)
        asText.push_back(static_cast<char>(b));

    std::uint64_t current = 0;
    auto const [_, ec] = std::from_chars(asText.data(), asText.data() + asText.size(), current);
    if (ec != std::errc {} || asText.empty())
        return std::unexpected(MakeInvalidArgument("existing value is not numeric"));

    if (decrement)
        ++_stats.decrHits;
    else
        ++_stats.incrHits;

    // memcached: increment wraps modulo 2^64 (natural for uint64), decrement
    // saturates at 0. `magnitude` is the full unsigned amount, so deltas in
    // [2^63, 2^64) are honoured rather than aliasing to the wrong direction.
    std::uint64_t const updated = [&]() -> std::uint64_t {
        if (!decrement)
            return current + magnitude;
        return magnitude >= current ? 0U : current - magnitude;
    }();

    auto newText = std::to_string(updated);
    std::vector<std::byte> newValue;
    newValue.reserve(newText.size());
    for (auto const c: newText)
        newValue.push_back(static_cast<std::byte>(c));

    auto const cas = MutateExisting(it, std::move(newValue), it->entry.flags, it->entry.expiry);
    return IStorage::IncrResult { .value = updated, .cas = cas };
}

std::expected<void, StorageError> InMemoryLruStorage::Delete(std::string_view key, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
    {
        ++_stats.deleteMisses;
        return std::unexpected(MakeKeyNotFound());
    }
    EraseAt(it);
    ++_stats.deleteHits;
    return {};
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    ++_stats.cmdTouch;
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
    {
        ++_stats.touchMisses;
        return std::unexpected(MakeKeyNotFound());
    }
    it->entry.expiry = newExpiry;
    it->entry.cas = _nextCas++;
    it->entry.lastAccess = now;
    ++_stats.touchHits;
    return it->entry.cas;
}

std::expected<GetResult, StorageError> InMemoryLruStorage::Peek(std::string_view key, TimePoint now)
{
    // Non-mutating: no LRU promotion, no lastAccess/fetched update, no
    // stats. Expired / flushed entries read as a miss but are left in
    // place for the writer-locked PurgeExpired to reclaim.
    auto const indexIt = _index.find(key);
    if (indexIt == _index.end())
        return GetResult { .found = false, .entry = {} };
    auto const& entry = indexIt->second->entry;
    if (!IsAlive(entry, _liveGeneration, now))
        return GetResult { .found = false, .entry = {} };
    return GetResult { .found = true, .entry = entry };
}

std::expected<CasToken, StorageError> InMemoryLruStorage::MarkStale(std::string_view key,
                                                                    std::optional<TimePoint> newExpiry,
                                                                    TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());
    it->entry.stale = true;
    if (newExpiry.has_value())
        it->entry.expiry = *newExpiry;
    it->entry.cas = _nextCas++;
    return it->entry.cas;
}

void InMemoryLruStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    // For an immediate flush (now or in the past), bump the generation
    // right away so all current entries become invisible. For a delayed
    // flush, record the effective time; entries observed via FindAlive
    // after effectiveAt has elapsed will be treated as expired by
    // PurgeExpired, but for simplicity we just bump generation immediately
    // here. memcached's `flush_all <delay>` is rarely used; if needed, the
    // engine can schedule the bump on the reactor instead.
    static_cast<void>(effectiveAt);
    ++_liveGeneration;
    ++_stats.cmdFlush;
}

std::size_t InMemoryLruStorage::PurgeExpired(TimePoint now)
{
    std::size_t purged = 0;
    auto it = _lru.begin();
    while (it != _lru.end())
    {
        auto const next = std::next(it);
        if (!IsAlive(it->entry, _liveGeneration, now))
        {
            if (it->entry.expiry <= now && !it->entry.fetched)
                ++_stats.expiredUnfetched;
            EraseAt(it);
            ++purged;
        }
        it = next;
    }
    return purged;
}

StorageStats InMemoryLruStorage::Snapshot() const noexcept
{
    _stats.itemCount = _lru.size();
    _stats.bytesUsed = _bytesUsed;
    _stats.bytesLimit = _maxBytes;
    return _stats;
}

void InMemoryLruStorage::Resize(std::size_t newMaxBytes)
{
    _maxBytes = newMaxBytes;
    EvictToFit();
}

void InMemoryLruStorage::InsertVerbatim(std::string_view key, CacheEntry entry)
{
    // Bypass the CAS / generation machinery and store the entry as the
    // caller supplied it. The whole point is to mirror a lower-tier
    // entry without rewriting its identity.
    auto const indexIt = _index.find(key);
    if (indexIt != _index.end())
    {
        auto const nodeIt = indexIt->second;
        _bytesUsed -= nodeIt->entry.ValueSize();
        nodeIt->entry = std::move(entry);
        _bytesUsed += nodeIt->entry.ValueSize();
        _lru.splice(_lru.begin(), _lru, nodeIt);
        EvictToFit();
        return;
    }
    Node node;
    node.key = std::string { key };
    node.entry = std::move(entry);
    auto const size = node.entry.ValueSize();
    _lru.push_front(std::move(node));
    _index.emplace(_lru.front().key, _lru.begin());
    _bytesUsed += size;
    EvictToFit();
}

void InMemoryLruStorage::EraseIfPresent(std::string_view key)
{
    auto const indexIt = _index.find(key);
    if (indexIt == _index.end())
        return;
    EraseAt(indexIt->second);
}

} // namespace FastCache

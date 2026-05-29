// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>

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

InMemoryLruStorage::InMemoryLruStorage(std::size_t maxBytes) noexcept:
    _maxBytes { maxBytes }
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
        EraseAt(nodeIt);
        return _lru.end();
    }

    // Move to front (most recently used).
    _lru.splice(_lru.begin(), _lru, nodeIt);
    return nodeIt;
}

void InMemoryLruStorage::EraseAt(Iterator it)
{
    _bytesUsed -= it->entry.value.size();
    _index.erase(it->key);
    _lru.erase(it);
}

void InMemoryLruStorage::EvictToFit()
{
    if (_maxBytes == 0)
        return;
    while (_bytesUsed > _maxBytes && !_lru.empty())
    {
        EraseAt(std::prev(_lru.end()));
        ++_stats.evictions;
    }
}

CasToken InMemoryLruStorage::InsertNew(std::string key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry)
{
    auto const cas = _nextCas++;
    auto const size = value.size();

    Node node;
    node.key = std::move(key);
    node.entry.value = std::move(value);
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
    _bytesUsed -= it->entry.value.size();
    it->entry.value = std::move(value);
    it->entry.flags = flags;
    it->entry.expiry = expiry;
    it->entry.generation = _liveGeneration;
    it->entry.cas = _nextCas++;
    _bytesUsed += it->entry.value.size();
    _lru.splice(_lru.begin(), _lru, it);
    EvictToFit();
    return it->entry.cas;
}

std::expected<GetResult, StorageError> InMemoryLruStorage::Get(std::string_view key, TimePoint now)
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

std::expected<CasToken, StorageError> InMemoryLruStorage::Set(std::string_view key,
                                                              std::vector<std::byte> value,
                                                              std::uint32_t flags,
                                                              TimePoint expiry)
{
    ++_stats.cmdSet;
    auto const indexIt = _index.find(key);
    if (indexIt != _index.end())
        return MutateExisting(indexIt->second, std::move(value), flags, expiry);
    return InsertNew(std::string { key }, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it != _lru.end())
        return std::unexpected(MakeKeyExists());
    return InsertNew(std::string { key }, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());
    return MutateExisting(it, std::move(value), flags, expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Append(std::string_view key,
                                                                 std::span<std::byte const> suffix,
                                                                 TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());

    auto combined = it->entry.value;
    combined.insert(combined.end(), suffix.begin(), suffix.end());
    return MutateExisting(it, std::move(combined), it->entry.flags, it->entry.expiry);
}

std::expected<CasToken, StorageError> InMemoryLruStorage::Prepend(std::string_view key,
                                                                  std::span<std::byte const> prefix,
                                                                  TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());

    std::vector<std::byte> combined;
    combined.reserve(prefix.size() + it->entry.value.size());
    combined.insert(combined.end(), prefix.begin(), prefix.end());
    combined.insert(combined.end(), it->entry.value.begin(), it->entry.value.end());
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
        return std::unexpected(MakeKeyNotFound());
    if (it->entry.cas != expected)
        return std::unexpected(MakeCasMismatch());
    return MutateExisting(it, std::move(value), flags, expiry);
}

std::expected<IStorage::IncrResult, StorageError> InMemoryLruStorage::IncrementOrInitialize(std::string_view key,
                                                                                            std::int64_t delta,
                                                                                            TimePoint now)
{
    auto const it = FindAlive(key, now);
    if (it == _lru.end())
        return std::unexpected(MakeKeyNotFound());

    // Decode the existing value as ASCII unsigned 64-bit.
    auto const& bytes = it->entry.value;
    std::string asText;
    asText.reserve(bytes.size());
    for (auto const b: bytes)
        asText.push_back(static_cast<char>(b));

    std::uint64_t current = 0;
    auto const [_, ec] = std::from_chars(asText.data(), asText.data() + asText.size(), current);
    if (ec != std::errc {} || asText.empty())
        return std::unexpected(MakeInvalidArgument("existing value is not numeric"));

    std::uint64_t updated = current;
    if (delta >= 0)
        updated += static_cast<std::uint64_t>(delta);
    else
    {
        auto const absDelta = static_cast<std::uint64_t>(-(delta + 1)) + 1; // safe |INT64_MIN|
        updated = current > absDelta ? current - absDelta : 0;              // saturating
    }

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
        return std::unexpected(MakeKeyNotFound());
    EraseAt(it);
    return {};
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
        _bytesUsed -= nodeIt->entry.value.size();
        nodeIt->entry = std::move(entry);
        _bytesUsed += nodeIt->entry.value.size();
        _lru.splice(_lru.begin(), _lru, nodeIt);
        EvictToFit();
        return;
    }
    Node node;
    node.key = std::string { key };
    node.entry = std::move(entry);
    auto const size = node.entry.value.size();
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

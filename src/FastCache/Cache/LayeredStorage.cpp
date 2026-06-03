// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/LayeredStorage.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

LayeredStorage::LayeredStorage(std::unique_ptr<InMemoryLruStorage> l1Cache, std::unique_ptr<IStorage> l2Backing) noexcept:
    _l1 { std::move(l1Cache) },
    _l2 { std::move(l2Backing) }
{
}

void LayeredStorage::MirrorIntoL1(std::string_view key, CacheEntry entry)
{
    _l1->InsertVerbatim(key, std::move(entry));
}

void LayeredStorage::DropFromL1(std::string_view key)
{
    _l1->EraseIfPresent(key);
}

std::expected<GetResult, StorageError> LayeredStorage::LoadFromL2AndMirror(std::string_view key, TimePoint now)
{
    auto l2Got = _l2->Get(key, now);
    if (!l2Got.has_value())
        return std::unexpected(l2Got.error());
    if (!l2Got->found)
        return *l2Got;
    MirrorIntoL1(key, l2Got->entry);
    return *l2Got;
}

std::expected<CasToken, StorageError> LayeredStorage::MirrorL2WriteResult(std::string_view key,
                                                                          CasToken l2Cas,
                                                                          TimePoint now)
{
    // After an L2 write succeeded with the given CAS, fetch the
    // freshly-stored entry from L2 so the L1 mirror gets every field
    // (flags, expiry, generation, lastAccess, stale) verbatim. Use Peek,
    // not Get: this is internal write-through bookkeeping, not a client
    // read, so it must not stamp lastAccess (Get would overwrite it to the
    // sentinel `now` passed here) or inflate hit stats. L2 is canonical.
    auto fetched = _l2->Peek(key, now);
    if (!fetched.has_value())
        return std::unexpected(fetched.error());
    if (fetched->found)
        MirrorIntoL1(key, fetched->entry);
    else
        DropFromL1(key);
    return l2Cas;
}

std::expected<GetResult, StorageError> LayeredStorage::Get(std::string_view key, TimePoint now)
{
    ++_stats.cmdGet;
    auto l1Got = _l1->Get(key, now);
    if (!l1Got.has_value())
        return std::unexpected(l1Got.error());
    if (l1Got->found)
    {
        ++_stats.getHits;
        return *l1Got;
    }
    // L1 miss: consult L2 and mirror.
    auto fromL2 = LoadFromL2AndMirror(key, now);
    if (!fromL2.has_value())
        return std::unexpected(fromL2.error());
    if (fromL2->found)
        ++_stats.getHits;
    else
        ++_stats.getMisses;
    return *fromL2;
}

std::expected<CasToken, StorageError> LayeredStorage::Set(std::string_view key,
                                                          std::vector<std::byte> value,
                                                          std::uint32_t flags,
                                                          TimePoint expiry)
{
    ++_stats.cmdSet;
    auto const cas = _l2->Set(key, std::move(value), flags, expiry);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    // Use TimePoint::min() as the "now" when re-fetching: Set just
    // stored the entry, so even an entry expiring at min() would be
    // returned by L2.Get(now=min()) — we want the value back, not its
    // alive-at-now decision. CowTreeStorage / InMemoryLruStorage both
    // treat expiry <= now as expired, so use a sentinel past instead.
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<CasToken, StorageError> LayeredStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    ++_stats.cmdSet;
    auto const cas = _l2->Add(key, std::move(value), flags, expiry, now);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<CasToken, StorageError> LayeredStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    ++_stats.cmdSet;
    auto const cas = _l2->Replace(key, std::move(value), flags, expiry, now);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<CasToken, StorageError> LayeredStorage::Append(std::string_view key,
                                                             std::span<std::byte const> suffix,
                                                             CasToken expected,
                                                             TimePoint now)
{
    ++_stats.cmdSet;
    auto const cas = _l2->Append(key, suffix, expected, now);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<CasToken, StorageError> LayeredStorage::Prepend(std::string_view key,
                                                              std::span<std::byte const> prefix,
                                                              CasToken expected,
                                                              TimePoint now)
{
    ++_stats.cmdSet;
    auto const cas = _l2->Prepend(key, prefix, expected, now);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<CasToken, StorageError> LayeredStorage::CompareAndSwap(std::string_view key,
                                                                     CasToken expected,
                                                                     std::vector<std::byte> value,
                                                                     std::uint32_t flags,
                                                                     TimePoint expiry,
                                                                     TimePoint now)
{
    ++_stats.cmdSet;
    auto const cas = _l2->CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<IStorage::IncrResult, StorageError> LayeredStorage::IncrementOrInitialize(std::string_view key,
                                                                                        std::uint64_t magnitude,
                                                                                        bool decrement,
                                                                                        TimePoint now)
{
    ++_stats.cmdSet;
    auto const r = _l2->IncrementOrInitialize(key, magnitude, decrement, now);
    if (!r.has_value())
        return std::unexpected(r.error());
    // Refresh L1 with the new entry — IncrementOrInitialize mutates
    // L2 and yields a new CAS; mirror that into L1 via a fetch.
    auto const mirror = MirrorL2WriteResult(key, r->cas, TimePoint::min());
    if (!mirror.has_value())
        return std::unexpected(mirror.error());
    return *r;
}

std::expected<void, StorageError> LayeredStorage::Delete(std::string_view key, TimePoint now)
{
    auto const r = _l2->Delete(key, now);
    // Drop from L1 even if L2 reported KeyNotFound — the L1 mirror
    // should never outlive the canonical entry. Bypass the per-key
    // existence check because we don't care whether L1 had it.
    DropFromL1(key);
    return r;
}

std::expected<CasToken, StorageError> LayeredStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    auto const cas = _l2->Touch(key, newExpiry, now);
    if (!cas.has_value())
    {
        // Keep L1 consistent on miss: the entry's identity changed only
        // if it existed in L2, so on miss we leave L1 alone — a later
        // Get will either find or miss it consistently with L2.
        return std::unexpected(cas.error());
    }
    // Re-fetch from L2 so the L1 mirror picks up the new expiry/CAS.
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<GetResult, StorageError> LayeredStorage::Peek(std::string_view key, TimePoint now)
{
    // Non-mutating across both tiers: L1 first, fall through to L2. No
    // mirror is populated (a Peek must leave observable state untouched).
    auto l1 = _l1->Peek(key, now);
    if (!l1.has_value())
        return std::unexpected(l1.error());
    if (l1->found)
        return *l1;
    return _l2->Peek(key, now);
}

std::expected<CasToken, StorageError> LayeredStorage::MarkStale(std::string_view key,
                                                                std::optional<TimePoint> newExpiry,
                                                                TimePoint now)
{
    auto const cas = _l2->MarkStale(key, newExpiry, now);
    if (!cas.has_value())
        return std::unexpected(cas.error());
    return MirrorL2WriteResult(key, *cas, TimePoint::min());
}

std::expected<GetResult, StorageError> LayeredStorage::GetAndTouch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    // Refresh the expiry (write-through to L2, mirror into L1) then read the
    // refreshed entry back through the tier. The atomicity boundary is the
    // enclosing ShardedStorage's per-shard lock; on the unwrapped reactor
    // there is no concurrent writer to interleave between the touch and read.
    auto const touched = Touch(key, newExpiry, now);
    if (!touched.has_value())
        return std::unexpected(touched.error());
    return Get(key, now);
}

std::expected<void, StorageError> LayeredStorage::CompareAndDelete(std::string_view key, CasToken expected, TimePoint now)
{
    // Peek falls through to L2, so the CAS compare sees the canonical entry
    // even when it was evicted from the L1 mirror.
    auto const peeked = Peek(key, now);
    if (!peeked.has_value())
        return std::unexpected(peeked.error());
    if (!peeked->found)
        return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
    if (peeked->entry.cas != expected)
        return std::unexpected(MakeStorageError(StorageErrorCode::CasMismatch));
    return Delete(key, now);
}

void LayeredStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    _l2->FlushWithGeneration(effectiveAt);
    _l1->FlushWithGeneration(effectiveAt);
}

std::size_t LayeredStorage::PurgeExpired(TimePoint now)
{
    // L2 is canonical for the purge count — counts entries actually
    // removed from the disk file. L1's purge is opportunistic.
    auto const l2Count = _l2->PurgeExpired(now);
    static_cast<void>(_l1->PurgeExpired(now));
    return l2Count;
}

StorageStats LayeredStorage::Snapshot() const noexcept
{
    // Canonical state (itemCount, bytesUsed, bytesLimit) comes from L2.
    // The LayeredStorage-level cmdGet/cmdSet/getHits/getMisses counters
    // track calls into this composite, not into the underlying tiers.
    auto l2Stats = _l2->Snapshot();
    l2Stats.cmdGet = _stats.cmdGet;
    l2Stats.cmdSet = _stats.cmdSet;
    l2Stats.getHits = _stats.getHits;
    l2Stats.getMisses = _stats.getMisses;
    // Aggregate evictions so users can see both tiers' churn.
    auto const l1Stats = _l1->Snapshot();
    l2Stats.evictions += l1Stats.evictions;
    return l2Stats;
}

void LayeredStorage::Resize(std::size_t newL1MaxBytes)
{
    _l1->Resize(newL1MaxBytes);
}

} // namespace FastCache

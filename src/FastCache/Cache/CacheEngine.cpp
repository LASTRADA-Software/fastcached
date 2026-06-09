// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/SetCodec.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    /// memcached's threshold: any exptime above 30 days is interpreted as a
    /// UNIX timestamp.
    constexpr std::uint32_t ExptimeAbsoluteThreshold = 60U * 60U * 24U * 30U;

} // namespace

CacheEngine::CacheEngine(IStorage& storage, IClock& clock, IWallClock& wallClock) noexcept:
    _storage { storage },
    _clock { clock },
    _wallClock { wallClock }
{
}

TimePoint CacheEngine::ExpiryFromExptime(std::uint32_t exptime) const noexcept
{
    if (exptime == 0)
        return TimePoint::max();

    auto const now = _clock.Now();
    if (exptime <= ExptimeAbsoluteThreshold)
        return now + std::chrono::seconds { exptime };

    // Absolute UNIX timestamp. Anchor against the injected wall clock so
    // tests can drive deterministic absolute-time TTLs via
    // ManualWallClock (the legacy code reached for
    // std::chrono::system_clock::now() inline, which was both a DI
    // violation and made the EXPIREAT wire path untestable).
    auto const sysNow = _wallClock.Now();
    auto const sysSeconds = std::chrono::duration_cast<std::chrono::seconds>(sysNow.time_since_epoch()).count();
    auto const delta = static_cast<std::int64_t>(exptime) - sysSeconds;
    if (delta <= 0)
        return now;
    return now + std::chrono::seconds { delta };
}

std::expected<GetResult, StorageError> CacheEngine::Get(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Get");
    return _storage.Get(key, _clock.Now());
}

std::expected<GetResult, StorageError> CacheEngine::Peek(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Peek");
    return _storage.Peek(key, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Set(std::string_view key,
                                                       std::vector<std::byte> value,
                                                       std::uint32_t flags,
                                                       std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Set");
    return _storage.Set(key, std::move(value), flags, ExpiryFromExptime(exptime));
}

std::expected<CasToken, StorageError> CacheEngine::SetWithDeadline(std::string_view key,
                                                                   std::vector<std::byte> value,
                                                                   std::uint32_t flags,
                                                                   TimePoint deadline)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetWithDeadline");
    return _storage.Set(key, std::move(value), flags, deadline);
}

std::expected<CasToken, StorageError> CacheEngine::Add(std::string_view key,
                                                       std::vector<std::byte> value,
                                                       std::uint32_t flags,
                                                       std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Add");
    return _storage.Add(key, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::AddWithDeadline(std::string_view key,
                                                                   std::vector<std::byte> value,
                                                                   std::uint32_t flags,
                                                                   TimePoint deadline)
{
    FC_ZONE_SCOPED_N("CacheEngine::AddWithDeadline");
    return _storage.Add(key, std::move(value), flags, deadline, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Replace(std::string_view key,
                                                           std::vector<std::byte> value,
                                                           std::uint32_t flags,
                                                           std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Replace");
    return _storage.Replace(key, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::ReplaceWithDeadline(std::string_view key,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       TimePoint deadline)
{
    FC_ZONE_SCOPED_N("CacheEngine::ReplaceWithDeadline");
    return _storage.Replace(key, std::move(value), flags, deadline, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Append(std::string_view key,
                                                          std::span<std::byte const> suffix,
                                                          CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::Append");
    return _storage.Append(key, suffix, expected, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Prepend(std::string_view key,
                                                           std::span<std::byte const> prefix,
                                                           CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::Prepend");
    return _storage.Prepend(key, prefix, expected, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::CompareAndSwap(
    std::string_view key, CasToken expected, std::vector<std::byte> value, std::uint32_t flags, std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::CompareAndSwap");
    return _storage.CompareAndSwap(key, expected, std::move(value), flags, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<IStorage::IncrResult, StorageError> CacheEngine::Increment(std::string_view key, std::uint64_t delta)
{
    FC_ZONE_SCOPED_N("CacheEngine::Increment");
    // The full uint64 delta is forwarded verbatim — memcached increments wrap
    // modulo 2^64, and a signed cast would alias deltas >= 2^63 to decrements.
    return _storage.IncrementOrInitialize(key, delta, /*decrement=*/false, _clock.Now());
}

std::expected<IStorage::IncrResult, StorageError> CacheEngine::Decrement(std::string_view key, std::uint64_t delta)
{
    FC_ZONE_SCOPED_N("CacheEngine::Decrement");
    // Pass the magnitude + direction rather than a negated signed delta:
    // negating INT64_MIN (delta == 2^63) would be signed-overflow UB.
    return _storage.IncrementOrInitialize(key, delta, /*decrement=*/true, _clock.Now());
}

std::expected<void, StorageError> CacheEngine::Delete(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Delete");
    return _storage.Delete(key, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Touch(std::string_view key, std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::Touch");
    return _storage.Touch(key, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::TouchAt(std::string_view key, TimePoint newExpiry)
{
    FC_ZONE_SCOPED_N("CacheEngine::TouchAt");
    return _storage.Touch(key, newExpiry, _clock.Now());
}

std::expected<std::optional<CacheEngine::TtlResult>, StorageError> CacheEngine::Ttl(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::Ttl");
    auto const now = _clock.Now();
    auto const expiry = _storage.PeekExpiry(key, now);
    if (!expiry.has_value())
        return std::unexpected(expiry.error());
    if (!expiry->has_value())
        return std::optional<TtlResult> {};
    auto const deadline = **expiry;
    if (deadline == TimePoint::max())
        return std::optional<TtlResult> { TtlResult { .hasExpiry = false, .remaining = Duration { 0 } } };
    auto const remaining = deadline > now ? (deadline - now) : Duration { 0 };
    return std::optional<TtlResult> { TtlResult { .hasExpiry = true, .remaining = remaining } };
}

std::expected<bool, StorageError> CacheEngine::ClearExpiry(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::ClearExpiry");
    return _storage.ClearExpiry(key, _clock.Now());
}

std::expected<GetResult, StorageError> CacheEngine::GetAndTouch(std::string_view key, std::uint32_t exptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::GetAndTouch");
    return _storage.GetAndTouch(key, ExpiryFromExptime(exptime), _clock.Now());
}

std::expected<void, StorageError> CacheEngine::CompareAndDelete(std::string_view key, CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::CompareAndDelete");
    return _storage.CompareAndDelete(key, expected, _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::MarkStale(std::string_view key, std::optional<std::uint32_t> newExptime)
{
    FC_ZONE_SCOPED_N("CacheEngine::MarkStale");
    std::optional<TimePoint> newExpiry;
    if (newExptime.has_value())
        newExpiry = ExpiryFromExptime(*newExptime);
    return _storage.MarkStale(key, newExpiry, _clock.Now());
}

void CacheEngine::FlushAll(std::uint32_t delaySeconds)
{
    auto const effectiveAt = delaySeconds == 0 ? _clock.Now() : _clock.Now() + std::chrono::seconds { delaySeconds };
    _storage.FlushWithGeneration(effectiveAt);
}

std::expected<CasToken, StorageError> CacheEngine::Update(
    std::string_view key, std::function<std::expected<IStorage::UpdateOutcome, StorageError>(GetResult const&)> const& fn)
{
    FC_ZONE_SCOPED_N("CacheEngine::Update");
    return _storage.Update(key, fn, _clock.Now());
}

namespace
{
    /// Decode the current entry as a set into `members`. Returns WrongType when
    /// the entry exists but is not a set, KeyNotFound when absent (the caller
    /// decides whether absence is an error or an empty set).
    /// @param current The guarded read of the entry.
    /// @param members Receives the decoded members.
    [[nodiscard]] std::expected<bool, StorageError> LoadSet(GetResult const& current, std::vector<std::string>& members)
    {
        members.clear();
        if (!current.found)
            return false; // absent — treat as empty set.
        if (!SetCodec::IsSet(current.entry.flags))
            return std::unexpected(MakeStorageError(StorageErrorCode::WrongType));
        if (!SetCodec::Decode(current.entry.ValueBytes(), members))
            return std::unexpected(MakeStorageError(StorageErrorCode::Corrupt));
        return true;
    }

    /// Read-only set lookup outside the guarded RMW: Peek the key, decode it, and
    /// hand the members to `consume`. Centralises the WrongType / decode handling
    /// shared by SMEMBERS / SISMEMBER / SMISMEMBER / SCARD.
    template <typename Consume>
    [[nodiscard]] auto WithSet(CacheEngine& engine, std::string_view key, Consume consume)
        -> std::expected<decltype(consume(std::declval<std::vector<std::string> const&>())), StorageError>
    {
        auto const peek = engine.Peek(key);
        if (!peek.has_value())
            return std::unexpected(peek.error());
        std::vector<std::string> members;
        if (peek->found)
        {
            if (!SetCodec::IsSet(peek->entry.flags))
                return std::unexpected(MakeStorageError(StorageErrorCode::WrongType));
            if (!SetCodec::Decode(peek->entry.ValueBytes(), members))
                return std::unexpected(MakeStorageError(StorageErrorCode::Corrupt));
        }
        return consume(members);
    }

} // namespace

std::expected<std::int64_t, StorageError> CacheEngine::SetAdd(std::string_view key, std::span<std::string const> members)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetAdd");
    std::int64_t added = 0;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        std::vector<std::string> existing;
        auto const loaded = LoadSet(current, existing);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        // Build a hash-membership view of `existing` so per-member tests are
        // O(1) and the answer is independent of insertion order. Using
        // std::ranges::binary_search + push_back inside the loop broke the
        // sorted invariant after the first push, turning subsequent searches
        // into UB and over-counting `added` when the input contains duplicates
        // or members that sort before earlier-added ones.
        std::unordered_set<std::string_view> seen;
        seen.reserve(existing.size() + members.size());
        for (auto const& e: existing)
            seen.insert(e);
        added = 0;
        for (auto const& m: members)
            if (seen.insert(m).second) // newly inserted -> not previously in set
            {
                existing.push_back(m);
                ++added;
            }
        if (added == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        SetCodec::Normalise(existing);
        return IStorage::UpdateOutcome { .value = SetCodec::Encode(existing),
                                         .flags = SetCodec::FcTypeSet,
                                         .action = IStorage::UpdateAction::Store };
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return added;
}

std::expected<std::int64_t, StorageError> CacheEngine::SetRemove(std::string_view key, std::span<std::string const> members)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetRemove");
    std::int64_t removed = 0;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        std::vector<std::string> existing;
        auto const loaded = LoadSet(current, existing);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        removed = 0;
        for (auto const& m: members)
        {
            auto const before = existing.size();
            std::erase(existing, m);
            if (existing.size() != before)
                ++removed;
        }
        if (removed == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        if (existing.empty())
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Delete };
        return IStorage::UpdateOutcome { .value = SetCodec::Encode(existing),
                                         .flags = SetCodec::FcTypeSet,
                                         .action = IStorage::UpdateAction::Store };
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return removed;
}

std::expected<std::vector<std::string>, StorageError> CacheEngine::SetMembers(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetMembers");
    return WithSet(*this, key, [](std::vector<std::string> const& members) { return members; });
}

std::expected<bool, StorageError> CacheEngine::SetIsMember(std::string_view key, std::string_view member)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetIsMember");
    return WithSet(*this, key, [member](std::vector<std::string> const& members) {
        return std::ranges::binary_search(members, member);
    });
}

std::expected<std::vector<bool>, StorageError> CacheEngine::SetMIsMember(std::string_view key,
                                                                         std::span<std::string const> members)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetMIsMember");
    return WithSet(*this, key, [members](std::vector<std::string> const& existing) {
        std::vector<bool> out;
        out.reserve(members.size());
        for (auto const& m: members)
            out.push_back(std::ranges::binary_search(existing, m));
        return out;
    });
}

std::expected<std::int64_t, StorageError> CacheEngine::SetCard(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetCard");
    return WithSet(
        *this, key, [](std::vector<std::string> const& members) { return static_cast<std::int64_t>(members.size()); });
}

std::expected<std::vector<std::string>, StorageError> CacheEngine::SetPop(std::string_view key, std::size_t count)
{
    FC_ZONE_SCOPED_N("CacheEngine::SetPop");
    std::vector<std::string> popped;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        std::vector<std::string> existing;
        auto const loaded = LoadSet(current, existing);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        popped.clear();
        // Deterministic "pop": take from the sorted front. fastcached has no
        // injected RNG seam, and a cache set's pop order is not contractual;
        // taking the lexicographically smallest members keeps it reproducible.
        auto const take = std::min(count, existing.size());
        popped.assign(existing.begin(), existing.begin() + static_cast<std::ptrdiff_t>(take));
        existing.erase(existing.begin(), existing.begin() + static_cast<std::ptrdiff_t>(take));
        if (take == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        if (existing.empty())
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Delete };
        return IStorage::UpdateOutcome { .value = SetCodec::Encode(existing),
                                         .flags = SetCodec::FcTypeSet,
                                         .action = IStorage::UpdateAction::Store };
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return popped;
}

} // namespace FastCache

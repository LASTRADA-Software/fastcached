// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/SetCodec.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
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

std::expected<CasToken, StorageError> CacheEngine::PeekCas(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::PeekCas");
    auto const result = _storage.Peek(key, _clock.Now());
    if (!result.has_value())
        return std::unexpected(result.error());
    return result->found ? result->entry.cas : CasToken { 0 };
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

std::expected<CasToken, StorageError> CacheEngine::ConcatGuarded(std::string_view key,
                                                                 std::span<std::byte const> extra,
                                                                 CasToken expected,
                                                                 bool atFront)
{
    // APPEND/PREPEND are string-only in redis and a raw byte concat on a typed
    // value (set/stream) would silently corrupt its encoded blob while keeping
    // the type tag — a later decode then fails irrecoverably. The type guard,
    // the CAS precondition, and the concat must be atomic with respect to a
    // concurrent SET/DEL, so the whole sequence runs inside the storage's
    // per-key Update boundary rather than as a separate Peek + Append.
    return _storage.Update(
        key,
        [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
            if (current.found)
            {
                if (SetCodec::IsSet(current.entry.flags) || StreamCodec::IsStream(current.entry.flags))
                    return std::unexpected(MakeStorageError(StorageErrorCode::WrongType));
                if (expected != CasToken { 0 } && current.entry.cas != expected)
                    return std::unexpected(MakeStorageError(StorageErrorCode::CasMismatch));
            }
            else if (expected != CasToken { 0 })
            {
                // A CAS precondition on a missing key cannot hold.
                return std::unexpected(MakeStorageError(StorageErrorCode::CasMismatch));
            }
            auto const existing = current.found ? current.entry.ValueBytes() : std::span<std::byte const> {};
            std::vector<std::byte> combined;
            combined.reserve(existing.size() + extra.size());
            auto const& first = atFront ? extra : existing;
            auto const& second = atFront ? existing : extra;
            combined.insert(combined.end(), first.begin(), first.end());
            combined.insert(combined.end(), second.begin(), second.end());
            // Preserve the prior entry's flags (a plain string carries 0); a
            // missing key is created as a plain string, matching memcached's
            // append/prepend-creates-on-absent behaviour.
            return IStorage::UpdateOutcome { .value = std::move(combined),
                                             .flags = current.found ? current.entry.flags : 0U,
                                             .action = IStorage::UpdateAction::Store };
        },
        _clock.Now());
}

std::expected<CasToken, StorageError> CacheEngine::Append(std::string_view key,
                                                          std::span<std::byte const> suffix,
                                                          CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::Append");
    return ConcatGuarded(key, suffix, expected, /*atFront=*/false);
}

std::expected<CasToken, StorageError> CacheEngine::Prepend(std::string_view key,
                                                           std::span<std::byte const> prefix,
                                                           CasToken expected)
{
    FC_ZONE_SCOPED_N("CacheEngine::Prepend");
    return ConcatGuarded(key, prefix, expected, /*atFront=*/true);
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

// -- redis stream type ------------------------------------------------------

namespace
{
    using StreamCodec::ConsumerGroup;
    using StreamCodec::PendingEntry;
    using StreamCodec::Stream;
    using StreamCodec::StreamEntry;
    using StreamCodec::StreamId;

    /// Decode the current entry as a stream into `stream`. Returns WrongType
    /// when the entry exists but is not a stream, false (with an empty stream)
    /// when absent — the caller decides whether absence is an error.
    /// @param current The guarded read of the entry.
    /// @param stream  Receives the decoded stream.
    /// @return True if a stream was present and decoded; false if absent.
    [[nodiscard]] std::expected<bool, StorageError> LoadStream(GetResult const& current, Stream& stream)
    {
        stream = Stream {};
        if (!current.found)
            return false;
        if (!StreamCodec::IsStream(current.entry.flags))
            return std::unexpected(MakeStorageError(StorageErrorCode::WrongType));
        if (!StreamCodec::Decode(current.entry.ValueBytes(), stream))
            return std::unexpected(MakeStorageError(StorageErrorCode::Corrupt));
        return true;
    }

    /// Read-only stream lookup outside the guarded RMW: Peek the key, decode it,
    /// and hand the stream to `consume`. Centralises the WrongType / decode
    /// handling shared by the read-only stream queries. `consume` is given a
    /// pointer that is null when the key is absent and MUST itself return a
    /// `std::expected<T, StorageError>` (so a query that treats absence as
    /// NOGROUP/KeyNotFound can signal it); `WithStream` returns that type
    /// directly without extra wrapping.
    /// @param engine  The owning engine.
    /// @param key     Stream key.
    /// @param consume Callback receiving the decoded stream (or nullptr).
    template <typename Consume>
    [[nodiscard]] auto WithStream(CacheEngine& engine, std::string_view key, Consume consume)
        -> decltype(consume(std::declval<Stream const*>()))
    {
        using Result = decltype(consume(std::declval<Stream const*>()));
        auto const peek = engine.Peek(key);
        if (!peek.has_value())
            return Result { std::unexpect, peek.error() };
        if (!peek->found)
            return consume(static_cast<Stream const*>(nullptr));
        if (!StreamCodec::IsStream(peek->entry.flags))
            return Result { std::unexpect, MakeStorageError(StorageErrorCode::WrongType) };
        Stream stream;
        if (!StreamCodec::Decode(peek->entry.ValueBytes(), stream))
            return Result { std::unexpect, MakeStorageError(StorageErrorCode::Corrupt) };
        return consume(&stream);
    }

    /// Store the (possibly mutated) stream back. Once a stream key exists it is
    /// persisted as a (possibly empty) stream and is NEVER auto-deleted here:
    /// redis only removes a stream key on an explicit DEL/UNLINK or TTL expiry,
    /// not as a side effect of XDEL/XTRIM emptying it or XGROUP DESTROY removing
    /// its last group. (An earlier heuristic that deleted an empty, never-
    /// appended stream wrongly destroyed a stream created via XGROUP CREATE ...
    /// MKSTREAM once its last group was dropped.)
    /// @param stream The stream to persist.
    /// @return The Store outcome for an Update callback.
    [[nodiscard]] IStorage::UpdateOutcome PersistStream(Stream const& stream)
    {
        return IStorage::UpdateOutcome { .value = StreamCodec::Encode(stream),
                                         .flags = StreamCodec::FcTypeStream,
                                         .action = IStorage::UpdateAction::Store };
    }

    /// Locate a consumer group by name within a decoded stream.
    /// @param stream The decoded stream.
    /// @param name   Group name.
    /// @return Pointer to the group, or nullptr if absent.
    [[nodiscard]] ConsumerGroup* FindGroup(Stream& stream, std::string_view name)
    {
        auto const it = std::ranges::find(stream.groups, name, &ConsumerGroup::name);
        return it == stream.groups.end() ? nullptr : &*it;
    }

    /// Ensure `consumer` is recorded in `group` (sorted, unique).
    /// @param group    The group.
    /// @param consumer Consumer name.
    /// @return True if the consumer was newly added.
    bool TouchConsumer(ConsumerGroup& group, std::string_view consumer)
    {
        auto const it = std::ranges::lower_bound(group.consumers, consumer);
        if (it != group.consumers.end() && *it == consumer)
            return false;
        group.consumers.emplace(it, consumer);
        return true;
    }

    /// Apply a trim directive to a decoded stream in place. Trimming does NOT
    /// advance `maxDeletedId`: redis only moves max-deleted-entry-id on explicit
    /// deletions (XDEL) and XSETID, never as a side effect of MAXLEN/MINID
    /// trimming, so XINFO STREAM keeps reporting the prior max-deleted ID.
    /// @param stream The stream to trim.
    /// @param trim   The directive.
    /// @return Number of entries removed.
    std::int64_t ApplyTrim(Stream& stream, CacheEngine::StreamTrim const& trim)
    {
        auto const before = stream.entries.size();
        if (trim.strategy == CacheEngine::StreamTrim::Strategy::MaxLen)
        {
            if (stream.entries.size() > trim.threshold)
            {
                auto const drop = stream.entries.size() - trim.threshold;
                stream.entries.erase(stream.entries.begin(), stream.entries.begin() + static_cast<std::ptrdiff_t>(drop));
            }
        }
        else
        {
            auto const cut = std::ranges::lower_bound(stream.entries, trim.minId, {}, &StreamEntry::id);
            stream.entries.erase(stream.entries.begin(), cut);
        }
        return static_cast<std::int64_t>(before - stream.entries.size());
    }

} // namespace

std::uint64_t CacheEngine::WallNowMs() const noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(_wallClock.Now().time_since_epoch()).count());
}

std::expected<StreamId, StorageError> CacheEngine::StreamAdd(std::string_view key,
                                                             std::optional<StreamId> requestedId,
                                                             bool seqAuto,
                                                             std::span<std::pair<std::string, std::string> const> fields,
                                                             std::optional<StreamTrim> trim,
                                                             bool noMkStream)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamAdd");
    auto const nowMs = WallNowMs();
    StreamId assigned {};
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        if (!*loaded && noMkStream)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));

        // Resolve the entry ID. Auto (`*`): max(nowMs, lastId.ms); the sequence
        // continues within the same ms or resets. Explicit ms with `*` seq
        // (`<ms>-*`): auto-sequence within that ms. Fully explicit: must be
        // strictly greater than the current last ID.
        // Helper: the next sequence within `ms`, given the current last ID.
        // When the sequence would wrap past UINT64_MAX within the same ms, redis
        // rejects the append ("sequence number overflow") rather than wrapping to
        // a smaller (non-monotonic) ID; surface that as InvalidArgument.
        auto const nextSeqWithin = [&](std::uint64_t ms) -> std::expected<std::uint64_t, StorageError> {
            if (ms != stream.lastId.ms)
                return std::uint64_t { 0 };
            if (stream.lastId.seq == std::numeric_limits<std::uint64_t>::max())
                return std::unexpected(MakeStorageError(StorageErrorCode::InvalidArgument));
            return stream.lastId.seq + 1;
        };

        if (!requestedId.has_value())
        {
            assigned.ms = std::max(nowMs, stream.lastId.ms);
            auto const seq = nextSeqWithin(assigned.ms);
            if (!seq.has_value())
                return std::unexpected(seq.error());
            assigned.seq = *seq;
        }
        else if (seqAuto)
        {
            assigned.ms = requestedId->ms;
            if (assigned.ms < stream.lastId.ms)
                return std::unexpected(MakeStorageError(StorageErrorCode::InvalidArgument));
            auto const seq = nextSeqWithin(assigned.ms);
            if (!seq.has_value())
                return std::unexpected(seq.error());
            assigned.seq = *seq;
        }
        else
        {
            assigned = *requestedId;
        }
        if (assigned == StreamId::Min())
            return std::unexpected(MakeStorageError(StorageErrorCode::InvalidArgument)); // 0-0 is illegal.
        if ((stream.entriesAdded != 0 || !stream.entries.empty() || stream.lastId != StreamId::Min())
            && assigned <= stream.lastId)
            return std::unexpected(MakeStorageError(StorageErrorCode::InvalidArgument));

        StreamEntry entry;
        entry.id = assigned;
        entry.fields.assign(fields.begin(), fields.end());
        stream.entries.push_back(std::move(entry));
        stream.lastId = assigned;
        ++stream.entriesAdded;
        if (trim.has_value())
            (void) ApplyTrim(stream, *trim);
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return assigned;
}

std::expected<std::int64_t, StorageError> CacheEngine::StreamLen(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamLen");
    return WithStream(*this, key, [](Stream const* stream) -> std::expected<std::int64_t, StorageError> {
        return stream == nullptr ? std::int64_t { 0 } : static_cast<std::int64_t>(stream->entries.size());
    });
}

std::expected<std::vector<StreamEntry>, StorageError> CacheEngine::StreamRange(
    std::string_view key, StreamId start, StreamId end, std::size_t count, bool reverse)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamRange");
    return WithStream(*this, key, [&](Stream const* stream) -> std::expected<std::vector<StreamEntry>, StorageError> {
        std::vector<StreamEntry> out;
        if (stream == nullptr || start > end)
            return out;
        auto const lo = std::ranges::lower_bound(stream->entries, start, {}, &StreamEntry::id);
        auto const hi = std::ranges::upper_bound(stream->entries, end, {}, &StreamEntry::id);
        if (!reverse)
            for (auto it = lo; it != hi && (count == 0 || out.size() < count); ++it)
                out.push_back(*it);
        else
            for (auto it = hi; it != lo && (count == 0 || out.size() < count);)
                out.push_back(*--it);
        return out;
    });
}

std::expected<std::vector<StreamEntry>, StorageError> CacheEngine::StreamRead(std::string_view key,
                                                                              StreamId after,
                                                                              std::size_t count)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamRead");
    // Nothing can be strictly after the maximal ID. Next() saturates at Max(),
    // so without this guard a cursor already at Max() would form the inclusive
    // range [Max, Max] and re-return the maximal entry on every poll (an
    // infinite redelivery loop for a reader parked at the top of the stream).
    if (after == StreamId::Max())
        return std::vector<StreamEntry> {};
    return StreamRange(key, after.Next(), StreamId::Max(), count, /*reverse*/ false);
}

std::expected<std::int64_t, StorageError> CacheEngine::StreamDelete(std::string_view key, std::span<StreamId const> ids)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamDelete");
    std::int64_t removed = 0;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        removed = 0;
        for (auto const& id: ids)
        {
            auto const it = std::ranges::lower_bound(stream.entries, id, {}, &StreamEntry::id);
            if (it != stream.entries.end() && it->id == id)
            {
                if (id > stream.maxDeletedId)
                    stream.maxDeletedId = id;
                stream.entries.erase(it);
                ++removed;
            }
        }
        if (removed == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return removed;
}

std::expected<std::int64_t, StorageError> CacheEngine::StreamTrimTo(std::string_view key, StreamTrim trim)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamTrimTo");
    std::int64_t evicted = 0;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        evicted = ApplyTrim(stream, trim);
        if (evicted == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return evicted;
}

std::expected<void, StorageError> CacheEngine::StreamSetId(std::string_view key,
                                                           StreamId lastId,
                                                           std::optional<std::uint64_t> entriesAdded,
                                                           std::optional<StreamId> maxDeletedId)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamSetId");
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        if (!*loaded)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        if (!stream.entries.empty() && lastId < stream.entries.back().id)
            return std::unexpected(MakeStorageError(StorageErrorCode::InvalidArgument));
        stream.lastId = lastId;
        if (entriesAdded.has_value())
            stream.entriesAdded = *entriesAdded;
        if (maxDeletedId.has_value())
            stream.maxDeletedId = *maxDeletedId;
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return {};
}

std::expected<StreamId, StorageError> CacheEngine::StreamLastId(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamLastId");
    return WithStream(*this, key, [](Stream const* stream) -> std::expected<StreamId, StorageError> {
        return stream == nullptr ? StreamId::Min() : stream->lastId;
    });
}

namespace
{
    /// Resolve a GroupStart + explicit ID into a concrete cursor ID.
    /// @param start  Where the cursor begins.
    /// @param at     Explicit ID when `start == At`.
    /// @param stream The stream (for the `$` = last-id case).
    /// @return The resolved cursor ID.
    [[nodiscard]] StreamId ResolveGroupCursor(CacheEngine::GroupStart start, StreamId at, Stream const& stream)
    {
        switch (start)
        {
            case CacheEngine::GroupStart::Beginning:
                return StreamId::Min();
            case CacheEngine::GroupStart::End:
                return stream.lastId;
            case CacheEngine::GroupStart::At:
                return at;
        }
        return StreamId::Min();
    }
} // namespace

std::expected<void, StorageError> CacheEngine::StreamGroupCreate(
    std::string_view key, std::string_view group, GroupStart start, StreamId at, bool mkStream)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamGroupCreate");
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        if (!*loaded && !mkStream)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        if (FindGroup(stream, group) != nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyExists));
        stream.groups.push_back(ConsumerGroup { .name = std::string { group },
                                                .lastDelivered = ResolveGroupCursor(start, at, stream),
                                                .entriesRead = 0,
                                                .consumers = {},
                                                .pel = {} });
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return {};
}

std::expected<void, StorageError> CacheEngine::StreamGroupSetId(std::string_view key,
                                                                std::string_view group,
                                                                GroupStart start,
                                                                StreamId at)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamGroupSetId");
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        g->lastDelivered = ResolveGroupCursor(start, at, stream);
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return {};
}

std::expected<bool, StorageError> CacheEngine::StreamGroupDestroy(std::string_view key, std::string_view group)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamGroupDestroy");
    bool removed = false;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        // XGROUP DESTROY (like every XGROUP subcommand except CREATE ... MKSTREAM)
        // requires the key to exist; redis errors rather than reporting "0 groups
        // removed" for a missing stream.
        if (!*loaded)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        auto const erased = std::erase_if(stream.groups, [group](ConsumerGroup const& g) { return g.name == group; });
        removed = erased != 0;
        if (!removed)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return removed;
}

std::expected<bool, StorageError> CacheEngine::StreamConsumerCreate(std::string_view key,
                                                                    std::string_view group,
                                                                    std::string_view consumer)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamConsumerCreate");
    bool created = false;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        created = TouchConsumer(*g, consumer);
        if (!created)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return created;
}

std::expected<std::int64_t, StorageError> CacheEngine::StreamConsumerDelete(std::string_view key,
                                                                            std::string_view group,
                                                                            std::string_view consumer)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamConsumerDelete");
    std::int64_t dropped = 0;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        dropped = static_cast<std::int64_t>(
            std::erase_if(g->pel, [consumer](PendingEntry const& p) { return p.consumer == consumer; }));
        auto const had = std::erase(g->consumers, consumer);
        if (dropped == 0 && had == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return dropped;
}

std::expected<std::vector<StreamEntry>, StorageError> CacheEngine::StreamReadGroup(std::string_view key,
                                                                                   std::string_view group,
                                                                                   std::string_view consumer,
                                                                                   std::optional<StreamId> after,
                                                                                   std::size_t count,
                                                                                   bool noAck)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamReadGroup");
    auto const nowMs = WallNowMs();
    std::vector<StreamEntry> out;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        out.clear();
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        if (!*loaded)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        // XREADGROUP registers the consumer unconditionally — even when the read
        // yields nothing — so the registration must be persisted even on the
        // otherwise-Unchanged paths below (an earlier version returned Unchanged
        // and silently dropped a brand-new consumer that read no entries).
        bool const consumerCreated = TouchConsumer(*g, consumer);

        if (!after.has_value())
        {
            // `>`: deliver new entries after the group cursor, recording them in
            // the PEL (unless NOACK) and advancing the cursor.
            auto const lo = std::ranges::upper_bound(stream.entries, g->lastDelivered, {}, &StreamEntry::id);
            for (auto it = lo; it != stream.entries.end() && (count == 0 || out.size() < count); ++it)
            {
                out.push_back(*it);
                g->lastDelivered = it->id;
                ++g->entriesRead;
                if (!noAck)
                    g->pel.push_back(PendingEntry {
                        .id = it->id, .consumer = std::string { consumer }, .deliveryTimeMs = nowMs, .deliveryCount = 1 });
            }
            if (out.empty())
                return consumerCreated ? PersistStream(stream)
                                       : IStorage::UpdateOutcome { .value = {},
                                                                   .flags = 0,
                                                                   .action = IStorage::UpdateAction::Unchanged };
            // Keep the PEL sorted by ID for linear range scans.
            std::ranges::sort(g->pel, {}, &PendingEntry::id);
            return PersistStream(stream);
        }

        // Explicit ID: re-read this consumer's own pending history strictly after
        // `after` (redis treats the supplied ID as an EXCLUSIVE cursor, so a
        // client paging with its last-seen ID does not re-receive the boundary
        // entry). Entries no longer present yield an empty field list.
        for (auto& pending: g->pel)
        {
            if (pending.consumer != consumer || pending.id <= *after)
                continue;
            if (count != 0 && out.size() >= count)
                break;
            auto const it = std::ranges::lower_bound(stream.entries, pending.id, {}, &StreamEntry::id);
            if (it != stream.entries.end() && it->id == pending.id)
                out.push_back(*it);
            else
                out.push_back(StreamEntry { .id = pending.id, .fields = {} });
        }
        // Re-reading the PEL does not mutate the stream — except that the consumer
        // may have just been registered, which must be persisted.
        return consumerCreated
                   ? PersistStream(stream)
                   : IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return out;
}

std::expected<std::int64_t, StorageError> CacheEngine::StreamAck(std::string_view key,
                                                                 std::string_view group,
                                                                 std::span<StreamId const> ids)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamAck");
    std::int64_t acked = 0;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        acked = 0;
        for (auto const& id: ids)
            acked += static_cast<std::int64_t>(std::erase_if(g->pel, [id](PendingEntry const& p) { return p.id == id; }));
        if (acked == 0)
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return acked;
}

std::expected<CacheEngine::PendingOverview, StorageError> CacheEngine::StreamPendingSummary(std::string_view key,
                                                                                            std::string_view group)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamPendingSummary");
    return WithStream(*this, key, [&](Stream const* stream) -> std::expected<PendingOverview, StorageError> {
        if (stream == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        auto const it = std::ranges::find(stream->groups, group, &ConsumerGroup::name);
        if (it == stream->groups.end())
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        PendingOverview overview;
        overview.count = it->pel.size();
        if (it->pel.empty())
            return overview;
        overview.minId = it->pel.front().id;
        overview.maxId = it->pel.back().id;
        std::vector<std::pair<std::string, std::uint64_t>> tally;
        for (auto const& p: it->pel)
        {
            auto row = std::ranges::find(tally, p.consumer, &std::pair<std::string, std::uint64_t>::first);
            if (row == tally.end())
                tally.emplace_back(p.consumer, 1);
            else
                ++row->second;
        }
        overview.perConsumer = std::move(tally);
        return overview;
    });
}

std::expected<std::vector<CacheEngine::PendingSummary>, StorageError> CacheEngine::StreamPendingRange(
    std::string_view key,
    std::string_view group,
    StreamId start,
    StreamId end,
    std::size_t count,
    std::optional<std::string_view> consumer,
    std::uint64_t minIdleMs)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamPendingRange");
    auto const nowMs = WallNowMs();
    return WithStream(*this, key, [&](Stream const* stream) -> std::expected<std::vector<PendingSummary>, StorageError> {
        if (stream == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        auto const it = std::ranges::find(stream->groups, group, &ConsumerGroup::name);
        if (it == stream->groups.end())
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        std::vector<PendingSummary> out;
        for (auto const& p: it->pel)
        {
            if (p.id < start || p.id > end)
                continue;
            if (consumer.has_value() && p.consumer != *consumer)
                continue;
            auto const idle = nowMs >= p.deliveryTimeMs ? nowMs - p.deliveryTimeMs : 0;
            if (idle < minIdleMs)
                continue;
            if (count != 0 && out.size() >= count)
                break;
            out.push_back(
                PendingSummary { .id = p.id, .consumer = p.consumer, .idleMs = idle, .deliveryCount = p.deliveryCount });
        }
        return out;
    });
}

namespace
{
    /// Shared XCLAIM/XAUTOCLAIM core: reassign matching PEL entries to
    /// `consumer`, populating `result`. Operates on a decoded, mutable stream.
    /// @param stream    The decoded stream (mutated).
    /// @param group     Target group.
    /// @param consumer  New owner.
    /// @param nowMs     Current wall-clock ms.
    /// @param minIdleMs Idle threshold a PEL entry must meet to be claimed.
    /// @param justId    Populate only IDs, not full entries.
    /// @param shouldClaim Predicate (PendingEntry) → bool selecting candidates.
    /// @param result    Receives the claimed entries/ids and deleted IDs.
    template <typename Predicate>
    void ClaimCore(Stream& stream,
                   ConsumerGroup& group,
                   std::string_view consumer,
                   std::uint64_t nowMs,
                   bool justId,
                   Predicate shouldClaim,
                   CacheEngine::ClaimResult& result)
    {
        for (auto& pending: group.pel)
        {
            if (!shouldClaim(pending))
                continue;
            auto const it = std::ranges::lower_bound(stream.entries, pending.id, {}, &StreamEntry::id);
            auto const present = it != stream.entries.end() && it->id == pending.id;
            if (!present)
            {
                // The underlying entry was deleted; XAUTOCLAIM drains it.
                result.deleted.push_back(pending.id);
                continue;
            }
            pending.consumer = std::string { consumer };
            pending.deliveryTimeMs = nowMs;
            // JUSTID explicitly does NOT bump the delivery counter (redis: the
            // entry is reassigned but not "re-delivered"), so a JUSTID-based
            // recovery sweep does not inflate retry counts toward a dead-letter
            // threshold. A normal claim does increment it.
            if (!justId)
            {
                ++pending.deliveryCount;
                result.entries.push_back(*it);
            }
            result.ids.push_back(pending.id);
        }
        // Drop PEL rows whose entries are gone.
        for (auto const& goneId: result.deleted)
            std::erase_if(group.pel, [goneId](PendingEntry const& p) { return p.id == goneId; });
    }
} // namespace

std::expected<CacheEngine::ClaimResult, StorageError> CacheEngine::StreamClaim(std::string_view key,
                                                                               std::string_view group,
                                                                               std::string_view consumer,
                                                                               std::uint64_t minIdleMs,
                                                                               std::span<StreamId const> ids,
                                                                               bool justId,
                                                                               bool force)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamClaim");
    auto const nowMs = WallNowMs();
    ClaimResult claimed;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        claimed = ClaimResult {};
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        TouchConsumer(*g, consumer);
        std::vector<StreamId> wanted { ids.begin(), ids.end() };
        // FORCE: create a PEL entry for any requested ID that exists in the
        // stream but is not currently pending, so it can be claimed below. Redis
        // uses this to recover an entry into a group that never delivered it.
        // The min-idle-time check does not gate a freshly forced entry (idle 0),
        // so seed its delivery time far enough back that the predicate accepts.
        if (force)
        {
            for (auto const& id: wanted)
            {
                auto const inPel = std::ranges::any_of(g->pel, [&](PendingEntry const& p) { return p.id == id; });
                if (inPel)
                    continue;
                auto const it = std::ranges::lower_bound(stream.entries, id, {}, &StreamEntry::id);
                if (it == stream.entries.end() || it->id != id)
                    continue; // not in the stream — nothing to force.
                g->pel.push_back(PendingEntry {
                    .id = id, .consumer = std::string { consumer }, .deliveryTimeMs = nowMs, .deliveryCount = 0 });
            }
            std::ranges::sort(g->pel, {}, &PendingEntry::id);
        }
        ClaimCore(
            stream,
            *g,
            consumer,
            nowMs,
            justId,
            [&](PendingEntry const& p) {
                if (std::ranges::find(wanted, p.id) == wanted.end())
                    return false;
                // A just-forced entry has deliveryTimeMs == nowMs (idle 0); the
                // >= comparison still admits it when minIdleMs == 0 (the common
                // FORCE case), and a non-zero min-idle naturally excludes it.
                auto const idle = nowMs >= p.deliveryTimeMs ? nowMs - p.deliveryTimeMs : 0;
                return idle >= minIdleMs;
            },
            claimed);
        if (claimed.ids.empty() && claimed.deleted.empty())
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        std::ranges::sort(g->pel, {}, &PendingEntry::id);
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return claimed;
}

std::expected<CacheEngine::ClaimResult, StorageError> CacheEngine::StreamAutoClaim(std::string_view key,
                                                                                   std::string_view group,
                                                                                   std::string_view consumer,
                                                                                   std::uint64_t minIdleMs,
                                                                                   StreamId start,
                                                                                   std::size_t count,
                                                                                   bool justId)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamAutoClaim");
    auto const nowMs = WallNowMs();
    ClaimResult claimed;
    auto const result = Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
        claimed = ClaimResult {};
        claimed.cursor = StreamId::Min();
        Stream stream;
        auto const loaded = LoadStream(current, stream);
        if (!loaded.has_value())
            return std::unexpected(loaded.error());
        auto* const g = FindGroup(stream, group);
        if (g == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        TouchConsumer(*g, consumer);
        auto const limit = count == 0 ? std::size_t { 100 } : count; // redis default COUNT is 100.
        std::size_t scanned = 0;
        ClaimCore(
            stream,
            *g,
            consumer,
            nowMs,
            justId,
            [&](PendingEntry const& p) {
                if (p.id < start)
                    return false;
                if (claimed.ids.size() >= limit)
                {
                    if (claimed.cursor == StreamId::Min())
                        claimed.cursor = p.id; // resume here next call.
                    return false;
                }
                ++scanned;
                auto const idle = nowMs >= p.deliveryTimeMs ? nowMs - p.deliveryTimeMs : 0;
                return idle >= minIdleMs;
            },
            claimed);
        (void) scanned;
        if (claimed.ids.empty() && claimed.deleted.empty())
            return IStorage::UpdateOutcome { .value = {}, .flags = 0, .action = IStorage::UpdateAction::Unchanged };
        std::ranges::sort(g->pel, {}, &PendingEntry::id);
        return PersistStream(stream);
    });
    if (!result.has_value())
        return std::unexpected(result.error());
    return claimed;
}

std::expected<CacheEngine::StreamInfo, StorageError> CacheEngine::StreamInfoOf(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamInfoOf");
    return WithStream(*this, key, [](Stream const* stream) -> std::expected<StreamInfo, StorageError> {
        if (stream == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        StreamInfo info;
        info.length = stream->entries.size();
        info.lastId = stream->lastId;
        info.maxDeletedId = stream->maxDeletedId;
        info.entriesAdded = stream->entriesAdded;
        info.groupCount = stream->groups.size();
        if (!stream->entries.empty())
        {
            info.first = stream->entries.front();
            info.last = stream->entries.back();
        }
        return info;
    });
}

std::expected<std::vector<CacheEngine::GroupInfo>, StorageError> CacheEngine::StreamGroupInfo(std::string_view key)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamGroupInfo");
    return WithStream(*this, key, [](Stream const* stream) -> std::expected<std::vector<GroupInfo>, StorageError> {
        if (stream == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        std::vector<GroupInfo> out;
        out.reserve(stream->groups.size());
        for (auto const& g: stream->groups)
            out.push_back(
                GroupInfo { .name = g.name,
                            .consumers = g.consumers.size(),
                            .pending = g.pel.size(),
                            .lastDelivered = g.lastDelivered,
                            .entriesRead = g.entriesRead,
                            // lag = entries ever added minus entries this group has read,
                            // clamped at 0 (a group created at the tail reads everything).
                            .lag = stream->entriesAdded > g.entriesRead ? stream->entriesAdded - g.entriesRead : 0 });
        return out;
    });
}

std::expected<std::vector<CacheEngine::ConsumerInfo>, StorageError> CacheEngine::StreamConsumerInfo(std::string_view key,
                                                                                                    std::string_view group)
{
    FC_ZONE_SCOPED_N("CacheEngine::StreamConsumerInfo");
    return WithStream(*this, key, [&](Stream const* stream) -> std::expected<std::vector<ConsumerInfo>, StorageError> {
        if (stream == nullptr)
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        auto const it = std::ranges::find(stream->groups, group, &ConsumerGroup::name);
        if (it == stream->groups.end())
            return std::unexpected(MakeStorageError(StorageErrorCode::KeyNotFound));
        std::vector<ConsumerInfo> out;
        out.reserve(it->consumers.size());
        for (auto const& name: it->consumers)
        {
            auto const pending = static_cast<std::uint64_t>(std::ranges::count(it->pel, name, &PendingEntry::consumer));
            out.push_back(ConsumerInfo { .name = name, .pending = pending });
        }
        return out;
    });
}

} // namespace FastCache

// SPDX-License-Identifier: Apache-2.0
#include "LocalCache.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// Compile objects carry no TTL: a key is a digest over everything that
    /// determines the object, so an entry is valid until it is evicted for space.
    /// Expiring one would only force a recompile of something still correct.
    ///
    /// `core::platform::SteadyTimePoint::max()` and NOT a default-constructed `core::platform::SteadyTimePoint`. The storage
    /// tests `entry.expiry <= now`, so the zero value means "expired before any
    /// clock reading" rather than "no deadline" -- every write would land and be
    /// unreadable, which is a cache that silently stores nothing. `CacheEngine`
    /// spells never-expires the same way.
    constexpr core::platform::SteadyTimePoint NoExpiry = core::platform::SteadyTimePoint::max();

    /// Compile values carry no memcached flags word; the framing is the wire's.
    constexpr std::uint32_t NoFlags = 0;

    /// One outcome of offering an object upstream, and which of the tier's counters it moves.
    ///
    /// A member of `CacheTierProfile` rather than a counter, because the two tiers count the same
    /// outcome on different series -- and the shared tier on none, its profile leaving both
    /// members absent.
    struct UpstreamStoreRow
    {
        UpstreamStore outcome {};                                            ///< The outcome this row describes.
        std::optional<IMetricsSink::Counter> CacheTierProfile::* counter {}; ///< Null where nothing is counted.
    };

    /// Which counter each outcome moves -- a table, so a fourth outcome is a row.
    ///
    /// `NotConfigured` naming **no** counter is the fix for #214 and a legitimate
    /// row rather than a gap, the same way `RefusalTable` has rows that move
    /// nothing: a node with no shared cache did not attempt a store, so it neither
    /// succeeded nor failed at one. Counting it as a failure made every
    /// single-machine install report a saturated failure rate, which is the
    /// reliable way to make an operator stop reading that counter at all.
    constexpr auto UpstreamStoreCounters = EnumTable<UpstreamStore, UpstreamStoreRow> {
        UpstreamStoreRow { .outcome = UpstreamStore::Stored, .counter = &CacheTierProfile::upstreamStores },
        UpstreamStoreRow { .outcome = UpstreamStore::Declined, .counter = &CacheTierProfile::upstreamStoreFailures },
        UpstreamStoreRow { .outcome = UpstreamStore::NotConfigured, .counter = nullptr },
    };
    static_assert(RowsInEnumeratorOrder(UpstreamStoreCounters, &UpstreamStoreRow::outcome));

    /// Move @p counter when the tier's profile names one.
    ///
    /// An absent member is a tier on which the event cannot happen -- the shared tier has no
    /// upstream to answer a fetch or take a store -- so skipping it is the profile's statement,
    /// not a counter this call forgot.
    /// @param metrics Where the tier counts.
    /// @param counter The profile's member for this event.
    void CountIfProfiled(IMetricsSink& metrics, std::optional<IMetricsSink::Counter> const& counter)
    {
        if (counter.has_value())
            metrics.Increment(*counter);
    }

    /// @p profile, once it states every column; a programmer error otherwise.
    ///
    /// Asked in the member initializer, so no constructor can keep a profile without it -- and a
    /// profile nobody asserted is refused where the tier is composed, rather than reaching
    /// `Increment(Counter::Last)` the first time the forgotten event happens.
    /// @param profile The tier's profile.
    /// @return A copy of @p profile, which is what the tier keeps anyway -- by value rather than a
    ///         reference to the parameter, which a temporary argument would leave dangling.
    [[nodiscard]] CacheTierProfile Stated(CacheTierProfile const& profile)
    {
        if (!StatesEveryMember(profile))
            throw std::invalid_argument { "a cache tier profile must state every column that has no absent "
                                          "reading: its verb pair, its drop verb and every counter" };
        return profile;
    }
} // namespace

LocalCache::LocalCache(IStorage& local,
                       ICacheUpstream& upstream,
                       core::platform::IClock& clock,
                       IMetricsSink& metrics,
                       CacheTierProfile const& profile):
    _local { local },
    _upstream { upstream },
    _clock { clock },
    _metrics { metrics },
    _counters { Stated(profile) }
{
    // A programmer error, so an exception: the pairing is fixed where the tier is composed, and a
    // tier reading through while counting none of it is the silence a profile exists to prevent.
    if (UpstreamCountingOf(profile) != UpstreamCounting::Every)
        throw std::invalid_argument {
            "a cache tier that reads through to an upstream must count every upstream outcome; one whose profile "
            "counts none reads through to nothing (NoUpstream)"
        };
}

LocalCache::LocalCache(IStorage& local,
                       NoUpstream& upstream,
                       core::platform::IClock& clock,
                       IMetricsSink& metrics,
                       CacheTierProfile const& profile):
    _local { local },
    _upstream { upstream },
    _clock { clock },
    _metrics { metrics },
    _counters { Stated(profile) }
{
    if (UpstreamCountingOf(profile) == UpstreamCounting::Partial)
        throw std::invalid_argument { "a cache tier profile counts every upstream outcome or none" };
}

core::async::Task<std::optional<std::vector<std::byte>>> LocalCache::Fetch(std::string_view key)
{
    if (auto const hit = _local.Get(key, _clock.now()); hit.has_value() && hit->found)
    {
        // No upstream call at all. That is the whole point of this tier: an object
        // key is a digest over the preprocessed text, the arguments, the compiler
        // identity and the dependency set, so a key that matches names the same
        // object by construction and there is nothing the shared cache could tell us
        // that we do not already know. Revalidating would have moved the round trip
        // rather than removed it.
        _metrics.Increment(_counters.hits);
        auto const bytes = hit->entry.ValueBytes();
        co_return std::vector<std::byte> { bytes.begin(), bytes.end() };
    }

    _metrics.Increment(_counters.misses);

    auto fetched = co_await _upstream.Fetch(key);
    if (!fetched.has_value())
        // A miss and an unreachable upstream are one answer here, deliberately: the
        // caller compiles either way. Which of the two it was is an operator's
        // question, and the upstream implementation counts it.
        co_return std::nullopt;

    CountIfProfiled(_metrics, _counters.upstreamHits);

    // Populate, so the NEXT build of this object is local. Without this the tier is
    // a proxy rather than a cache and the second build is as slow as the first.
    //
    // A failure to populate is not a failure to serve: the value is in hand and the
    // caller is owed it. Losing the local copy costs one future round trip, which is
    // strictly better than failing a build that could have succeeded.
    if (auto const stored = _local.Set(key, *fetched, NoFlags, NoExpiry); !stored.has_value())
        CountIfProfiled(_metrics, _counters.fillFailures);

    co_return fetched;
}

core::async::Task<bool> LocalCache::Store(std::string_view key, std::span<std::byte const> value)
{
    // Local FIRST, and it is the write that must not be lost: it is what makes this
    // machine's next build fast, and it must not fail for a reason the network chose.
    auto const stored = _local.Set(key, std::vector<std::byte> { value.begin(), value.end() }, NoFlags, NoExpiry);
    if (!stored.has_value())
    {
        _metrics.Increment(_counters.storeFailures);
        co_return false;
    }

    // Then offer it to the fleet, best-effort by contract. A shared cache that cannot
    // be reached costs the fleet one entry and costs this machine nothing -- so the
    // answer is counted rather than returned, because a client that retried on it
    // would be retrying something already durable where it matters.
    //
    // Counted through the table, which is what keeps "there is no upstream" from
    // being counted as "the upstream refused": the row for that outcome names no
    // counter at all.
    //
    // And through the profile: a row names which of the tier's members it moves, and a tier with
    // no upstream leaves that member absent.
    auto const& row = UpstreamStoreCounters.at(static_cast<std::size_t>(co_await _upstream.Store(key, value)));
    if (row.counter != nullptr)
        CountIfProfiled(_metrics, _counters.*row.counter);

    co_return true;
}

CacheDropOutcome LocalCache::Drop(std::string_view key)
{
    auto const removed = _local.Delete(key, _clock.now());
    if (removed.has_value())
        return CacheDropOutcome::Removed;
    return removed.error().code == StorageErrorCode::KeyNotFound ? CacheDropOutcome::Absent : CacheDropOutcome::Failed;
}

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#include "LocalCache.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <optional>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// Compile objects carry no TTL: a key is a digest over everything that
    /// determines the object, so an entry is valid until it is evicted for space.
    /// Expiring one would only force a recompile of something still correct.
    ///
    /// `TimePoint::max()` and NOT a default-constructed `TimePoint`. The storage
    /// tests `entry.expiry <= now`, so the zero value means "expired before any
    /// clock reading" rather than "no deadline" -- every write would land and be
    /// unreadable, which is a cache that silently stores nothing. `CacheEngine`
    /// spells never-expires the same way.
    constexpr TimePoint NoExpiry = TimePoint::max();

    /// Compile values carry no memcached flags word; the framing is the wire's.
    constexpr std::uint32_t NoFlags = 0;

    /// One outcome of offering an object upstream, and the counter it moves.
    struct UpstreamStoreRow
    {
        UpstreamStore outcome {};                        ///< The outcome this row describes.
        std::optional<IMetricsSink::Counter> counter {}; ///< What it increments, if anything.
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
        UpstreamStoreRow { .outcome = UpstreamStore::Stored, .counter = IMetricsSink::Counter::NodeCacheUpstreamStores },
        UpstreamStoreRow { .outcome = UpstreamStore::Declined,
                           .counter = IMetricsSink::Counter::NodeCacheUpstreamStoreFailures },
        UpstreamStoreRow { .outcome = UpstreamStore::NotConfigured, .counter = std::nullopt },
    };
    static_assert(RowsInEnumeratorOrder(UpstreamStoreCounters, &UpstreamStoreRow::outcome));
} // namespace

LocalCache::LocalCache(IStorage& local, ICacheUpstream& upstream, IClock& clock, IMetricsSink& metrics) noexcept:
    _local { local },
    _upstream { upstream },
    _clock { clock },
    _metrics { metrics }
{
}

Task<std::optional<std::vector<std::byte>>> LocalCache::Fetch(std::string_view key)
{
    if (auto const hit = _local.Get(key, _clock.Now()); hit.has_value() && hit->found)
    {
        // No upstream call at all. That is the whole point of this tier: an object
        // key is a digest over the preprocessed text, the arguments, the compiler
        // identity and the dependency set, so a key that matches names the same
        // object by construction and there is nothing the shared cache could tell us
        // that we do not already know. Revalidating would have moved the round trip
        // rather than removed it.
        _metrics.Increment(IMetricsSink::Counter::NodeCacheHits);
        auto const bytes = hit->entry.ValueBytes();
        co_return std::vector<std::byte> { bytes.begin(), bytes.end() };
    }

    _metrics.Increment(IMetricsSink::Counter::NodeCacheMisses);

    auto fetched = co_await _upstream.Fetch(key);
    if (!fetched.has_value())
        // A miss and an unreachable upstream are one answer here, deliberately: the
        // caller compiles either way. Which of the two it was is an operator's
        // question, and the upstream implementation counts it.
        co_return std::nullopt;

    _metrics.Increment(IMetricsSink::Counter::NodeCacheUpstreamHits);

    // Populate, so the NEXT build of this object is local. Without this the tier is
    // a proxy rather than a cache and the second build is as slow as the first.
    //
    // A failure to populate is not a failure to serve: the value is in hand and the
    // caller is owed it. Losing the local copy costs one future round trip, which is
    // strictly better than failing a build that could have succeeded.
    if (auto const stored = _local.Set(key, *fetched, NoFlags, NoExpiry); !stored.has_value())
        _metrics.Increment(IMetricsSink::Counter::NodeCacheFillFailures);

    co_return fetched;
}

Task<bool> LocalCache::Store(std::string_view key, std::span<std::byte const> value)
{
    // Local FIRST, and it is the write that must not be lost: it is what makes this
    // machine's next build fast, and it must not fail for a reason the network chose.
    auto const stored = _local.Set(key, std::vector<std::byte> { value.begin(), value.end() }, NoFlags, NoExpiry);
    if (!stored.has_value())
    {
        _metrics.Increment(IMetricsSink::Counter::NodeCacheStoreFailures);
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
    auto const& row = UpstreamStoreCounters.at(static_cast<std::size_t>(co_await _upstream.Store(key, value)));
    if (row.counter.has_value())
        _metrics.Increment(*row.counter);

    co_return true;
}

} // namespace FastCache::Node

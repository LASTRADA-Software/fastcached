// SPDX-License-Identifier: Apache-2.0
#include "LocalCache.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
/// Bytes from text, for readable fixtures.
[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (auto const ch: text)
        out.push_back(static_cast<std::byte>(ch));
    return out;
}

/// A shared cache under the test's control.
///
/// Counts calls, because the property that matters most here is one of *absence*:
/// a local hit must not touch the upstream at all, and only a call count can say so.
class ScriptedUpstream final: public ICacheUpstream
{
  public:
    std::map<std::string, std::vector<std::byte>> entries;
    std::size_t fetches { 0 };
    std::size_t stores { 0 };
    bool reachable { true };

    [[nodiscard]] Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override
    {
        ++fetches;
        if (!reachable)
            co_return std::nullopt;
        auto const it = entries.find(std::string { key });
        co_return it == entries.end() ? std::nullopt : std::optional { it->second };
    }

    [[nodiscard]] Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) override
    {
        ++stores;
        if (!reachable)
            // `Declined`, never `NotConfigured`: this double stands for an upstream
            // that EXISTS and is unreachable, which is the case the failure counter
            // is for.
            co_return UpstreamStore::Declined;
        entries[std::string { key }] = std::vector<std::byte> { value.begin(), value.end() };
        co_return UpstreamStore::Stored;
    }

    [[nodiscard]] bool Configured() const noexcept override
    {
        return true;
    }
};

/// A node with a small local tier over a scripted shared cache.
struct Fixture
{
    // Field order is the analyzer's, not the reading order: `local` and `cache` are
    // large and alignment-sensitive, and putting the small members first left 64
    // bytes of padding in a struct every test instantiates.
    InMemoryLruStorage local { 64 * 1024 };
    ManualClock clock;
    ScriptedUpstream upstream;
    AtomicMetricsSink metrics;
    LocalCache cache { local, upstream, clock, metrics };

    [[nodiscard]] std::uint64_t Count(IMetricsSink::Counter counter) const
    {
        return metrics.Read(counter);
    }
};
} // namespace

TEST_CASE("A local hit never touches the network", "[node][cache]")
{
    // The entire reason this tier exists. An implementation that revalidated against
    // the shared cache would have MOVED the round trip rather than removed it, and a
    // developer rebuilding the same tree on a slow link would be no better off.
    //
    // It is safe because an object key is a digest over the preprocessed text, the
    // arguments, the compiler identity and the dependency set -- a key that matches
    // names the same object by construction, so there is nothing the shared cache
    // could tell us that we do not already know.
    Fixture fix;
    REQUIRE(SyncRun(fix.cache.Store("k1", Bytes("object-one"))));
    auto const storesAfterWrite = fix.upstream.stores;

    auto const hit = SyncRun(fix.cache.Fetch("k1"));
    REQUIRE(hit.has_value());
    CHECK(Unwrap(hit) == Bytes("object-one"));

    // Not "few" calls -- zero. That is the property.
    CHECK(fix.upstream.fetches == 0);
    CHECK(fix.upstream.stores == storesAfterWrite);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheHits) == 1);
}

TEST_CASE("A local miss reads through and fills the local tier", "[node][cache]")
{
    // Without the fill this is a proxy rather than a cache: the second build would be
    // exactly as slow as the first.
    Fixture fix;
    fix.upstream.entries["k2"] = Bytes("object-two");

    auto const first = SyncRun(fix.cache.Fetch("k2"));
    REQUIRE(first.has_value());
    CHECK(Unwrap(first) == Bytes("object-two"));
    CHECK(fix.upstream.fetches == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamHits) == 1);

    // The second lookup is local, which is the whole point of having filled it.
    auto const second = SyncRun(fix.cache.Fetch("k2"));
    REQUIRE(second.has_value());
    CHECK(Unwrap(second) == Bytes("object-two"));
    CHECK(fix.upstream.fetches == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheHits) == 1);
}

TEST_CASE("An unreachable shared cache is a miss, not a failure", "[node][cache]")
{
    // Every caller's answer to both is "compile it", so distinguishing them here
    // would buy nothing and would give a build a failure mode it does not need. The
    // distinction is kept where it is actionable -- a counter an operator reads.
    Fixture fix;
    fix.upstream.entries["k3"] = Bytes("object-three");
    fix.upstream.reachable = false;

    CHECK_FALSE(SyncRun(fix.cache.Fetch("k3")).has_value());
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheMisses) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamHits) == 0);
}

TEST_CASE("A store writes locally first, then offers upstream", "[node][cache]")
{
    // Order is the substance. The local write is what makes THIS machine's next build
    // fast and must not fail for a reason the network chose; the upstream offer is
    // best-effort by contract.
    Fixture fix;

    REQUIRE(SyncRun(fix.cache.Store("k4", Bytes("object-four"))));
    CHECK(fix.upstream.stores == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStores) == 1);

    // Readable locally with no network call.
    auto const hit = SyncRun(fix.cache.Fetch("k4"));
    REQUIRE(hit.has_value());
    CHECK(fix.upstream.fetches == 0);
}

TEST_CASE("A store survives a shared cache that will not take it", "[node][cache]")
{
    // A fleet that cannot be reached costs the fleet one shared entry and costs this
    // machine nothing. Reporting failure here would fail a build whose object is
    // already durable exactly where it needs to be.
    Fixture fix;
    fix.upstream.reachable = false;

    CHECK(SyncRun(fix.cache.Store("k5", Bytes("object-five"))));
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheStoreFailures) == 0);

    // And it is still served locally, which is what "costs this machine nothing" means.
    auto const hit = SyncRun(fix.cache.Fetch("k5"));
    REQUIRE(hit.has_value());
    CHECK(Unwrap(hit) == Bytes("object-five"));
}

TEST_CASE("A node with no shared cache still caches locally", "[node][cache]")
{
    // The honest shape for one developer's machine, or a fleet that has not been
    // given a shared cache yet. `NoUpstream` is a named type rather than a null
    // pointer so every call site is spared a branch and "there is no upstream" is a
    // decision somebody made.
    ManualClock clock;
    AtomicMetricsSink metrics;
    InMemoryLruStorage local { 64 * 1024 };
    NoUpstream none;
    LocalCache cache { local, none, clock, metrics };

    CHECK(SyncRun(cache.Store("k6", Bytes("object-six"))));
    auto const hit = SyncRun(cache.Fetch("k6"));
    REQUIRE(hit.has_value());
    CHECK(Unwrap(hit) == Bytes("object-six"));

    // A key nobody stored is simply a miss -- not an error, and not a hang.
    CHECK_FALSE(SyncRun(cache.Fetch("never-stored")).has_value());
}

TEST_CASE("A node with no shared cache reports no upstream stores and no failures", "[node][cache]")
{
    // #214. `NoUpstream::Store` answered `false` by contract and `LocalCache` read
    // that as "the shared cache declined it", so EVERY local store incremented the
    // failure counter. The reported install read 1800 -- exactly its
    // `cmd_set_total` -- which is a 100 % upstream store failure rate on a machine
    // that has no upstream to fail.
    //
    // An operator alerting on that counter alerts permanently on every
    // single-machine install, which is how a counter stops being read at all.
    ManualClock clock;
    AtomicMetricsSink metrics;
    InMemoryLruStorage local { 64 * 1024 };
    NoUpstream none;
    LocalCache cache { local, none, clock, metrics };

    for (auto const index: std::views::iota(0, 5))
        CHECK(SyncRun(cache.Store(std::format("k{}", index), Bytes("object"))));

    // Neither counter moves. Not "failures is zero" alone: the way to get this
    // wrong in the other direction is to count the non-event as a success.
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 0);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheUpstreamStores) == 0);

    // And the local writes -- the ones that must not be lost -- all happened.
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheStoreFailures) == 0);
    CHECK(none.Configured() == false);
}

TEST_CASE("A shared cache that declines is still counted as a failure", "[node][cache]")
{
    // The half that must NOT change. Silencing the non-event by not counting at all
    // would take the real failure with it, and an unreachable shared cache would
    // then look exactly like a healthy one -- the same defect #214 describes,
    // pointing the other way.
    Fixture fix;
    fix.upstream.reachable = false;

    CHECK(SyncRun(fix.cache.Store("k7", Bytes("object-seven"))));
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStores) == 0);
    CHECK(fix.upstream.Configured());
}

TEST_CASE("A shared cache that takes the object is counted as a store", "[node][cache]")
{
    Fixture fix;

    CHECK(SyncRun(fix.cache.Store("k8", Bytes("object-eight"))));
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStores) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 0);
}

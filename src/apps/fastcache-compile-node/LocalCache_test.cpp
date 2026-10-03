// SPDX-License-Identifier: Apache-2.0
#include "LocalCache.hpp"
#include "PrivateTierProfile.hpp"
#include "SharedTierProfile.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/platform/Clock.hpp>
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

    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override
    {
        ++fetches;
        if (!reachable)
            co_return std::nullopt;
        auto const it = entries.find(std::string { key });
        co_return it == entries.end() ? std::nullopt : std::optional { it->second };
    }

    [[nodiscard]] core::async::Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) override
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
    core::platform::ManualClock clock;
    ScriptedUpstream upstream;
    AtomicMetricsSink metrics;
    LocalCache cache { local, upstream, clock, metrics, PrivateTierProfile };

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
    REQUIRE(core::async::syncRun(fix.cache.Store("k1", Bytes("object-one"))));
    auto const storesAfterWrite = fix.upstream.stores;

    auto const hit = core::async::syncRun(fix.cache.Fetch("k1"));
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

    auto const first = core::async::syncRun(fix.cache.Fetch("k2"));
    REQUIRE(first.has_value());
    CHECK(Unwrap(first) == Bytes("object-two"));
    CHECK(fix.upstream.fetches == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamHits) == 1);

    // The second lookup is local, which is the whole point of having filled it.
    auto const second = core::async::syncRun(fix.cache.Fetch("k2"));
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

    CHECK_FALSE(core::async::syncRun(fix.cache.Fetch("k3")).has_value());
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheMisses) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamHits) == 0);
}

TEST_CASE("A store writes locally first, then offers upstream", "[node][cache]")
{
    // Order is the substance. The local write is what makes THIS machine's next build
    // fast and must not fail for a reason the network chose; the upstream offer is
    // best-effort by contract.
    Fixture fix;

    REQUIRE(core::async::syncRun(fix.cache.Store("k4", Bytes("object-four"))));
    CHECK(fix.upstream.stores == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStores) == 1);

    // Readable locally with no network call.
    auto const hit = core::async::syncRun(fix.cache.Fetch("k4"));
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

    CHECK(core::async::syncRun(fix.cache.Store("k5", Bytes("object-five"))));
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheStoreFailures) == 0);

    // And it is still served locally, which is what "costs this machine nothing" means.
    auto const hit = core::async::syncRun(fix.cache.Fetch("k5"));
    REQUIRE(hit.has_value());
    CHECK(Unwrap(hit) == Bytes("object-five"));
}

TEST_CASE("A node with no shared cache still caches locally", "[node][cache]")
{
    // The honest shape for one developer's machine, or a fleet that has not been
    // given a shared cache yet. `NoUpstream` is a named type rather than a null
    // pointer so every call site is spared a branch and "there is no upstream" is a
    // decision somebody made.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    InMemoryLruStorage local { 64 * 1024 };
    NoUpstream none;
    LocalCache cache { local, none, clock, metrics, PrivateTierProfile };

    CHECK(core::async::syncRun(cache.Store("k6", Bytes("object-six"))));
    auto const hit = core::async::syncRun(cache.Fetch("k6"));
    REQUIRE(hit.has_value());
    CHECK(Unwrap(hit) == Bytes("object-six"));

    // A key nobody stored is simply a miss -- not an error, and not a hang.
    CHECK_FALSE(core::async::syncRun(cache.Fetch("never-stored")).has_value());
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
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    InMemoryLruStorage local { 64 * 1024 };
    NoUpstream none;
    LocalCache cache { local, none, clock, metrics, PrivateTierProfile };

    for (auto const index: std::views::iota(0, 5))
        CHECK(core::async::syncRun(cache.Store(std::format("k{}", index), Bytes("object"))));

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

    CHECK(core::async::syncRun(fix.cache.Store("k7", Bytes("object-seven"))));
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStores) == 0);
    CHECK(fix.upstream.Configured());
}

TEST_CASE("A shared cache that takes the object is counted as a store", "[node][cache]")
{
    Fixture fix;

    CHECK(core::async::syncRun(fix.cache.Store("k8", Bytes("object-eight"))));
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStores) == 1);
    CHECK(fix.Count(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 0);
}

// --- cache-drop (#1276) --------------------------------------------------------

TEST_CASE("A drop removes the key from this tier, and a second drop finds nothing", "[node][cache][cache-drop]")
{
    // Three outcomes, and the second run of a repair is the one a `bool` would get wrong:
    // nothing to remove is an answer, not a failure.
    Fixture fix;
    fix.upstream.reachable = false;
    REQUIRE(core::async::syncRun(fix.cache.Store("k9", Bytes("object-nine"))));

    CHECK(fix.cache.Drop("k9") == CacheDropOutcome::Removed);
    CHECK_FALSE(core::async::syncRun(fix.cache.Fetch("k9")).has_value());
    CHECK(fix.cache.Drop("k9") == CacheDropOutcome::Absent);

    // Counted by the storage, not by a second tally beside it.
    CHECK(fix.local.Snapshot().deleteHits == 1);
    CHECK(fix.local.Snapshot().deleteMisses == 1);
}

TEST_CASE("A drop reaches this tier only, so a shared cache holding the key refills it", "[node][cache][cache-drop]")
{
    // #1276's D3, pinned in the direction that surprises: the drop is Removed, the shared
    // cache is left holding the object, and the next miss here reads it back through. That
    // is the rule rather than a leak -- a destructive verb reaches the endpoint its sender
    // named -- and it is why the operator is told to drop the key upstream as well.
    Fixture fix;
    fix.upstream.entries["k10"] = Bytes("object-ten");
    REQUIRE(core::async::syncRun(fix.cache.Fetch("k10")).has_value()); // fills the local tier
    REQUIRE(fix.upstream.fetches == 1);

    CHECK(fix.cache.Drop("k10") == CacheDropOutcome::Removed);
    CHECK(fix.upstream.entries.contains("k10"));

    auto const refilled = core::async::syncRun(fix.cache.Fetch("k10"));
    REQUIRE(refilled.has_value());
    CHECK(Unwrap(refilled) == Bytes("object-ten"));
    CHECK(fix.upstream.fetches == 2);
}

TEST_CASE("A tier counts on its own descriptor and moves nobody else's counters", "[node][cache][shared-cache]")
{
    // The fleet's shared tier reuses this class, so a stored value is canonicalized on the one
    // path every server of this wire shares -- and it must still move only its own series. An
    // operator reading the private tier's hit rate is reading this machine's builds; a shared
    // tier counting there would fold every other machine's builds into it.
    InMemoryLruStorage local { 1024 * 1024 };
    NoUpstream upstream;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    LocalCache shared { local, upstream, clock, metrics, SharedTierProfile };

    std::ignore = core::async::syncRun(shared.Fetch("absent"));
    auto const value = std::vector<std::byte> { std::byte { 1 } };
    REQUIRE(core::async::syncRun(shared.Store("k", value)));
    REQUIRE(core::async::syncRun(shared.Fetch("k")).has_value());

    CHECK(metrics.Read(IMetricsSink::Counter::NodeSharedCacheMisses) == 1);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
    // The private tier's series are untouched: an operator reading them sees this machine's builds only.
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheMisses) == 0);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheHits) == 0);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheUpstreamStoreFailures) == 0);
}

TEST_CASE("A tier that counts no upstream outcome reads through to nothing", "[node][cache][shared-cache]")
{
    // The shared tier's profile names no upstream counter, because the shared tier IS the top of
    // the fleet's cache. Paired with an upstream that reads and stores, every read-through and every
    // offered store would happen with nothing an operator reads moving -- so the pairing is refused
    // where the tier is composed, not discovered in a graph that stays flat.
    InMemoryLruStorage local { 1024 * 1024 };
    ScriptedUpstream upstream;
    NoUpstream nothing;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;

    CHECK(UpstreamCountingOf(SharedTierProfile) == UpstreamCounting::None);
    CHECK(UpstreamCountingOf(PrivateTierProfile) == UpstreamCounting::Every);

    CHECK_THROWS_AS((LocalCache { local, upstream, clock, metrics, SharedTierProfile }), std::invalid_argument);
    CHECK_NOTHROW((LocalCache { local, nothing, clock, metrics, SharedTierProfile }));
    // An upstream reached through the interface is one that may read through, whatever it is.
    CHECK_THROWS_AS((LocalCache { local, static_cast<ICacheUpstream&>(nothing), clock, metrics, SharedTierProfile }),
                    std::invalid_argument);
    // The private tier counts every outcome, so either pairing is sound.
    CHECK_NOTHROW((LocalCache { local, upstream, clock, metrics, PrivateTierProfile }));
    CHECK_NOTHROW((LocalCache { local, nothing, clock, metrics, PrivateTierProfile }));

    // A profile counting some outcomes of an upstream and not others is refused either way.
    auto partial = PrivateTierProfile;
    partial.fillFailures = std::nullopt;
    CHECK(UpstreamCountingOf(partial) == UpstreamCounting::Partial);
    CHECK_THROWS_AS((LocalCache { local, upstream, clock, metrics, partial }), std::invalid_argument);
    CHECK_THROWS_AS((LocalCache { local, nothing, clock, metrics, partial }), std::invalid_argument);
}

TEST_CASE("A cache tier profile that leaves a column unstated is refused at compile time", "[node][cache][shared-cache]")
{
    // The columns default to their UNSTATED values -- an unstated verb pair, `DropVerb::Unstated`,
    // `Counter::Last` -- so a row that forgot one compiles and the missing-field warning cannot say
    // so. `StatesEveryMember` is what does, and each profile header asserts it. Asserted here in
    // the refusing direction too, one column at a time, so the guard is watched refusing.
    STATIC_REQUIRE(StatesEveryMember(PrivateTierProfile));
    STATIC_REQUIRE(StatesEveryMember(SharedTierProfile));

    constexpr auto forgotVerbs = [] {
        auto row = PrivateTierProfile;
        row.verbs = TierVerbs {};
        return row;
    }();
    constexpr auto forgotDrop = [] {
        auto row = PrivateTierProfile;
        row.drop = DropVerb::Unstated;
        return row;
    }();
    constexpr auto forgotHits = [] {
        auto row = SharedTierProfile;
        row.hits = IMetricsSink::Counter::Last;
        return row;
    }();
    constexpr auto forgotRefusal = [] {
        auto row = SharedTierProfile;
        row.refusedForeignGeneration = IMetricsSink::Counter::Last;
        return row;
    }();
    STATIC_REQUIRE_FALSE(StatesEveryMember(forgotVerbs));
    STATIC_REQUIRE_FALSE(StatesEveryMember(forgotDrop));
    STATIC_REQUIRE_FALSE(StatesEveryMember(forgotHits));
    STATIC_REQUIRE_FALSE(StatesEveryMember(forgotRefusal));
}

TEST_CASE("A tier built from a profile that leaves a column unstated is refused, whoever forgot the assert",
          "[node][cache][shared-cache]")
{
    // The `static_assert` beside a profile is a guard called ALONGSIDE; a third profile whose author
    // forgot it would reach `Increment(Counter::Last)` at run time. So the construction itself asks,
    // with either upstream, and the stated profiles are the control.
    InMemoryLruStorage local { 1024 * 1024 };
    ScriptedUpstream upstream;
    NoUpstream nothing;
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;

    auto forgotStores = PrivateTierProfile;
    forgotStores.storeFailures = IMetricsSink::Counter::Last;
    auto forgotDrop = SharedTierProfile;
    forgotDrop.drop = DropVerb::Unstated;

    CHECK_THROWS_AS((LocalCache { local, upstream, clock, metrics, forgotStores }), std::invalid_argument);
    CHECK_THROWS_AS((LocalCache { local, nothing, clock, metrics, forgotStores }), std::invalid_argument);
    CHECK_THROWS_AS((LocalCache { local, nothing, clock, metrics, forgotDrop }), std::invalid_argument);
    CHECK_NOTHROW((LocalCache { local, upstream, clock, metrics, PrivateTierProfile }));
    CHECK_NOTHROW((LocalCache { local, nothing, clock, metrics, SharedTierProfile }));
}

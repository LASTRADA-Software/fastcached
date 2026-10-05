// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "InProcessSharedUpstream.hpp"
#include "LocalCache.hpp"
#include "PrivateTierProfile.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <core/async/SyncRun.hpp>
#include <tests/SharedTierFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::AStoredObject;
using FastCache::Testing::MemoryOpener;
using FastCache::Testing::NamingSharedCache;
using FastCache::Testing::StatusOf;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// @param counter A counter.
/// @return Its exported name, through the catalogue's one lookup; a counter with no row is a name
///         nobody could expect, so it never compares equal to an expected row.
[[nodiscard]] std::string_view NameOf(IMetricsSink::Counter counter)
{
    auto const* const row = DescriptorOf(counter);
    return row != nullptr ? row->prometheusName : std::string_view { "<no catalogue row>" };
}

/// Which counters moved, and by how much, keyed by their exported names -- EVERY counter, so a row
/// that moved and was not expected is in the answer as surely as one that was.
using Movement = std::map<std::string_view, std::uint64_t>;

/// Every counter's reading.
/// @param metrics The sink.
/// @return One reading per counter, by name.
[[nodiscard]] Movement Readings(IMetricsSink const& metrics)
{
    Movement readings;
    for (auto const counter: Enumerators<IMetricsSink::Counter>())
        readings.emplace(NameOf(counter), metrics.Read(counter));
    return readings;
}

/// What moved from @p before to @p after.
/// @param before The earlier readings.
/// @param after The later ones.
/// @return Every counter whose reading changed, with how far.
[[nodiscard]] Movement Moved(Movement const& before, Movement const& after)
{
    Movement moved;
    for (auto const& [name, reading]: after)
        if (auto const was = before.at(name); reading != was)
            moved.emplace(name, reading - was);
    return moved;
}

/// @param rows Each counter expected to move, and by how much.
/// @return The same, by exported name.
[[nodiscard]] Movement Expected(std::initializer_list<std::pair<IMetricsSink::Counter, std::uint64_t>> rows)
{
    Movement expected;
    for (auto const& [counter, by]: rows)
        expected.emplace(NameOf(counter), by);
    return expected;
}

/// @param movement What moved.
/// @return One `name +delta` per row, in name order: what a failed comparison prints.
[[nodiscard]] std::string Describe(Movement const& movement)
{
    std::string text;
    for (auto const& [name, by]: movement)
        text += std::format("{} +{}\n", name, by);
    return text;
}

} // namespace

TEST_CASE("The named machine's private tier reads its own shared tier without a network", "[node][shared-cache][in-process]")
{
    // Nothing here can dial: the upstream is built from the host alone, with no connector, dialer or
    // session to reach one through.
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();

    InProcessSharedUpstream upstream { host };
    CHECK(upstream.Configured());
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());

    auto const value = AStoredObject();
    CHECK(core::async::syncRun(upstream.Store("k", value)) == UpstreamStore::Stored);
    auto const fetched = core::async::syncRun(upstream.Fetch("k"));
    REQUIRE(fetched.has_value());
    CHECK(Unwrap(fetched) == value);
    // Through the shared tier's own cache: the fleet's series moved, as they would for any other
    // fleet machine's read of the same object.
    CHECK(opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheMisses) == 1);
    CHECK(opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
}

TEST_CASE("A private tier on the machine that stopped being named reads through to nothing",
          "[node][shared-cache][in-process]")
{
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    InProcessSharedUpstream upstream { host };
    // Before any state is applied, nothing is served here.
    CHECK_FALSE(upstream.Configured());

    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();
    REQUIRE(upstream.Configured());
    REQUIRE(core::async::syncRun(upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);

    // The setting moves away: the next operation finds no tier, and is "not configured" -- the
    // answer that is never counted as a failure -- rather than a store declined by a tier.
    host.Applied(NamingSharedCache("cache-d"));
    host.Reconcile();
    CHECK_FALSE(upstream.Configured());
    CHECK(core::async::syncRun(upstream.Store("k", AStoredObject())) == UpstreamStore::NotConfigured);
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    // And it was asked of no tier: the retired one saw the one store and nothing after it.
    CHECK(opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheMisses) == 0);
    CHECK(opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 0);
}

TEST_CASE("A tier the setting moves away from in the middle of a call is retired under it, never freed",
          "[node][shared-cache][in-process]")
{
    // The upstream holds the tier for the whole call. So when the setting moves and the host reconciles
    // WHILE a call is inside the tier -- here, from the tier's own storage call, after the upstream took
    // the tier -- the tier is retired and kept alive by the call, and only the next reconcile frees it.
    // Holding a raw pointer or a reference instead frees it under the call.
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();
    InProcessSharedUpstream upstream { host };
    auto const value = AStoredObject();
    REQUIRE(core::async::syncRun(upstream.Store("k", value)) == UpstreamStore::Stored);

    auto const noThread = std::thread::id {};
    auto moved = 0;
    auto freedDuringCall = false;
    auto retiredDuringCall = false;
    auto const moveAway = [&] {
        ++moved;
        host.Applied(NamingSharedCache("cache-d"));
        host.Reconcile();
        // Observed here rather than inferred afterwards: the reconcile inside the call unpublished the
        // tier, and did not free it.
        retiredDuringCall = host.Current() == nullptr;
        freedDuringCall = opener.closedOn != noThread;
    };

    // A FETCH: the move lands inside it, and the value comes back from the tier it took.
    opener.storageHook = moveAway;
    auto const fetched = core::async::syncRun(upstream.Fetch("k"));
    REQUIRE(moved == 1);
    CHECK(retiredDuringCall);
    CHECK_FALSE(freedDuringCall);
    REQUIRE(fetched.has_value());
    CHECK(Unwrap(fetched) == value);
    CHECK_FALSE(upstream.Configured());
    CHECK(opener.closedOn == noThread); // it outlived the call: held in the host's retired list
    host.Reconcile();
    CHECK(opener.closedOn == std::this_thread::get_id()); // and the next reconcile freed it, here

    // A STORE: the same, on a tier named again. It still answers `Stored` -- into the tier that has
    // just stopped being the named one, exactly as a remote SHARED-STORE answered just before the move
    // would. Benign: the object is content-addressed and valid, and served again if this machine is
    // named again; the newly named machine lacks one shared entry, which its next STORE fills.
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();
    REQUIRE(upstream.Configured());
    opener.closedOn = noThread;
    opener.storageHook = moveAway;
    CHECK(core::async::syncRun(upstream.Store("k2", value)) == UpstreamStore::Stored);
    REQUIRE(moved == 2);
    CHECK(retiredDuringCall);
    CHECK_FALSE(freedDuringCall);
    CHECK_FALSE(upstream.Configured());
    CHECK(opener.closedOn == noThread);
    host.Reconcile();
    CHECK(opener.closedOn == std::this_thread::get_id());
}

TEST_CASE("A private tier over the in-process upstream moves each tier's row once, on one sink",
          "[node][shared-cache][in-process]")
{
    // The production shape: the private tier and the shared tier in one process, counting into ONE
    // sink. The two profiles' rows are disjoint, so each operation must move each tier's row exactly
    // once -- and a read that took the shared tier twice, or a tier that counted what the other
    // already did, would move a row twice or a row nobody expected. So the case compares EVERY
    // counter's movement with the table, never only the rows it names.
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();
    InProcessSharedUpstream upstream { host };
    auto& metrics = opener.metrics;
    InMemoryLruStorage privateStore { 64 * 1024 * 1024 };
    LocalCache privateCache { privateStore, upstream, opener.clock, metrics, PrivateTierProfile };
    CacheProxy privateProxy { privateCache, metrics };
    auto const tier = host.Current();
    REQUIRE(tier != nullptr);

    // A daemon STORE carrying its roots: canonicalized by the PRIVATE tier's proxy, and stored in the
    // shared tier as tokens -- with no frame and no second canonicalization on the way.
    CompileValue produced;
    produced.objectBlob = { std::byte { 0x01 } };
    produced.textRegions.push_back(TextRegion { .grammar = PathCanon::Grammar::ShowIncludes,
                                                .bytes = "Note: including file: /home/dev/proj/inc/a.hpp\n" });
    auto const storeFrame = Wire::EncodeStoreAs(Wire::DaemonCacheVerbs,
                                                Wire::StoreRequest { .key = "k",
                                                                     .prefetchGroup = {},
                                                                     .srcRoot = "/home/dev/proj",
                                                                     .buildTree = "/home/dev/proj/build",
                                                                     .value = EncodeCompileValue(produced) });
    auto readings = Readings(metrics);
    CHECK(StatusOf(core::async::syncRun(privateProxy.Answer(storeFrame))) == Wire::Status::Ok);
    CHECK(Describe(Moved(readings, Readings(metrics)))
          == Describe(Expected({ { IMetricsSink::Counter::NodeCacheUpstreamStores, 1 } })));
    auto const inShared = core::async::syncRun(tier->Fetch("k"));
    REQUIRE(inShared.has_value());
    auto const decoded = DecodeCompileValue(Unwrap(inShared));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->textRegions.size() == 1);
    CHECK(decoded->textRegions[0].bytes == "Note: including file: <SRCROOT>/inc/a.hpp\n");

    // A daemon FETCH of a key only the shared tier holds: a private miss, a shared hit, and the
    // private tier's read-through hit -- one each, and nothing else.
    REQUIRE(core::async::syncRun(tier->Store("k2", AStoredObject())));
    readings = Readings(metrics);
    CHECK(StatusOf(core::async::syncRun(privateProxy.Answer(Wire::EncodeFetchAs(Wire::DaemonCacheVerbs, "k2"))))
          == Wire::Status::Ok);
    CHECK(Describe(Moved(readings, Readings(metrics)))
          == Describe(Expected({ { IMetricsSink::Counter::NodeCacheMisses, 1 },
                                 { IMetricsSink::Counter::NodeCacheUpstreamHits, 1 },
                                 { IMetricsSink::Counter::NodeSharedCacheHits, 1 } })));

    // And none of a remote read's SESSION rows, which have nothing to count with no peer: named, so
    // their absence is asserted rather than implied.
    for (auto const counter: { IMetricsSink::Counter::NodeSharedCacheSessionsOpened,
                               IMetricsSink::Counter::NodeSharedCacheProofsFailed,
                               IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey,
                               IMetricsSink::Counter::NodeSharedCacheStaleHints,
                               IMetricsSink::Counter::NodeSharedCacheUnresolved })
    {
        INFO(NameOf(counter));
        CHECK(metrics.Read(counter) == 0);
    }
}

TEST_CASE("An in-process upstream borrows its host for as long as it exists", "[node][shared-cache][in-process]")
{
    // The order `main` needs -- host first, so it goes last -- is checked by the host, not left to
    // the owner: a host destroyed while a borrow is alive ends the process by name. The count is what
    // that check reads.
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    CHECK(host.Borrowers() == 0);
    {
        InProcessSharedUpstream const first { host };
        CHECK(host.Borrowers() == 1);
        {
            InProcessSharedUpstream const second { host };
            CHECK(host.Borrowers() == 2);
        }
        CHECK(host.Borrowers() == 1);
    }
    CHECK(host.Borrowers() == 0);
}

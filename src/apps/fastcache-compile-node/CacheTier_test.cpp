// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{

/// Everything `StartCacheTierOrExplain` needs, over one clock.
///
/// The reactor is declared LAST, so among the members it is destroyed FIRST -- and
/// that is fine here only because no member holds a tier. A case keeps its tier in
/// a local declared AFTER the fixture, which is therefore destroyed BEFORE it, so
/// the tier's destructor posts its listener close onto a reactor that is still
/// there. Reversing the two is not a compile error and would present as a close
/// posted onto a reactor that has gone.
struct Fixture
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    Distributed::ClusterMembership membership { { "127.0.0.1:7000" } };
    NodeIoLoop io;

    /// A config bound to a port found at run time, so cases never collide.
    ///
    /// Not a number somebody picked: `catch_discover_tests` runs every case as its
    /// own process and the suite runs in parallel, so a fixed port is a failure
    /// that appears only under `ctest -j`. And not `0` either -- `ParseTcpPort`
    /// rejects it, correctly, since as a CLI value it names no port an operator
    /// could dial -- so a probe is bound and released to find one.
    [[nodiscard]] static NodeConfig BaseConfig()
    {
        auto probe = BlockingListener::Bind("127.0.0.1", 0);
        REQUIRE(probe);
        // Asked of the SOCKET, not of the pointer: `Bind` returns a listener in an
        // errored state rather than nothing, so a null check passes on a bind that
        // failed and the port below comes back 0.
        REQUIRE(probe->IsBound());
        auto const port = probe->BoundPort();
        probe.reset();

        NodeConfig cfg;
        cfg.cacheListen = std::format("127.0.0.1:{}", port);
        return cfg;
    }

    /// Start a tier over @p cfg.
    /// @param cfg What the node was told to be.
    /// @return The tier, a null tier meaning "carry on without one", or the reason.
    [[nodiscard]] std::expected<std::unique_ptr<CacheTier>, std::string> Start(NodeConfig const& cfg)
    {
        return StartCacheTierOrExplain(io, cfg, membership, clock, metrics, logger);
    }
};

/// Whether any captured line contains @p needle.
/// @param logger Where the tier reported.
/// @param needle Text to look for.
/// @return True when some line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view needle)
{
    auto const records = logger.Snapshot();
    return std::ranges::any_of(records, [needle](CapturingLogger::Record const& r) { return r.message.contains(needle); });
}

/// One tier's entry, or nullopt when the cache has no such tier.
///
/// A template over the table, because the two this file reads -- `SnapshotTiers()`'s
/// statistics and `CacheCapacityOf`'s budgets -- are both an `EnumTable` keyed by
/// tier, and two helpers with one body is two things to keep in step.
/// @param tiers Any per-tier table.
/// @param tier Which tier to read.
/// @return That tier's entry.
template <typename Table>
[[nodiscard]] auto const& At(Table const& tiers, StorageTier tier)
{
    return tiers[static_cast<std::size_t>(tier)];
}

} // namespace

TEST_CASE("A node told to hold nothing serves no cache tier", "[node][cache-tier]")
{
    // `--cache-memory 0` used to build an InMemoryLruStorage with a zero budget,
    // which is how that class spells UNBOUNDED -- so the one flag an operator can
    // switch this off with was the one that removed the limit, and a node told to
    // hold nothing grew until the machine ran out of memory. Silent throughout:
    // the cache works, and works better and better.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    CHECK(*started == nullptr);
}

TEST_CASE("A node with no cache port serves no cache tier", "[node][cache-tier]")
{
    // `--listen-cache=` is the documented way to turn the local cache off, and it
    // turns it off WITHOUT touching `--cache-memory` -- whose default is a share of
    // host RAM, so it is left non-zero here rather than relying on the runner's.
    // That pairing, a budget asked for and no tier built, is what made sizing the
    // machine's reservation from the flag reserve a quarter of RAM for nothing
    // (#167).
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheListen.clear();
    cfg.cacheMemoryBytes = 8ULL * 1024 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier == nullptr);

    // Absent, not zero, and all the way down: this is the record `NodeCapacityOf`
    // derives the reservation from, so a memory tier reported here for a tier that
    // was never built is memory the fleet would never get back.
    auto const budget = CacheCapacityOf(tier.get());
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Memory).has_value());
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Disk).has_value());

    // And it says so. This was the one route to "no cache tier" that logged
    // nothing at all, so an operator had no line anywhere telling them this node
    // was not caching.
    CHECK(Logged(fixture.logger, "serving no local cache tier"));
}

TEST_CASE("A cache budget with no port to serve it is called out", "[node][cache-tier]")
{
    // `--cache-memory` and `--cache-dir` do nothing at all without a port, and a
    // flag silently doing nothing is the shape this codebase keeps a list about.
    // Named only when the operator actually set one -- a default nobody typed is
    // not something to warn them about.
    Testing::ScratchDirectory const scratch { "node-cache-no-port" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheListen.clear();
    cfg.cacheDir = scratch.Path();

    auto const started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    CHECK(*started == nullptr);
    CHECK(Logged(fixture.logger, "--cache-memory/--cache-dir have no effect"));
}

TEST_CASE("A memory-only node reports its budget and no disk tier", "[node][cache-tier]")
{
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
    // Absent rather than a disk tier holding nothing: an operator who did not ask
    // for `--cache-dir` has no disk tier to be empty.
    CHECK_FALSE(At(tiers, StorageTier::Disk).has_value());

    // And the announced record says the same, because that is the one
    // `NodeCapacityOf` subtracts from this machine's RAM (#167). Asserted here
    // rather than in a case of its own: `CacheCapacityOf` is a projection of the
    // snapshot above, and this file's fixture costs a process and a port probe.
    auto const budget = CacheCapacityOf(tier.get());
    REQUIRE(At(budget.tierBytesLimit, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(budget.tierBytesLimit, StorageTier::Memory)) == cfg.cacheMemoryBytes);
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Disk).has_value());
}

// The name may not START with `--`: `catch_discover_tests` passes it to the
// runner as an argument, and Catch2 reads a leading double dash as a flag and
// exits with "Unrecognised token" -- which ctest reports as a failing test that
// passes when run by hand.
TEST_CASE("Naming --cache-dir gives the node an on-disk tier", "[node][cache-tier]")
{
    // The flag was parsed and round-tripped by the config writer and read by
    // nothing: `CacheTier::Start` built a bare in-memory store and never looked at
    // the path. A node configured with a disk cache had none, and said so nowhere.
    Testing::ScratchDirectory const scratch { "node-cache-dir" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 4 * 1024 * 1024;
    cfg.cacheDir = scratch.Path();

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    REQUIRE(At(tiers, StorageTier::Disk).has_value());
    // Distinct budgets, so a report that named one tier's figure under the other's
    // label could not pass: the memory half is capped and the disk half is not.
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).bytesLimit == 0);
}

TEST_CASE("A disk-only node keeps its disk tier", "[node][cache-tier]")
{
    // `--cache-memory 0 --cache-dir <path>` turns off the in-memory half and
    // nothing else. Reading it as "no cache at all" would silently discard a tier
    // the operator named a path for.
    Testing::ScratchDirectory const scratch { "node-cache-disk-only" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir = scratch.Path();

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    CHECK_FALSE(At(tiers, StorageTier::Memory).has_value());
    CHECK(At(tiers, StorageTier::Disk).has_value());
}

TEST_CASE("A cache directory that cannot be opened is fatal when it was named", "[node][cache-tier]")
{
    // The same rule `--listen-cache` follows: an address the operator TYPED is a
    // promise, and a broken promise stops startup rather than being logged and
    // carried on from. `--cache-dir` has no default at all, so naming it is the
    // only way to reach this path.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 1024 * 1024;
    // A path under a regular file, which no platform will create a store beneath.
    Testing::ScratchDirectory const scratch { "node-cache-bad-dir" };
    auto const blocker = scratch / "not-a-directory";
    {
        std::ofstream const file { blocker };
    }
    cfg.cacheDir = blocker / "cache";

    auto const started = fixture.Start(cfg);
    REQUIRE_FALSE(started.has_value());
    // And it names the flag that failed. Both failures this function can report
    // leave through one string, so a directory it could not create used to reach
    // the operator as "--listen-cache ..." and send them to check a port.
    CHECK(started.error().contains("--cache-dir"));
    CHECK_FALSE(started.error().contains("--listen-cache"));
}

TEST_CASE("A cache port already taken is fatal exactly when the operator named it", "[node][cache-tier]")
{
    // The acceptance of #286, both halves. Which one applies is decided by
    // PROVENANCE and not by the value: it used to be `cfg.cacheListen !=
    // NodeConfig{}.cacheListen`, so an operator who read the startup line and typed
    // the port back to pin it produced a value equal to the default and had their
    // promise read as a convenience -- the node started, reported healthy, and
    // served no cache.
    //
    // The port is held for the duration rather than probed and released, which is
    // what makes the bind actually fail; and it is an ephemeral one rather than the
    // default 6674, because a case may not depend on what else is running on the
    // machine. Only the bit differs between the two sections.
    Fixture fixture;
    auto const held = BlockingListener::Bind("127.0.0.1", 0);
    // `IsBound`, not the pointer: `Bind` hands back a listener in an ERRORED state
    // rather than nothing, so a null check proves only that it allocated. An
    // unbound one reports port 0, `--listen-cache=127.0.0.1:0` is refused by
    // `ParseTcpPort` before any bind is attempted, and both sections below then
    // pass on that refusal instead of on a port conflict -- green, and testing
    // nothing.
    REQUIRE(held);
    REQUIRE(held->IsBound());

    NodeConfig cfg;
    cfg.cacheListen = std::format("127.0.0.1:{}", held->BoundPort());
    cfg.cacheMemoryBytes = 1024 * 1024;

    SECTION("named, so a broken promise stops startup")
    {
        cfg.cacheListenExplicit = true;

        auto const started = fixture.Start(cfg);
        REQUIRE_FALSE(started.has_value());
        // Naming the flag, because the two failures this function reports leave
        // through one string and an operator sent to the wrong flag is sent nowhere.
        CHECK(started.error().contains("--listen-cache"));
    }

    SECTION("defaulted, so a convenience nobody asked for is warned past")
    {
        // A node sharing a machine with `fastcached` must not refuse to start over a
        // port it was never told to take -- the launcher reaches the daemon there
        // instead. Never silently, though.
        cfg.cacheListenExplicit = false;

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        CHECK(*started == nullptr);
        CHECK(Logged(fixture.logger, "continuing without a local cache tier"));
    }
}

TEST_CASE("A tier says whether it has a shared cache behind it", "[node][cache-tier]")
{
    // #214's wiring, asserted rather than assumed. The scrape's
    // `upstream_configured` gauge is read off the upstream that was BUILT, so this
    // is the join between the configuration and what a scrape reports -- and a
    // field nothing populates renders a confident `0` for every node, which is the
    // failure mode the gauge exists to prevent.
    Fixture fixture;

    SECTION("none configured")
    {
        auto cfg = Fixture::BaseConfig();
        cfg.upstream.clear();

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);
        CHECK_FALSE(tier->HasUpstream());
    }

    SECTION("one configured")
    {
        // Named, not reachable -- and that is the distinction. `Configured()` says an
        // operator asked for a shared cache; whether it answers is what the store
        // counters report, and conflating the two is how a laptop came to look like
        // a fleet whose cache is down.
        auto cfg = Fixture::BaseConfig();
        cfg.upstream = "127.0.0.1:1";

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);
        CHECK(tier->HasUpstream());
    }
}

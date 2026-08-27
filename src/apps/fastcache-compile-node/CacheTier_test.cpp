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

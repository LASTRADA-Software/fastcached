// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
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
    NullLogger logger;
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

/// One tier's entry, or nullopt when the cache has no such tier.
/// @param tiers What `SnapshotTiers()` returned.
/// @param tier Which tier to read.
/// @return That tier's entry.
[[nodiscard]] std::optional<StorageStats> const& At(TieredStorageStats const& tiers, StorageTier tier)
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

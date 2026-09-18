// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeIoLoop.hpp"
#include "ScratchClaim.hpp"
#include "WorkerTier.hpp"
#include "WorkerTierTestFixture.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Node;

using WorkerTierFixture = WorkerTierTesting::WorkerTierFixture;
using WorkerTierTesting::Worker;

TEST_CASE("A node running no worker builds no worker tier, and a worker builds one", "[node][worker-tier]")
{
    // #206, #1387. The tier is the one place the question *does this node run a worker*
    // is answered, and on a node running none there is nothing: no pool thread, no
    // capacity sized to zero, no validator, no survey and no heartbeat to register with.
    // A `--scheduler` is named on it deliberately -- the cluster and enrollment commands
    // read it -- and it still builds nothing that could register.
    WorkerTierFixture fixture;

    fixture.cfg.slots = 0;
    fixture.cfg.toolchains.clear();
    // A machine that cannot even be ASKED for its scratch base, which a node running no
    // worker must start on: it never asks.
    fixture.machineThrows = true;
    auto const none = fixture.Start();
    REQUIRE(none.has_value());
    CHECK(none.value() == nullptr);
    CHECK(fixture.machineBuilds == 0);
    CHECK(fixture.discoveryCalls == 0);
    // No scratch root was claimed: the base holds nothing.
    CHECK(std::filesystem::is_empty(fixture.scratch.Path()));

    // The control: the same node running a worker is one, sized from the machine.
    fixture.cfg = Worker();
    fixture.machineThrows = false;
    auto const worker = fixture.Start();
    REQUIRE(worker.has_value());
    REQUIRE(worker.value() != nullptr);
    CHECK(fixture.machineBuilds == 1);
    auto const& tier = *worker.value();
    CHECK(tier.Slots() == Distributed::OfferableSlots(fixture.capacity, std::nullopt));
    CHECK(tier.StartupToolchainCount() == 1);
    // Seeded with what the process was started advertising, which is what the first
    // registration and every lease check before any reload read (#1279). Asserted here
    // because `Start` is where the seam is built and handed to the validator: a tier
    // that came up answering something else would have the fleet dialling one address
    // while this worker verified grants against another.
    CHECK(tier.Advertised().Current() == "127.0.0.1:6674");
    CHECK_FALSE(std::filesystem::is_empty(fixture.scratch.Path()));
}

TEST_CASE("A worker with no scheduler to register with is refused rather than started", "[node][worker-tier]")
{
    // The startup table refuses this configuration, and the tier refuses it again by
    // name: a worker that started without a link would never register, which is the
    // invisible-node failure. One fact, so the link and the worker cannot disagree.
    WorkerTierFixture fixture;
    fixture.cfg.schedulers.clear();

    auto const refused = fixture.Start();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("no --scheduler"));
}

TEST_CASE("A worker answers whether its scratch root can be written into a mapping rule", "[node][worker-tier][conditions]")
{
    // #1364. The worker's half of a debug-prefix-map rule it cannot spell was a startup Warn and
    // nothing else; it is now a LATCHED condition row too, answered where the root is claimed. WHAT
    // DISTINGUISHES: three roots give three different answers -- benign, raised, and not evaluated
    // because there is no root -- and a tier answering any one of them for all three fails two.
    namespace Wire = CompileCacheWire;
    WorkerTierFixture fixture;

    SECTION("an ordinary root is checked and benign")
    {
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::ScratchRootUnmappable) == Wire::ConditionState::Clear);
    }

    SECTION("a root with a space in it is raised, naming the rule and the root")
    {
        fixture.scratchBase = fixture.scratch.Path() / "with space";
        std::filesystem::create_directories(fixture.scratchBase);
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::ScratchRootUnmappable) == Wire::ConditionState::Raised);
        auto const rows = fixture.conditions.Snapshot();
        auto const row =
            std::ranges::find(rows, RowFor(NodeCondition::ScratchRootUnmappable).id, &Wire::NodeConditionFields::id);
        REQUIRE(row != rows.end());
        CHECK(row->detail.contains("-fdebug-prefix-map"));
        CHECK(row->detail.contains("with space"));
        CHECK(row->persistence == "latched");
    }

    SECTION("a worker with nothing to compile claims no root, and says it could not check one")
    {
        fixture.cfg.toolchains.clear();
        auto const tier = fixture.Start();
        REQUIRE(tier.has_value());
        REQUIRE(tier.value() != nullptr);
        CHECK(fixture.conditions.StateOf(NodeCondition::ScratchRootUnmappable) == Wire::ConditionState::NotEvaluated);
    }
}

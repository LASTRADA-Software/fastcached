// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeIoLoop.hpp"
#include "ScratchClaim.hpp"
#include "WorkerTier.hpp"

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

namespace
{

/// A discovery that finds nothing and counts how often it was asked.
class CountingDiscovery final: public Cc::IToolchainDiscovery
{
  public:
    /// @param calls Incremented per `Discover`; outlives this.
    explicit CountingDiscovery(int& calls) noexcept:
        _calls { calls }
    {
    }

    std::vector<Cc::ToolchainCandidate> Discover() override
    {
        ++_calls;
        return {};
    }

  private:
    int& _calls;
};

/// A worker naming one compiler and its own scheduler, so nothing is spawned to start it.
[[nodiscard]] NodeConfig Worker()
{
    NodeConfig cfg;
    cfg.schedulers = { "127.0.0.1:6675" };
    cfg.toolchains = { "/usr/bin/g++" };
    return cfg;
}

/// Everything a worker tier borrows, over fakes where the machine would be asked, and
/// all of it outliving any tier a case starts.
struct TierFixture
{
    Testing::ScratchDirectory scratch { "fc-worker-tier" };
    NullLogger logger;
    AtomicMetricsSink metrics;
    Distributed::OpenMembership membership;
    Testing::ScriptedHostAddresses addresses;
    ManualClock clock;
    CachedLocalityOracle locality { addresses, clock };
    NodeIoLoop io;
    std::unique_ptr<IHostFactsSource> host = MakeSystemHostFacts();
    Distributed::NodeCapacity capacity { .logicalCores = 16 };
    /// What the next `Start` builds from; a case edits it first.
    NodeConfig cfg = Worker();
    ConfiguredCredential credential { cfg, nullptr };
    int discoveryCalls = 0;
    /// How often the machine was asked for; a no-worker start must never ask.
    int machineBuilds = 0;
    /// Whether building the machine fails the way `temp_directory_path()` does for a
    /// `TMP` that names no directory.
    bool machineThrows = false;

    /// Start a tier for `cfg` on a machine of sixteen cores.
    [[nodiscard]] std::expected<std::unique_ptr<WorkerTier>, std::string> Start()
    {
        auto const makeMachine = [this] {
            ++machineBuilds;
            if (machineThrows)
                throw std::filesystem::filesystem_error("temp_directory_path",
                                                        std::make_error_code(std::errc::no_such_file_or_directory));
            return WorkerMachine { .runner = Cc::MakeProcessRunner(),
                                   .host = Cc::MakeToolchainHost(),
                                   .discovery = std::make_unique<CountingDiscovery>(discoveryCalls),
                                   .claimant = MakeLockFileScratchClaimant(),
                                   .scratchBase = scratch.Path() };
        };
        return WorkerTier::Start(WorkerTierParts { .cfg = cfg,
                                                   .reloader = nullptr,
                                                   .capacity = capacity,
                                                   .advertise = "127.0.0.1:6674",
                                                   .activation = SocketActivation::No,
                                                   .membership = membership,
                                                   .locality = locality,
                                                   .io = io,
                                                   .host = *host,
                                                   .cacheTier = nullptr,
                                                   .credential = credential,
                                                   .metrics = metrics,
                                                   .logger = logger },
                                 makeMachine);
    }
};

} // namespace

TEST_CASE("A node running no worker builds no worker tier, and a worker builds one", "[node][worker-tier]")
{
    // #206, #1387. The tier is the one place the question *does this node run a worker*
    // is answered, and on a node running none there is nothing: no pool thread, no
    // capacity sized to zero, no validator, no survey and no heartbeat to register with.
    // A `--scheduler` is named on it deliberately -- the cluster and enrollment commands
    // read it -- and it still builds nothing that could register.
    TierFixture fixture;

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
    TierFixture fixture;
    fixture.cfg.schedulers.clear();

    auto const refused = fixture.Start();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("no --scheduler"));
}

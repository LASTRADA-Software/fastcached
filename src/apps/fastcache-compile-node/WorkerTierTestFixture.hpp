// SPDX-License-Identifier: Apache-2.0
#pragma once

// What starting a worker tier takes, over fakes where the machine would be asked -- shared by the
// cases that start one, so the assembly `main` performs is spelled once for tests rather than once
// per file that needs a worker (#1364 needed one to prove every condition row is evaluated).

#include "NodeConditions.hpp"
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

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <tests/ScratchPath.hpp>

namespace FastCache::Node::WorkerTierTesting
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
[[nodiscard]] inline NodeConfig Worker()
{
    NodeConfig cfg;
    cfg.schedulers = { "127.0.0.1:6675" };
    cfg.toolchains = { "/usr/bin/g++" };
    return cfg;
}

/// Everything a worker tier borrows, over fakes where the machine would be asked, and
/// all of it outliving any tier a case starts.
struct WorkerTierFixture
{
    FastCache::Testing::ScratchDirectory scratch { "fc-worker-tier" };
    NullLogger logger;
    AtomicMetricsSink metrics;
    Distributed::OpenMembership membership;
    FastCache::Testing::ScriptedHostAddresses addresses;
    ManualClock clock;
    CachedLocalityOracle locality { addresses, clock };
    NodeIoLoop io;
    std::unique_ptr<IHostFactsSource> host = MakeSystemHostFacts();
    Distributed::NodeCapacity capacity { .logicalCores = 16 };
    /// The one advertised endpoint, owned HERE because `main` owns it: since #1440 the tier
    /// borrows it rather than building one, so the presence loop and the worker read the same
    /// value changing at the same moment.
    AnnouncedEndpoint announced { "127.0.0.1:6674" };
    /// What the next `Start` builds from; a case edits it first.
    NodeConfig cfg = Worker();
    ConfiguredCredential credential { cfg, nullptr };
    /// Where the tier answers its conditions; a case reads it after `Start`.
    NodeConditions conditions;
    int discoveryCalls = 0;
    /// How often the machine was asked for; a no-worker start must never ask.
    int machineBuilds = 0;
    /// Whether building the machine fails the way `temp_directory_path()` does for a
    /// `TMP` that names no directory.
    bool machineThrows = false;
    /// Where the machine claims its scratch root; empty for the scratch directory itself. A case
    /// sets it to a path no debug-prefix-map rule can spell to watch the worker say so.
    std::filesystem::path scratchBase {};

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
                                   .scratchBase = scratchBase.empty() ? scratch.Path() : scratchBase };
        };
        return WorkerTier::Start(WorkerTierParts { .cfg = cfg,
                                                   .reloader = nullptr,
                                                   .capacity = capacity,
                                                   .announced = announced,
                                                   .activation = SocketActivation::No,
                                                   .membership = membership,
                                                   .locality = locality,
                                                   .io = io,
                                                   .host = *host,
                                                   .cacheTier = nullptr,
                                                   .credential = credential,
                                                   // No cluster key: every case here is about the worker tier itself, and a
                                                   // worker with none is the ordinary single-machine shape.
                                                   .proofKey = nullptr,
                                                   // No roster either, for the same reason: a worker no other
                                                   // machine reaches checks no grant (#178).
                                                   .leaseRoster = nullptr,
                                                   .metrics = metrics,
                                                   .logger = logger,
                                                   .conditions = conditions },
                                 makeMachine);
    }
};

} // namespace FastCache::Node::WorkerTierTesting

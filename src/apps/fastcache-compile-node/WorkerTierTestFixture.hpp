// SPDX-License-Identifier: Apache-2.0
#pragma once

// What starting a worker tier takes, over fakes where the machine would be asked -- shared by the
// cases that start one, so the assembly `main` performs is spelled once for tests rather than once
// per file that needs a worker (#1364 needed one to prove every condition row is evaluated).

#include "EndpointDialerTestUtils.hpp"
#include "EndpointResolver.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeIoLoop.hpp"
#include "ScratchClaim.hpp"
#include "WorkerTier.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>

#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <core/platform/Clock.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/ScriptedHostEvents.hpp>
#include <tests/SteppedDrainWait.hpp>

namespace FastCache::Node::WorkerTierTesting
{

/// A refresh that publishes nothing and counts how often it was asked: a case publishes on
/// `WorkerTierFixture::announced` itself, as `EndpointResolver` would.
class CountingRefresh final: public IEndpointRefresh
{
  public:
    /// Counts the call.
    void Refresh() override
    {
        ++_calls;
    }

    /// @return How often `Refresh` was asked; read after the heartbeat is joined.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls.load();
    }

  private:
    std::atomic<int> _calls { 0 };
};

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

/// A machine whose compilers cannot be started at all: every spawn answers `NotSpawned`, which is
/// a probe that did not RUN. So a survey concludes *none could be asked* -- which is not fatal and
/// leaves the heartbeat running, serving nothing -- rather than ending the node.
class NeverSpawnsRunner final: public Cc::IProcessRunner
{
  public:
    /// @return A run that never started.
    [[nodiscard]] Cc::CompileRun RunCaptureCombined(std::span<std::string const> /*argv*/) override
    {
        return Cc::CompileRun {};
    }

    /// @return A run that never started.
    [[nodiscard]] Cc::CompileRun RunCaptureSplit(std::span<std::string const> /*argv*/) override
    {
        return Cc::CompileRun {};
    }
};

/// The scheduler a fixture worker registers with: its fleet's, on this machine.
inline constexpr std::string_view FleetScheduler = "127.0.0.1:6675";

/// A worker naming one compiler, so nothing is spawned to start it, and registering where its
/// formation record says: a learner of a fleet whose scheduler is `FleetScheduler`.
[[nodiscard]] inline NodeConfig Worker()
{
    NodeConfig cfg;
    cfg.toolchains = { "/usr/bin/g++" };
    cfg.formation = NodeFormationView { .mode = Cluster::NodeMode::Learner,
                                        .clusterId = "fleet-1",
                                        .createdAtUnixSeconds = 0,
                                        .foundedHere = false,
                                        .fleetMembers = {},
                                        .fleetSchedulers = { std::string { FleetScheduler } } };
    return cfg;
}

/// Everything a worker tier borrows, over fakes where the machine would be asked, and
/// all of it outliving any tier a case starts.
struct WorkerTierFixture
{
    FastCache::Testing::ScratchDirectory scratch { "fc-worker-tier" };
    /// Capturing rather than `NullLogger`, so a case that needs to read what the tier logged
    /// -- a withheld toolchain label's Warn among them -- has a record to read.
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    Distributed::OpenMembership membership;
    FastCache::Testing::ScriptedHostAddresses addresses;
    core::platform::ManualClock clock;
    CachedLocalityOracle locality { addresses, clock };
    NodeIoLoop io;
    std::unique_ptr<IHostFactsSource> host = MakeSystemHostFacts();
    Distributed::NodeCapacity capacity { .logicalCores = 16 };
    /// The one advertised endpoint, owned HERE because `main` owns it: since #1440 the tier
    /// borrows it rather than building one, so the presence loop and the worker read the same
    /// value changing at the same moment.
    AnnouncedEndpoint announced { "127.0.0.1:6674" };
    /// What the heartbeat refreshes at the top of every beat; publishes nothing.
    CountingRefresh endpoints;
    /// What the next `Start` builds from; a case edits it first.
    NodeConfig cfg = Worker();
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
    /// The host, scripted: a case fires what the OS would have.
    FastCache::Testing::ScriptedHostEvents hostEvents;
    /// Run at each poll of `suspendWait`; empty for none. A case whose heartbeat thread runs may block
    /// here -- boundedly, on the case's own thread -- for something that thread does, so no REAL
    /// deadline races it: the suspend's budget is measured on the stepped clock below.
    ///
    /// **A poll may never happen**: the drain tests its predicate before its first sleep, so a
    /// heartbeat that settles first leaves the hook unrun. And what a case can observe from here --
    /// a log line -- precedes the heartbeat's `Settle`, so the drain's own `Drained`/`Ceiling` still
    /// races that gap and must not be asserted.
    std::function<void()> suspendPoll;
    /// Where a suspend's bounded wait runs; a case reads how long it took by this clock.
    FastCache::Testing::SteppedDrainWait suspendWait { [this] {
        if (suspendPoll)
            suspendPoll();
    } };
    /// Whether the machine's compilers cannot be started (`NeverSpawnsRunner`) rather than asked
    /// for real -- how a case launches a heartbeat that serves nothing without ending the node.
    bool compilersCannotRun = false;
    /// What the heartbeat's scheduler dials answer, one frame per dial, read by the first `Start`.
    /// EMPTY by default: a case that launches no heartbeat expects no dial at all, so any is a
    /// `FAIL` in the fake's own voice -- which is how a case asserts that a host event dialled nobody
    /// on the thread that delivered it.
    std::vector<std::vector<std::byte>> heartbeatReplies {};

    /// The dialer `Dialer()` builds from `heartbeatReplies`; reached through it, never directly.
    std::optional<FastCache::Testing::ScriptedDialer> builtDialer;

    /// @return How the heartbeat reaches a scheduler; built from `heartbeatReplies` at the first `Start`.
    [[nodiscard]] FastCache::Testing::ScriptedDialer& Dialer()
    {
        if (!builtDialer.has_value())
            builtDialer.emplace(heartbeatReplies);
        return *builtDialer;
    }
    /// Where a supervisor handed the node surface over; `AsConfigured` for a node that binds its
    /// own, which is every case but the socket-activation ones.
    ActivatedNodeEndpoint activatedNodeEndpoint = AsConfigured;
    /// Where the tier records the lease check it built; a case reads it after `Start`.
    LeaseCheckInForce leaseCheck;
    /// Where each started tier registers, one per `Start` -- built from `cfg` and
    /// `activatedNodeEndpoint` as they stand THEN, as `main` builds the process's one -- and kept for
    /// as long as the fixture, since a tier borrows it.
    std::vector<std::unique_ptr<AppliedSchedulers>> schedulers;

    /// Start a tier for `cfg` on a machine of sixteen cores.
    [[nodiscard]] std::expected<std::unique_ptr<WorkerTier>, NodeRefusal> Start()
    {
        auto const makeMachine = [this] {
            ++machineBuilds;
            if (machineThrows)
                throw std::filesystem::filesystem_error("temp_directory_path",
                                                        std::make_error_code(std::errc::no_such_file_or_directory));
            return WorkerMachine { .runner =
                                       compilersCannotRun ? std::make_unique<NeverSpawnsRunner>() : Cc::MakeProcessRunner(),
                                   .host = Cc::MakeToolchainHost(),
                                   .discovery = std::make_unique<CountingDiscovery>(discoveryCalls),
                                   .claimant = MakeLockFileScratchClaimant(),
                                   .scratchBase = scratchBase.empty() ? scratch.Path() : scratchBase };
        };
        schedulers.push_back(std::make_unique<AppliedSchedulers>(cfg, activatedNodeEndpoint));
        return WorkerTier::Start(WorkerTierParts { .cfg = cfg,
                                                   .reloader = nullptr,
                                                   .capacity = capacity,
                                                   .announced = announced,
                                                   .endpoints = endpoints,
                                                   .activatedNodeEndpoint = activatedNodeEndpoint,
                                                   .schedulers = *schedulers.back(),
                                                   .membership = membership,
                                                   .locality = locality,
                                                   .io = io,
                                                   .host = *host,
                                                   .cacheTier = nullptr,
                                                   // Nothing proves: every case here is about the worker tier
                                                   // itself, against a scripted fleet that serves no handshake.
                                                   .prover = nullptr,
                                                   // No roster either, for the same reason: a worker no other
                                                   // machine reaches checks no grant (#178).
                                                   .leaseRoster = nullptr,
                                                   .leaseCheck = leaseCheck,
                                                   .metrics = metrics,
                                                   .logger = logger,
                                                   .conditions = conditions,
                                                   .hostEvents = hostEvents,
                                                   .suspendWait = suspendWait,
                                                   .schedulerDialer = Dialer() },
                                 makeMachine);
    }
};

} // namespace FastCache::Node::WorkerTierTesting

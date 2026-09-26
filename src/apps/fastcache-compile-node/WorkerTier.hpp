// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "EndpointDialer.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeReload.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeToolchains.hpp"
#include "SchedulerLink.hpp"
#include "ScratchClaim.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CompileJob.hpp>
#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <WorkerProtocol.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

class CacheTier;
class FleetSampler;
class NodeIoLoop;

/// What a worker spawns, walks and writes through: the machine, as seams.
///
/// Owned by the tier once handed over, and handed over rather than built inside it so a
/// test can construct a worker without a compiler on the machine running it. `discovery`
/// borrows `runner` and `host`, which is why the three travel together and are moved as
/// pointers: the addresses it borrowed survive the move.
struct WorkerMachine
{
    std::unique_ptr<Cc::IProcessRunner> runner;         ///< Spawns every compiler this worker runs.
    std::unique_ptr<Cc::IToolchainHost> host;           ///< What a toolchain fingerprint reads.
    std::unique_ptr<Cc::IToolchainDiscovery> discovery; ///< This machine's compilers; borrows the two above.
    std::unique_ptr<IScratchClaimant> claimant;         ///< Claims the scratch root exclusively.
    std::filesystem::path scratchBase;                  ///< Where scratch roots are claimed.
};

/// This machine's own worker seams: the real process runner, toolchain host, discovery
/// and scratch claimant, claiming under `ScratchBaseDirectory()`.
///
/// Handed to `WorkerTier::Start` as a factory and never called by `main` itself, because
/// `ScratchBaseDirectory()` asks `temp_directory_path()`, which THROWS for a `TMP` naming
/// a directory that is not there -- a question a node running no worker has no business
/// asking.
/// @return The machine, for `WorkerTier::Start`.
[[nodiscard]] WorkerMachine MakeSystemWorkerMachine();

/// Builds the machine a worker runs on, asked only once a worker is decided.
using WorkerMachineFactory = std::function<WorkerMachine()>;

/// What a worker tier borrows from the node that starts it, all of which outlive it.
struct WorkerTierParts
{
    NodeConfig const& cfg;                     ///< The configuration the node started with.
    NodeReloader const* reloader;              ///< The live configuration; null with no file.
    Distributed::NodeCapacity const& capacity; ///< What `NodeCapacityOf` made of this machine.
    /// Where this node tells clients to reach it, right now.
    ///
    /// **Owned by `main` rather than by this tier since
    /// [#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440), because a node
    /// that runs no worker still has to announce an address** -- and there may be exactly ONE
    /// of these per process. A second would be a second value changing at a second moment,
    /// which is the defect #1279 closed *inside* the worker reopened one level up, between the
    /// worker and the presence loop: the registration and the lease check would agree with
    /// each other and disagree with what the fleet page was told.
    ///
    /// This tier remains its only PUBLISHER, which is what keeps that property cheap: an
    /// address is learned by re-surveying, and only a worker re-surveys. The presence loop
    /// reads it and never writes.
    AnnouncedEndpoint& announced;
    SocketActivation activation;                      ///< Whether a supervisor handed the port over.
    Distributed::IMembershipOracle const& membership; ///< Who may send a compile at all.
    ILocalityOracle const& locality;                  ///< Who may cordon this worker.
    NodeIoLoop& io;                                   ///< The reactor a compile's reply returns to.
    IHostFactsSource const& host;                     ///< The hostname a registration labels.
    CacheTier const* cacheTier;                       ///< Null on a node with no cache.
    ICredentialSource const& credential;              ///< What the heartbeat presents.
    /// How this machine proves WHICH machine it is to a scheduler (#178), or null where nothing
    /// proves -- a test whose scripted fleet serves no handshake. One instance per process,
    /// shared with the presence loop.
    NodeProofClient const* prover;
    /// What a lease grant is verified against (#178), or null when this node verifies none --
    /// legal only where no other machine can reach it. Owned by `main`'s `NodeRoster`.
    Distributed::ILeaseRoster const* leaseRoster;
    IMetricsSink& metrics; ///< Where the worker counts.
    ILogger& logger;       ///< Where it reports.
    /// Where the worker's conditions are answered (#1364): whether its scratch root can be
    /// written into a debug-prefix-map rule. Evaluated where the root is claimed, beside the
    /// warning that says the same thing at startup.
    NodeConditions& conditions;
};

class WorkerTier;

/// The heartbeat thread of a started worker, which stops and joins when destroyed.
///
/// A handle of its own rather than a member of the tier, because the two have different
/// lifetimes in `WorkerBody`: the tier is built before the node surface that routes to
/// its responder, and the heartbeat reads the sampler that is built long after, so the
/// thread must be joined before the sampler goes while the tier outlives the surface.
class WorkerHeartbeat
{
  public:
    /// @param thread The running heartbeat.
    explicit WorkerHeartbeat(std::jthread thread) noexcept:
        _thread { std::move(thread) }
    {
    }

  private:
    std::jthread _thread;
};

/// The node's worker: survey, scratch root, job runner, lease check, slot cap, compile
/// responder and heartbeat, owned as one thing (#1387).
///
/// **The mirror of `CacheTier` and `SchedulerTier`, and null on a node that runs none**
/// (`--slots=0`, #206). A node running no worker therefore builds no pool thread, no
/// capacity sized to zero and no validator nothing will ask -- the objects that existed
/// only so the code around them could stay unconditional. It was loose locals in
/// `WorkerBody`, a function sitting at the cognitive-complexity ceiling the build
/// enforces and in no test target, so the question *does this node have a worker* was
/// asked at each site that touched one and answered by nothing a test could reach.
class WorkerTier
{
  public:
    /// Build the worker, or nothing when this node runs none.
    ///
    /// The cheap half of the survey runs here -- deciding which compilers to serve,
    /// which refuses a malformed `--toolchain` by name -- and the expensive walk runs
    /// on the heartbeat thread `Launch` starts. Binds nothing: the compile verbs are
    /// answered on the node's one `0xFC` listener through `Responder()`.
    ///
    /// **A worker always has a scheduler link, and that is enforced here rather than
    /// assumed.** The startup table refuses a worker with no `--scheduler`; a
    /// configuration that reached this anyway is a defect in that table, and it is
    /// refused by name rather than left to start a worker that never registers.
    ///
    /// **Nothing of the machine is built for a node running no worker**, the scratch base
    /// included: @p makeMachine is called only once a worker is decided.
    /// @param parts What the node lends the tier.
    /// @param makeMachine Builds the machine's seams; not called when no worker runs.
    /// @return The tier; null when `RunsWorker` is false; or why the node must not start.
    [[nodiscard]] static std::expected<std::unique_ptr<WorkerTier>, std::string> Start(
        WorkerTierParts const& parts, WorkerMachineFactory const& makeMachine);

    ~WorkerTier() = default;

    WorkerTier(WorkerTier const&) = delete;
    WorkerTier& operator=(WorkerTier const&) = delete;
    WorkerTier(WorkerTier&&) = delete;
    WorkerTier& operator=(WorkerTier&&) = delete;

    /// Start surveying, registering and heartbeating.
    ///
    /// Separate from `Start` because the sampler the heartbeat hands history through is
    /// built after the admin surface, which reads this tier's capacity.
    /// It no longer takes the `FleetSampler`: history is the MACHINE's and rides
    /// NODE-ANNOUNCE from the presence loop, which runs on every node (#1440). This tier
    /// hands over none, so it reads none.
    /// @param statusClock The clock `node-status` differences a registration against.
    /// @return The running heartbeat.
    [[nodiscard]] WorkerHeartbeat Launch(core::platform::IClock const& statusClock);

    /// @return What answers the compile family on this node's `0xFC` listener.
    [[nodiscard]] CompileResponder& Responder() noexcept
    {
        return _responder;
    }

    /// @return The slot cap, byte budget and drain every compile door spends.
    [[nodiscard]] CompileCapacity& Capacity() noexcept
    {
        return _capacity;
    }

    /// @return Where the survey and the registrations are published for `node-status`.
    [[nodiscard]] NodeRuntimeState& Runtime() noexcept
    {
        return _runtime;
    }

    /// Where this worker is telling clients to reach it, now.
    ///
    /// The seam itself rather than a string, so a caller cannot take a copy that then
    /// goes stale -- which is the whole defect #1279 records, and the reason this
    /// returns a reference to an interface holding no value of its own.
    /// @return The source; it lives as long as this tier.
    [[nodiscard]] Cc::IAdvertisedEndpointSource const& Advertised() const noexcept
    {
        return _announced;
    }

    /// @return The slots this worker offers and enforces.
    [[nodiscard]] std::uint32_t Slots() const noexcept
    {
        return _slots;
    }

    /// @return How many toolchains the cheap half of the survey found.
    [[nodiscard]] std::size_t StartupToolchainCount() const noexcept
    {
        return _discovered.entries.size();
    }

    /// @return The directory this worker compiles in.
    [[nodiscard]] std::filesystem::path const& ScratchRoot() const noexcept
    {
        return _jobs.ScratchRoot();
    }

    /// The compiler a served fingerprint names, asked of the runner so a re-survey is
    /// reflected at once.
    /// @param fingerprint A toolchain fingerprint.
    /// @return Its compiler, or empty when it is not served.
    [[nodiscard]] std::string CompilerFor(std::string_view fingerprint) const
    {
        return _jobs.CompilerFor(fingerprint);
    }

    /// Stop admitting compiles, and wait for the ones admitted to finish.
    void StopAndDrain();

    /// Whether the worker ended in a way a supervisor must read as a failure: a survey
    /// that found nothing to serve, or a fleet that is not the one `--cluster-id` names.
    /// @return True when the process should exit non-zero.
    [[nodiscard]] bool EndedInRefusal() const noexcept
    {
        return _surveyFoundNothing || _fleetAssertionFailed;
    }

  private:
    WorkerTier(WorkerTierParts const& parts,
               WorkerMachine machine,
               DiscoveredToolchains discovered,
               std::unique_ptr<IScratchClaim> scratchClaim,
               std::unique_ptr<Distributed::WorkerLeaseState> leaseState,
               Cc::LeaseValidator validator,
               SchedulerLink link,
               std::uint32_t slots);

    /// The heartbeat thread's body: the first survey, then a round per interval.
    void Heartbeat(std::stop_token const& stop, core::platform::IClock const& statusClock);

    /// One registrar per served toolchain, carrying this machine's capacity record and
    /// the endpoint in force when it is called.
    [[nodiscard]] std::vector<Cc::WorkerRegistrar> RegistrarsFor(std::map<std::string, ServedToolchain> const& served);

    /// Make @p served what the compile port and the registrations answer, in that order.
    void Serve(std::map<std::string, ServedToolchain> served);

    /// Advertise @p endpoint from now on, retiring the registrations under the old one.
    ///
    /// **The one caller of `AnnouncedEndpoint::Publish`, and the ordering is the whole
    /// point.** Publishing before the registrars are rebuilt is what makes the new
    /// address the one they carry; rebuilding through `AdoptRegistrars` is what queues
    /// the old `(fingerprint, endpoint)` entries for withdrawal instead of destroying
    /// the `WorkerId` they need to be retired with. Either half alone leaves the
    /// scheduler leasing an address this worker does not answer on.
    ///
    /// The lease check moves with it, because the validator reads the same seam -- so
    /// there is no moment at which this worker verifies against one address while the
    /// fleet holds another. That is the property a second reader of the configuration
    /// could not have.
    /// @param endpoint The new endpoint; non-empty, per `AdvertisedEndpointChange`.
    void AnnounceAs(std::string endpoint);

    NodeConfig const& _cfg;
    NodeReloader const* _reloader;
    CacheTier const* _cacheTier;
    ICredentialSource const& _credential;

    /// How this machine proves itself, or null where nothing proves. Borrowed, and it outlives
    /// this tier: `main` declares it above the tier and destroys it after.
    NodeProofClient const* _prover;
    IMetricsSink& _metrics;
    ILogger& _logger;
    /// What this worker advertises, and the one thing the registration and the lease
    /// check both read. Borrowed from `main`, which declares it above this tier and destroys
    /// it after -- `_prover`'s arrangement, for `_prover`'s reason.
    AnnouncedEndpoint& _announced;
    WorkerMachine _machine;
    core::platform::SteadyClock _toolchainClock;
    DiscoveredToolchains _discovered;
    std::map<std::string, ServedToolchain> _toolchains;
    std::unique_ptr<IScratchClaim> _scratchClaim;
    Cc::CompileJobRunner _jobs;
    std::vector<std::string> _appliedExtraArgs;
    std::unique_ptr<Distributed::WorkerLeaseState> _leaseState;
    Cc::WorkerProtocol _protocol;
    std::uint32_t _slots;
    core::async::ThreadPoolExecutor _pool;
    CompileCapacity _capacity;
    CompileResponder _responder;
    NodeRuntimeState _runtime;
    CompileCacheWire::CapacityFields _advertisedWire;
    Cc::CredentialNotice _registrarNotice;
    std::vector<Cc::WorkerRegistrar> _registrars;
    std::vector<Cc::WorkerRegistrar> _withdrawals;
    BlockingEndpointDialer _dialer;
    SchedulerLink _link;
    std::atomic<bool> _surveyFoundNothing { false };
    std::atomic<bool> _fleetAssertionFailed { false };
};

} // namespace FastCache::Node

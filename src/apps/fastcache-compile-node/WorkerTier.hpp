// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "EndpointDialer.hpp"
#include "EndpointResolver.hpp"
#include "HostEventInbox.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeRefusal.hpp"
#include "NodeReload.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeToolchains.hpp"
#include "RefusedArguments.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"
#include "ScratchClaim.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

/// The label a REGISTER for @p toolchain carries: its own, or none when a scheduler would
/// refuse the whole registration over it (`Cc::ToolchainLabelWithheldBecause`).
///
/// The label is display only, so the worker registers without it rather than not at all --
/// and says so once per toolchain at Warn, naming the compiler and why, because a fleet page
/// showing no name for a compiler this node serves is otherwise a question nobody can answer.
/// @param toolchain What this worker serves.
/// @param logger Where the Warn goes.
/// @return The label to send; empty when it is withheld.
[[nodiscard]] std::string RegisteredToolchainLabel(ServedToolchain const& toolchain, ILogger& logger);

/// How a worker that served and then gave up ends, when it did.
///
/// A survey that found nothing to compile with is `ToolchainSurvey`, a FAILURE: a compiler
/// installed since is found by the next one.
/// @param surveyFoundNothing Whether the survey found nothing to serve.
/// @return The cause, or nullopt when the worker stopped cleanly.
[[nodiscard]] constexpr std::optional<NodeRefusalCause> WorkerEnding(bool surveyFoundNothing) noexcept
{
    if (surveyFoundNothing)
        return NodeRefusalCause::ToolchainSurvey;
    return std::nullopt;
}

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
    NodeReloader const* reloader {};           ///< The live configuration; null with no file.
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
    /// Its only PUBLISHER is `EndpointResolver` (`endpoints`), which derives it from the
    /// configuration in force and the address this machine routes from. This tier and the
    /// presence loop read it; the tier re-registers whenever it differs from the endpoint its
    /// registrations were filed under. The lease check does NOT read it: it reads the endpoint the
    /// tier is registered under (`WorkerTier::Advertised`), which follows at the re-registration.
    AnnouncedEndpoint& announced;
    /// Re-derives `announced` when it is due, asked at the top of every heartbeat so a network
    /// change the heartbeat was woken for is published before the round reads it; and watched, so
    /// a move published on any other thread wakes the heartbeat to re-register at once.
    IEndpointRefresh& endpoints;
    /// Where a supervisor handed the node surface over; disengaged when the node binds its own. One
    /// value for both questions it answers -- whether the socket was handed over, which the lease
    /// check asks, and where this node's own scheduler answers, which the registration asks -- so
    /// the two cannot disagree.
    ActivatedNodeEndpoint activatedNodeEndpoint;
    /// Where the heartbeat registers, re-read at every round (`AppliedSchedulers`): the process's one,
    /// shared with the presence loop and told every applied state by the consensus tier.
    ISchedulerEndpointSource const& schedulers;
    Distributed::IMembershipOracle const& membership; ///< Who may send a compile at all.
    /// Who may cordon this worker, and what each heartbeat reports it answers on: ONE set, the
    /// one the ticket audience checks, so a dial hint never names an address this node refuses.
    ILocalityOracle const& locality;
    NodeIoLoop& io;                ///< The reactor a compile's reply returns to.
    IHostFactsSource const& host;  ///< The hostname a registration labels.
    CacheTier const* cacheTier {}; ///< Null on a node with no cache.
    /// How this machine proves WHICH machine it is to a scheduler (#178), or null where nothing
    /// proves -- a test whose scripted fleet serves no handshake. One instance per process,
    /// shared with the presence loop.
    NodeProofClient const* prover {};
    /// What a lease grant is verified against (#178), or null when this node verifies none --
    /// legal only where no other machine can reach it. Owned by `main`'s `NodeRoster`.
    Distributed::ILeaseRoster const* leaseRoster {};
    /// Where the lease check this worker builds is recorded, for the reload guard
    /// (`ReloadCheckWith`). Owned by `main`, across every body, beside the reloader that reads it.
    LeaseCheckInForce& leaseCheck;
    IMetricsSink& metrics; ///< Where the worker counts.
    ILogger& logger;       ///< Where it reports.
    /// Where the worker's conditions are answered (#1364): whether its scratch root can be
    /// written into a debug-prefix-map rule. Evaluated where the root is claimed, beside the
    /// warning that says the same thing at startup.
    NodeConditions& conditions;
    /// Where the host's suspend, resume and network events arrive. The tier listens for as long
    /// as it exists.
    IHostEvents& hostEvents;
    /// Where a suspend's bounded wait for its withdrawal blocks and reads time.
    IDrainWait& suspendWait;
    /// How the heartbeat reaches a scheduler -- its rounds and a suspend's one withdrawal. Only
    /// the heartbeat thread dials through it, never the thread a host event arrives on.
    IEndpointDialer& schedulerDialer;
};

class WorkerTier;

/// Wait out one heartbeat interval, handling a suspend on the way.
///
/// **A suspend is withdrawn and then waited PAST**: announcing right after withdrawing would
/// re-register a machine that is about to sleep. So the withdrawal runs, the suspend waiting on it
/// is let go (`HostEventInbox::Settle`), and the wait goes on -- unless something else is pending
/// beside it, a resume or a network change, which ends the wait so the round runs. Any other wake
/// ends it too. Runs on the heartbeat's thread, which is what makes every dial the withdrawal makes
/// one the delivering thread never makes.
///
/// A free function over the two objects rather than a `WorkerTier` member, because the heartbeat
/// loop is reached by no test and this is the rule a suspend depends on.
/// @param stop Ends the wait, and the heartbeat.
/// @param capacity Whose wait is woken by a cordon and by a host event.
/// @param inbox Where host events wait for this thread.
/// @param announcedCordon The cordon the round that just ran carried.
/// @param interval How long to wait when nothing happens.
/// @param withdrawForSuspend Retires every registration and tells the scheduler.
/// @return What ended the wait; `Stopped` ends the heartbeat.
[[nodiscard]] HeartbeatWake AwaitNextRound(std::stop_token const& stop,
                                           CompileCapacity& capacity,
                                           HostEventInbox& inbox,
                                           bool announcedCordon,
                                           std::chrono::milliseconds interval,
                                           std::function<void()> const& withdrawForSuspend);

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

/// Adopt a reloaded compile-argument allowlist, and say so when it changed.
///
/// Out here rather than private to the heartbeat, so the one decision a reload makes about
/// refused arguments is one a test can reach with the objects the tier holds.
/// @param jobs The runner the set is applied to.
/// @param refused What this worker has refused; re-judged against the set now in force when the
///        set changes, because a changed allowlist is the operator acting on exactly that report.
/// @param logger Where the change is announced, at Warn.
/// @param inForce The set applied now; replaced when the candidate differs.
/// @param candidate The set the live configuration names.
void AdoptAllowlist(Cc::CompileJobRunner& jobs,
                    RefusedArgumentsReport& refused,
                    ILogger& logger,
                    std::vector<std::string>& inForce,
                    std::vector<std::string> const& candidate);

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
    /// assumed.** It is aimed where the formation record says this node registers
    /// (`SchedulersOf`), never at `--scheduler`; a record that names nowhere is refused by
    /// name rather than left to start a worker that never registers.
    ///
    /// **Nothing of the machine is built for a node running no worker**, the scratch base
    /// included: @p makeMachine is called only once a worker is decided.
    /// @param parts What the node lends the tier.
    /// @param makeMachine Builds the machine's seams; not called when no worker runs.
    /// @return The tier; null when `RunsWorker` is false; or why the node must not start.
    [[nodiscard]] static std::expected<std::unique_ptr<WorkerTier>, NodeRefusal> Start(
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
    /// @param reachability How loudly a scheduler that does not answer, or refuses, is said: the
    ///        process's one, shared with the presence loop. Must outlive the returned heartbeat.
    /// @return The running heartbeat.
    [[nodiscard]] WorkerHeartbeat Launch(core::platform::IClock const& statusClock, SchedulerReachability& reachability);

    /// @return Where this worker's next round registers, in the order it tries them, as the
    ///         heartbeat last read them (`ISchedulerEndpointSource`). Read it before `Launch`: a round
    ///         re-reads it on the heartbeat's own thread.
    [[nodiscard]] std::vector<std::string> const& RegistersWith() const noexcept
    {
        return _link.Configured();
    }

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

    /// The endpoint this worker is REGISTERED under, now: the one the scheduler signs into its
    /// grants and the one the lease check verifies them against.
    ///
    /// Not the published endpoint (`WorkerTierParts::announced`), which `EndpointResolver` moves on
    /// its own thread: the scheduler goes on granting for the registered endpoint until the heartbeat
    /// re-registers, and a grant for it is honoured until then. This moves at that one point.
    ///
    /// The seam itself rather than a string, so a caller cannot take a copy that then
    /// goes stale -- which is the whole defect #1279 records, and the reason this
    /// returns a reference to an interface holding no value of its own.
    /// @return The source; it lives as long as this tier.
    [[nodiscard]] Cc::IAdvertisedEndpointSource const& Advertised() const noexcept
    {
        return *_registeredAs;
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

    /// One registrar per served toolchain, carrying this machine's capacity record and
    /// the endpoint in force when it is called.
    ///
    /// A pure query -- it reads `_toolchains`' entries and `_announced`'s current value and
    /// builds a `Cc::WorkerRegistrar` per one, whose own constructor only stores what it is
    /// given -- so it is public rather than reached through a test-only seam: calling it
    /// does not register, heartbeat or withdraw anything, and a case can ask it directly
    /// for what a real round would build without spinning the heartbeat thread that is
    /// `Serve`'s and `FollowAnnouncedEndpoint`'s only production caller.
    /// @param served What this worker currently serves, fingerprint to toolchain.
    /// @return One registrar per entry of @p served.
    [[nodiscard]] std::vector<Cc::WorkerRegistrar> RegistrarsFor(std::map<std::string, ServedToolchain> const& served);

    /// Stop admitting compiles, and wait for the ones admitted to finish.
    void StopAndDrain();

    /// How the worker ended, when a supervisor must not read it as a clean stop
    /// (`WorkerEnding`).
    /// @return The cause the process ends with, or nullopt for a clean stop.
    [[nodiscard]] std::optional<NodeRefusalCause> Ending() const noexcept
    {
        return WorkerEnding(_surveyFoundNothing);
    }

  private:
    WorkerTier(WorkerTierParts const& parts,
               WorkerMachine machine,
               DiscoveredToolchains discovered,
               std::unique_ptr<IScratchClaim> scratchClaim,
               std::unique_ptr<Distributed::WorkerLeaseState> leaseState,
               std::unique_ptr<AnnouncedEndpoint> registeredAs,
               Cc::LeaseValidator validator,
               SchedulerLink link,
               std::uint32_t slots);

    /// The heartbeat thread's body: the first survey, then a round per interval.
    void Heartbeat(std::stop_token const& stop,
                   core::platform::IClock const& statusClock,
                   SchedulerReachability& reachability);

    /// Make @p served what the compile port and the registrations answer, in that order.
    void Serve(std::map<std::string, ServedToolchain> served);

    /// Re-file the registrations under the endpoint now published, when it is not the one they
    /// were filed under (`_registeredAs`).
    ///
    /// The endpoint is `EndpointResolver`'s to publish; this is the worker's half of a move.
    /// Rebuilding through `AdoptRegistrars` is what queues the old `(fingerprint, endpoint)`
    /// entries for withdrawal instead of destroying the `WorkerId` they need to be retired with,
    /// and the round that follows withdraws them before it registers the new ones. Compared
    /// against what was REGISTERED rather than against the previous publication, so a move and
    /// its reversal between two beats re-register nothing, and three moves in three beats
    /// withdraw each superseded endpoint exactly once.
    ///
    /// The lease check does NOT move when the endpoint is published: the validator reads the
    /// endpoint this worker is REGISTERED under (`RegisterUnder`), which moves here, with the
    /// registrars, as the old entries are queued for withdrawal -- so between the publication and
    /// this call a grant the scheduler signed for the endpoint still registered stays valid.
    /// @param statusClock What `node-status` stamps against, for the registrations this retires.
    void FollowAnnouncedEndpoint(core::platform::IClock const& statusClock);

    /// Make @p endpoint the one this worker is registered under and verifies grants against.
    /// @param endpoint What the registrars in force were just built under.
    void RegisterUnder(std::string endpoint);

    /// One registrar per entry of @p served, every one filed under @p endpoint.
    /// @param served What this worker serves, fingerprint to toolchain.
    /// @param advertised The endpoint the whole set is registered under, read once by the caller.
    /// @return The registrars.
    [[nodiscard]] std::vector<Cc::WorkerRegistrar> RegistrarsAt(std::map<std::string, ServedToolchain> const& served,
                                                                std::string const& advertised);

    /// Retire every registration and tell the scheduler, before the machine sleeps.
    /// @param round What to withdraw and where to log.
    /// @param statusClock What `node-status` stamps against.
    void WithdrawForSuspend(HeartbeatRound const& round, core::platform::IClock const& statusClock);

    NodeConfig const& _cfg;
    NodeReloader const* _reloader;
    CacheTier const* _cacheTier;

    /// How this machine proves itself, or null where nothing proves. Borrowed, and it outlives
    /// this tier: `main` declares it above the tier and destroys it after.
    NodeProofClient const* _prover;
    IMetricsSink& _metrics;
    ILogger& _logger;
    /// What this worker advertises, and the one thing the registration and the lease
    /// check both read. Borrowed from `main`, which declares it above this tier and destroys
    /// it after -- `_prover`'s arrangement, for `_prover`'s reason.
    AnnouncedEndpoint& _announced;
    /// Publishes `_announced` when due; refreshed at the top of every beat, and watched so a move
    /// published on another thread wakes the heartbeat. Borrowed from `main`.
    IEndpointRefresh& _endpoints;
    /// The endpoint the registrars in force were built under, and what the lease check verifies
    /// a grant against (`Advertised`). Published by the heartbeat thread alone, as it rebuilds the
    /// registrars. A heap object, because the validator built before this tier borrows it.
    std::unique_ptr<AnnouncedEndpoint> _registeredAs;
    /// What each heartbeat reports this machine answers on: the locality oracle's own set
    /// (`HeartbeatRound::locality`). Borrowed from `main`, which declares it above this tier.
    ILocalityOracle const& _locality;
    WorkerMachine _machine;
    core::platform::SteadyClock _toolchainClock;
    DiscoveredToolchains _discovered;
    std::map<std::string, ServedToolchain> _toolchains;
    std::unique_ptr<IScratchClaim> _scratchClaim;
    Cc::CompileJobRunner _jobs;
    std::vector<std::string> _appliedExtraArgs;
    std::unique_ptr<Distributed::WorkerLeaseState> _leaseState;
    /// Which arguments this worker refused, said as a condition and once per argument in the
    /// log. Declared before `_protocol`, which borrows it, so member order keeps it alive.
    RefusedArgumentsReport _refusedArguments;
    Cc::WorkerProtocol _protocol;
    std::uint32_t _slots;
    core::async::ThreadPoolExecutor _pool;
    CompileCapacity _capacity;
    CompileResponder _responder;
    NodeRuntimeState _runtime;
    CompileCacheWire::CapacityFields _advertisedWire;
    std::vector<Cc::WorkerRegistrar> _registrars;
    std::vector<Cc::WorkerRegistrar> _withdrawals;
    IEndpointDialer& _dialer;
    SchedulerLink _link;
    std::atomic<bool> _surveyFoundNothing { false };
    std::atomic<bool> _addressCapNoticed { false };
    /// Host events for the heartbeat thread. After `_capacity`, whose wake it calls.
    HostEventInbox _hostInbox;
    /// Listening, from construction to destruction. After `_hostInbox`, so it goes first.
    HostEventSubscription _hostSubscription;

    /// Wakes the heartbeat when the published endpoint moves, so it re-registers at once.
    class HeartbeatWakeOnMove final: public IEndpointMoveSink
    {
      public:
        /// @param capacity Whose heartbeat wait is ended.
        explicit HeartbeatWakeOnMove(CompileCapacity& capacity) noexcept:
            _capacity { capacity }
        {
        }

        /// Ends the heartbeat's wait, as a host event does.
        void OnEndpointMoved() noexcept override
        {
            _capacity.WakeHeartbeat();
        }

      private:
        CompileCapacity& _capacity;
    };

    /// After `_capacity`, whose wake it calls.
    HeartbeatWakeOnMove _moveWake { _capacity };
    /// Watching from construction to destruction. After `_moveWake`, so it goes first.
    EndpointMoveWatch _moveWatch;
};

} // namespace FastCache::Node

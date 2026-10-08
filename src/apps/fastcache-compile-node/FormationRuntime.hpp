// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ConsensusTier.hpp"
#include "DiscoveryTier.hpp"
#include "EndpointDialer.hpp"
#include "EnrollChannel.hpp"
#include "FleetProbe.hpp"
#include "FormationController.hpp"
#include "FormationLoop.hpp"
#include "LiveNodeConfig.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Cluster/SplitEvidence.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/StopAwareWait.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/SrvResolver.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file FormationRuntime.hpp
/// One serving body's formation, assembled: the controller and the production seams it acts
/// through, the hooks consensus tells it on, and the thread that beats it.
namespace FastCache::Node
{

/// The rules a shaped configuration is judged by: production's are `StartupPolicyRejection`.
/// @param cfg The configuration a record shaped.
/// @return Why a start into it is refused, or nothing.
using StartupRules = std::optional<std::string> (*)(NodeConfig const& cfg);

/// Production's `IShapeJudge`: the record applied to the configuration in force, judged by the startup
/// rules -- what a restart into that shape would be refused by.
///
/// **Both of its inputs are read at the moment it judges**, never captured when it was built: the
/// configuration through `INodeConfigSource`, so an accepted reload of a reloadable flag changes what
/// it judges; and the remembered fleet endpoints, as the reform that adopts the record and every
/// restart read them -- so a move must have written what it leaves in that file BEFORE the save it is
/// judged in (`DissolveInto`).
class StartupShapeJudge final: public IShapeJudge
{
  public:
    /// @param config The configuration in force, read at each judgement; a move's record is applied
    ///        to a copy. Must outlive this.
    /// @param endpoints The fleet endpoints remembered, which a record's schedulers are read with.
    /// @param rules The rules judged: `StartupPolicyRejection` in production; a case may state one
    ///        rule of its own through the same judge.
    StartupShapeJudge(INodeConfigSource const& config, Cluster::FleetEndpointsFile& endpoints, StartupRules rules);

    /// @copydoc IShapeJudge::RefusalOf
    [[nodiscard]] std::optional<std::string> RefusalOf(Cluster::FormationRecord const& next) const override;

  private:
    INodeConfigSource const& _config;        ///< Read at each judgement.
    Cluster::FleetEndpointsFile& _endpoints; ///< Read at each judgement.
    StartupRules _rules;                     ///< What the shaped configuration is judged by.
};

/// The time, randomness and network a body's formation acts through: INJECTED, so `main` constructs
/// the production ones once and a test hands in a `ManualClock`, a scripted random source, a dialer
/// that reaches nobody and a scripted SRV resolver -- never built inside the runtime.
struct FormationRuntimeParts
{
    core::platform::IWallClock const& wall; ///< What a minted cluster's creation time is read from.
    ISecureRandom& random;                  ///< Where nonces and minted cluster ids come from.
    IEndpointDialer& dialer;                ///< What an `Enroll` and a fleet probe dial through.
    ISrvResolver const& srv;                ///< What answers the SRV question for seeds.
    /// What the beat waits `TickInterval` through, AND the clock every interval the controller keeps
    /// is read from (`IStopAwareWait::Clock`): one object, so the two cannot be supplied apart.
    IStopAwareWait const& wait;
};

/// What a body's formation acts through that outlives every body: `main` builds them once, and a
/// reform hands the same ones to the next body.
///
/// Constructed rather than aggregate-initialised: every member is a reference, so there is no
/// default state, and a constructor is what says every one is bound.
struct FormationDurables
{
    /// @param recordStore Where every change of the record is written first.
    /// @param fleetEndpoints Where the fleet's voters are remembered as seeds.
    /// @param reformRequest Raised when a move changes the node's shape; read by a body's stop loop.
    /// @param runtimeParts The time, randomness and network the formation acts through.
    FormationDurables(Cluster::IFormationStore& recordStore,
                      Cluster::FleetEndpointsFile& fleetEndpoints,
                      ReformRequest& reformRequest,
                      FormationRuntimeParts const& runtimeParts) noexcept:
        store { recordStore },
        endpoints { fleetEndpoints },
        reform { reformRequest },
        parts { runtimeParts }
    {
    }

    Cluster::IFormationStore& store;        ///< Where every change of the record is written first.
    Cluster::FleetEndpointsFile& endpoints; ///< Where the fleet's voters are remembered as seeds.
    ReformRequest& reform;                  ///< Raised by a move; ends the body at its stop loop.
    FormationRuntimeParts parts;            ///< The time, randomness and network the formation acts through.
};

/// What one serving body runs its formation by.
struct FormationBody
{
    Cluster::FormationRecord record; ///< The record this body was adopted from.
    FormationDurables durables;      ///< What outlives the body.
    /// The live configuration, or null when this node has no file: what a move is judged against
    /// (`LiveNodeConfig`). Borrowed; outlives the body. Every construction names it, null included.
    NodeReloader const* reloader { nullptr };
};

/// An `IClusterAdmin` the consensus tier is attached to once it runs.
///
/// The formation controller is built before the tier -- the tier's hooks tell it the applied state
/// -- and proposes through the tier, so one of the two must be wired late. Until the tier is
/// attached, nobody leads that this node knows: a proposal is refused `NotLeader`, a refusal about
/// the MOMENT, which the controller retries at a later beat.
class LateClusterAdmin final: public Distributed::IClusterAdmin
{
  public:
    /// @param admin The running tier; must outlive this, or be detached with null first.
    void Attach(Distributed::IClusterAdmin* admin) noexcept;

    /// @copydoc Distributed::IClusterAdmin::ClusterState
    [[nodiscard]] Cluster::ClusterState ClusterState() const override;

    /// @copydoc Distributed::IClusterAdmin::ProposeToCluster
    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override;

    /// @copydoc Distributed::IClusterAdmin::NoteAnnouncedEndpoint
    ///
    /// Dropped while no tier is attached: nobody leads that this node knows, and the member announces
    /// again at its next interval.
    void NoteAnnouncedEndpoint(Consensus::NodeId const& member, std::string endpoint) override;

  private:
    std::atomic<Distributed::IClusterAdmin*> _admin { nullptr };
};

/// The join memos a node that serves no scheduler reads: none, since no member announces to it.
class NoAnnouncedJoinMemos final: public Cluster::IAnnouncedJoinMemos
{
  public:
    /// @return Nothing.
    [[nodiscard]] std::vector<Cluster::AskedJoinBy> AnnouncedJoinMemos() const override
    {
        return {};
    }
};

/// Every seed a solitary node tries, in `OrderSeeds`' order, asked afresh at each probe round.
///
/// A function of what is kept NOW: the remembered endpoints file, the typed `--fleet-seed` values,
/// and the DNS SRV record for this machine's domain -- so an answer that changed is seen at the next
/// round, and a lookup that fails costs that source and nothing else.
/// @param endpoints Where the fleet endpoints are remembered.
/// @param typed The `--fleet-seed` values.
/// @param srv What answers the SRV question.
/// @param dnsSuffix This machine's domain; empty asks no SRV question at all.
/// @return The seeds.
[[nodiscard]] std::vector<Cluster::SeedCandidate> SeedsNow(Cluster::FleetEndpointsFile& endpoints,
                                                           std::span<std::string const> typed,
                                                           ISrvResolver const& srv,
                                                           std::string_view dnsSuffix);

/// One serving body's formation, running.
///
/// The controller the node's summary, its split evidence and its proven fleets all go through, over
/// production seams -- a dialled `Enroll` channel that presents no credential, a dialled fleet probe,
/// the system SRV resolver -- and a thread that beats it. Destroyed before the body's consensus tier
/// would be a controller its hooks still call into, so a body declares this FIRST.
class FormationRuntime
{
  public:
    /// How often the controller beats: the presence loop's interval, the beat every other periodic
    /// duty of a node runs at. At most one exchange per beat (`FormationController::Tick`).
    static constexpr std::chrono::seconds TickInterval = NodeAnnounceInterval;

    /// The per-call ceiling of a poll or a probe the controller dials: the operator's enrollment
    /// verbs', since an `Enroll` is the same exchange whoever sends it. Each exchange holds its own round-trip bound
    /// on top (`DialledFleetProbe::ExchangeDeadline`, `Cc::ExchangeFramed`'s).
    static constexpr std::chrono::milliseconds IoTimeout = EnrollDialTimeout;

    /// @param cfg The configuration the record shaped; must outlive this.
    /// @param identityKey This node's identity key pair: what every `Enroll` states and is signed
    ///        with. `MakeFormationRuntime` refuses a node that holds none. Must outlive this.
    /// @param record The record this body runs by.
    /// @param durables What outlives the body.
    /// @param reloader The live configuration, or null when this node has no file; must outlive this.
    /// @param advertised Where this node's `0xFC` port answers now: what every summary and every
    ///        `Enroll` states. Must outlive this.
    /// @param raftAdvertised Where this node's Raft port answers now: what every summary states while
    ///        the mode opens the Raft port. Must outlive this.
    /// @param memos The join memos this fleet's members announced -- the scheduler's, when this node
    ///        serves one -- or null for none. Must outlive this.
    /// @param metrics Where the controller counts; must outlive this.
    /// @param logger Where every move is said; must outlive this.
    /// @param conditions Where `formation-move-refused` is answered; null when nobody reads it. Must
    ///        outlive this.
    FormationRuntime(NodeConfig const& cfg,
                     Ed25519KeyPair const& identityKey,
                     Cluster::FormationRecord record,
                     FormationDurables durables,
                     NodeReloader const* reloader,
                     Cc::IAdvertisedEndpointSource const& advertised,
                     Cc::IAdvertisedEndpointSource const& raftAdvertised,
                     Cluster::IAnnouncedJoinMemos const* memos,
                     IMetricsSink& metrics,
                     ILogger& logger,
                     NodeConditions* conditions);

    FormationRuntime(FormationRuntime const&) = delete;
    FormationRuntime& operator=(FormationRuntime const&) = delete;
    FormationRuntime(FormationRuntime&&) = delete;
    FormationRuntime& operator=(FormationRuntime&&) = delete;

    /// Stops the beat and waits for it.
    ~FormationRuntime();

    /// @return The controller: the summary source, the split evidence and the fleet observer.
    [[nodiscard]] FormationController& Controller() noexcept
    {
        return _controller;
    }

    /// @return What the consensus tier tells this formation; valid for as long as this.
    [[nodiscard]] FormationHooks Hooks();

    /// Ends the beat when it goes out of scope: declared after the consensus tier the beat proposes
    /// through, so the beat has stopped -- and the tier is detached -- before that tier is destroyed.
    class Beat
    {
      public:
        /// @param runtime What beats; null for a body that runs no formation.
        explicit Beat(FormationRuntime* runtime) noexcept:
            _runtime { runtime }
        {
        }

        Beat(Beat const&) = delete;
        Beat& operator=(Beat const&) = delete;
        Beat(Beat&&) = delete;
        Beat& operator=(Beat&&) = delete;

        ~Beat()
        {
            if (_runtime != nullptr)
                _runtime->Stop();
        }

      private:
        FormationRuntime* _runtime;
    };

    /// Propose through @p tier, and begin beating the controller, once its readers are wired.
    /// @param tier The running consensus tier; null when this node runs none.
    void Start(Distributed::IClusterAdmin* tier);

    /// Stop the beat and wait for it, then detach the tier. Idempotent.
    ///
    /// In that order, so no beat is inside the tier when it is detached, and every proposal after it
    /// is refused `NotLeader` without reaching the tier -- which may already be gone.
    void Stop() noexcept;

  private:
    NodeConfig const& _cfg;          ///< The configuration the record shaped.
    FormationDurables _durables;     ///< What outlives the body, and the seams it acts through.
    NoAnnouncedJoinMemos _noMemos;   ///< The memos read when this node serves no scheduler.
    LiveNodeConfig _live;            ///< The configuration in force, as the judge reads it.
    StartupShapeJudge _judge;        ///< What every move's record is judged by.
    LateClusterAdmin _admin;         ///< The consensus tier, once attached; detached by `Stop`.
    DialledEnrollChannel _enroll;    ///< An `Enroll` over the injected dialer.
    DialledFleetProbe _probe;        ///< A fleet probe over the injected dialer, clock and random source.
    FormationController _controller; ///< The controller every surface reads.
    std::jthread _beat;              ///< Declared last: joined first, before anything it reads is destroyed.
};

/// What a node with no identity key says as it refuses to run a formation.
inline constexpr std::string_view FormationNeedsIdentityKey =
    "this node holds no identity key, so its formation would ask to join under none -- and an approval "
    "made from what it asked would admit a zero key";

/// The formation a body runs: built when the body's configuration runs consensus -- which every mode
/// does -- and none for a node whose consensus is closed, which answers with the summary its start
/// computed (`AnsweredFleetSummary`) and moves nowhere.
///
/// **A configuration that runs consensus and holds no identity key is REFUSED, never run under a zero
/// key.** The key is what every `Enroll` states and is signed with, and a defaulted one would pass
/// every check this side makes: the window records zero, an approval made from the row admits zero,
/// and the joiner validates the roster against the zero it sent. Only the Raft session would refuse,
/// long after.
/// @param cfg The configuration the record shaped; must outlive the runtime.
/// @param identityKey This node's identity key pair, as its start resolved it; must outlive the runtime.
/// @param body What the body runs its formation by.
/// @param advertised Where this node's `0xFC` port answers now; must outlive the runtime.
/// @param raftAdvertised Where this node's Raft port answers now; must outlive the runtime.
/// @param scheduler This node's scheduler, whose service holds the join memos members announced;
///        null when it serves none. Must outlive the runtime.
/// @param metrics Where the controller counts.
/// @param logger Where every move is said.
/// @param conditions Where `formation-move-refused` is answered; null when nobody reads it.
/// @return The runtime, not yet beating; null when the configuration runs no consensus; or
///         `FormationNeedsIdentityKey` when it runs consensus and holds no identity key.
[[nodiscard]] std::expected<std::unique_ptr<FormationRuntime>, std::string> MakeFormationRuntime(
    NodeConfig const& cfg,
    std::optional<Ed25519KeyPair> const& identityKey,
    FormationBody const& body,
    Cc::IAdvertisedEndpointSource const& advertised,
    Cc::IAdvertisedEndpointSource const& raftAdvertised,
    SchedulerTier* scheduler,
    IMetricsSink& metrics,
    ILogger& logger,
    NodeConditions* conditions);

/// What this node answers about itself: the controller's summary, or @p fallback when no formation runs.
/// @param runtime The body's formation; may be null.
/// @param fallback What a node with no formation answers; never null. A pointer rather than a
///        reference so a temporary cannot be handed in and handed back.
/// @return The source every surface that describes this node reads.
[[nodiscard]] Cluster::IFleetSummarySource const& SummarySourceOf(FormationRuntime* runtime,
                                                                  Cluster::IFleetSummarySource const* fallback) noexcept;

/// The join memos this node hands its leader at every announcement; null when no formation runs.
/// @param runtime The body's formation; may be null.
/// @return The source, or null.
[[nodiscard]] Cluster::IAskedJoinsSource const* AskedJoinsOf(FormationRuntime* runtime) noexcept;

/// What consensus tells the formation; empty when no formation runs.
/// @param runtime The body's formation; may be null.
/// @return The hooks.
[[nodiscard]] FormationHooks FormationHooksOf(FormationRuntime* runtime);

/// What discovery hands the formation and asks it; both null when no formation runs.
/// @param runtime The body's formation; may be null.
/// @return The readers.
[[nodiscard]] DiscoveryFormation DiscoveryFormationOf(FormationRuntime* runtime) noexcept;

/// Begin the formation's beat over @p tier, ended by the returned object.
/// @param runtime The body's formation; may be null, which begins nothing.
/// @param tier The running consensus tier; may be null.
/// @return What ends the beat when it goes out of scope.
[[nodiscard]] std::unique_ptr<FormationRuntime::Beat> BeginFormation(FormationRuntime* runtime,
                                                                     Distributed::IClusterAdmin* tier);

} // namespace FastCache::Node

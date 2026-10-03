// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollChannel.hpp"
#include "EnrollClient.hpp"
#include "FleetProbe.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/FormationTransitions.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// @file FormationController.hpp
/// What moves a node between modes: it decides from proven fleets and the applied cluster state,
/// writes the formation record FIRST, and only then acts on it.
///
/// Every move is a row of `Cluster::FormationTransitions`; this is the one place that fires them.

/// How long a pending node keeps asking a fleet that does not answer before it gives the join up.
///
/// `EnrollTotalBound`'s figure, for its reason: an operator walking to another terminal, not one
/// going home. Giving up changes nothing anywhere -- the node never stopped serving its own cluster
/// -- so the bound costs a later ask and never a wrong state.
inline constexpr std::chrono::minutes PendingGiveUpAfter { 10 };
static_assert(PendingGiveUpAfter == EnrollTotalBound, "a pending node gives up where --enroll-from does");

/// How long a node that a person refused leaves that fleet alone before asking it again.
///
/// Long enough that a refusal is not answered by the same request a beat later -- a person said
/// no, and a list refilling under them is noise -- and short enough that a machine refused by
/// mistake needs no operator on it to try again.
inline constexpr std::chrono::hours RejectedRetryAfter { 1 };

/// How often a solitary node asks the next seed which fleet it is in, once every typed seed has been
/// asked.
///
/// An INTERVAL, never per sighting: a probe is a dialled exchange with a machine this node chose
/// from a list, and one per beacon would hand whoever beacons a way to make it dial.
inline constexpr std::chrono::seconds SeedProbeInterval { 60 };

/// How many proven fleets a solitary node holds between two beats; the oldest is dropped.
///
/// The queue holds what was proven since the last decision and is emptied by it, so this is a bound
/// on one beat's sightings, not a memory of the segment.
inline constexpr std::size_t MaxQueuedFleets = 16;

/// How many fleet ids a node names, once each, as never to be asked for their id; past it, the rest
/// go unnamed rather than growing a set a segment can fill.
inline constexpr std::size_t MaxNamedUnaskableFleets = 64;

/// Archives a cluster's Raft store out of the cluster dir's root.
class IStoreArchiver
{
  public:
    IStoreArchiver() = default;
    IStoreArchiver(IStoreArchiver const&) = delete;
    IStoreArchiver& operator=(IStoreArchiver const&) = delete;
    IStoreArchiver(IStoreArchiver&&) = delete;
    IStoreArchiver& operator=(IStoreArchiver&&) = delete;
    virtual ~IStoreArchiver() = default;

    /// Move the store of @p clusterId out of the root.
    ///
    /// Idempotent: a store already, or partly, archived under @p clusterId is finished, never
    /// refused -- a crash between the record naming it and the archive completing is resumed by
    /// asking again.
    /// @param clusterId The cluster whose store sits in the root.
    /// @return Nothing, or a sentence naming what could not be moved.
    [[nodiscard]] virtual std::expected<void, std::string> Archive(std::string_view clusterId) = 0;
};

/// Asks `main` to end the serving body and rebuild it from the formation record.
class IReformSignal
{
  public:
    IReformSignal() = default;
    IReformSignal(IReformSignal const&) = delete;
    IReformSignal& operator=(IReformSignal const&) = delete;
    IReformSignal(IReformSignal&&) = delete;
    IReformSignal& operator=(IReformSignal&&) = delete;
    virtual ~IReformSignal() = default;

    /// Ask for a reform. Called after the record that calls for it was saved, never before.
    virtual void RequestReform() = 0;
};

/// Who this node is, as every summary it announces and every `Enroll` it sends says it.
struct SelfFacts
{
    std::string nodeId;            ///< Its minted id.
    Ed25519PublicKey publicKey {}; ///< Its identity key.
    std::string nodeEndpoint;      ///< Its `0xFC` endpoint, as `AdvertisedEndpoint(cfg)` states it.
    std::string raftEndpoint;      ///< Where its Raft port answers; announced EMPTY while its listener is closed.
};

/// What a formation controller acts through. Every member outlives the controller.
struct FormationParts
{
    Cluster::IFormationStore& store;        ///< Where every change is written, before it is acted on.
    IStoreArchiver& archiver;               ///< Moves a left cluster's store out of the root.
    IEnrollChannel& enroll;                 ///< Asks a fleet to admit this node, once a beat.
    IFleetProbe& probe;                     ///< Asks a seed which fleet it is in.
    Cluster::FleetEndpointsFile& endpoints; ///< Where an approval remembers the fleet's voters as seeds.

    /// Every seed to try, in `OrderSeeds`' order; re-asked at every probe round, so a DNS answer or
    /// a remembered endpoint that changed is seen. Its `--fleet-seed` candidates are the TYPED seeds:
    /// no fleet is decided on before each was asked once, so a seed that answers slower than a
    /// beacon still wins -- read from the candidates themselves, never from a flag beside them.
    std::function<std::vector<Cluster::SeedCandidate>()> seeds;

    IReformSignal& reform;               ///< Told when a move changes the node's shape.
    core::platform::IClock const& clock; ///< What the give-up and the probe interval are measured on.
    core::platform::WallClockRef wall;   ///< What the record's instants and the rejection window read.
    ISecureRandom& random;               ///< Where a newly minted cluster id comes from.
    IMetricsSink& metrics;               ///< `FormationYields`, `FormationJoinsAbandoned`.
    ILogger& logger;                     ///< Every move, and every move that could not be written.
};

/// Moves a node between modes by `Cluster::FormationTransitions`, writing the record first.
///
/// **The record is written BEFORE the node acts on it.** A move computes the new record, saves it,
/// and only then performs its effect and asks for a reform -- so a crash between the two leaves a
/// record that describes what the node is about to be, and the next start resumes it. A save that
/// fails leaves the mode where it was and says why; the node keeps serving as it was.
///
/// **One exchange per beat at most**: `Tick` does one probe or one `Enroll` poll, outside the lock,
/// so `Current` -- which every beacon and proof reads -- never waits on a network.
///
/// Thread-safe: discovery hands fleets in and reads `Current` on its thread, the applied state
/// arrives on consensus's, a revocation verdict on the transport's, and `Tick` runs on the heartbeat
/// thread. `Tick` itself is called from one thread.
class FormationController final: public Cluster::IFleetSummarySource, public Cluster::IFleetObserver
{
  public:
    /// @param parts What it acts through.
    /// @param self Who this node is.
    /// @param record The record `main` adopted; this controller owns every later write of it.
    FormationController(FormationParts parts, SelfFacts self, Cluster::FormationRecord record);

    /// What this node says about itself: the cluster it runs, in the state its mode announces.
    /// @return The summary.
    [[nodiscard]] CompileCacheWire::FleetSummary Current() const override;

    /// Queue a proven fleet; it is decided on at the next beat.
    /// @param fleet What was proven, and how it arrived.
    void OnFleetProven(Cluster::ProvenFleet const& fleet) override;

    /// The heartbeat thread's beat: decide, probe or poll -- at most one exchange.
    void Tick();

    /// Read the state this node's consensus applied.
    ///
    /// Forgotten first (#1539's reading, `IsSelfForgotten`), then the seat this node holds, then
    /// whether the cluster recorded another machine.
    ///
    /// **Only ever the APPLIED state**, never a proposal or a leader's word: a learner becomes a
    /// voter -- and opens the scheduler its own worker proves itself to -- only once this state
    /// records it as a voter under its OWN key, which is exactly what `NodeRoster::StandingOf`
    /// answers `Voter` from. Fed anything earlier, the scheduler would open before its own record,
    /// and call its own worker `NotVoter`.
    ///
    /// **And only the state of the cluster this node is IN.** `ClusterState` names no cluster, and
    /// between a move and the reform it asks for, the consensus of the cluster being LEFT still runs
    /// and still applies -- a founder's own state records it as a voter under its own key, which would
    /// seat it as a voter of the fleet it just joined. So each state is tagged with the cluster it
    /// belongs to, a fact the consensus tier knows, and any other cluster's is ignored.
    /// @param state The applied state.
    /// @param clusterId The cluster whose consensus applied it.
    /// @param leader Who leads, when known.
    /// @param leaderNodeEndpoint Where the leader answers the `0xFC` port; empty when unknown.
    void OnClusterState(Cluster::ClusterState const& state,
                        std::string_view clusterId,
                        std::optional<Consensus::NodeId> const& leader,
                        std::string_view leaderNodeEndpoint);

    /// A signed verdict saying this node's key is revoked.
    ///
    /// Counted only from a VOTER -- of the last applied state, or of the roster the fleet handed
    /// over when no state has been applied yet: anybody else's word is not the cluster's.
    /// @param acceptor Whose signature the verdict carried.
    void OnOwnKeyRevoked(Consensus::NodeId const& acceptor);

    /// @return The mode the record says.
    [[nodiscard]] Cluster::NodeMode Mode() const;

    /// @return The record as last written.
    [[nodiscard]] Cluster::FormationRecord Record() const;

  private:
    struct TriggerContext;

    /// What firing a trigger did.
    struct FireOutcome
    {
        bool fired { false };  ///< The record was written and the mode moved.
        bool reform { false }; ///< The move asks for a reform, once the lock is released.
    };

    /// The store every move writes through: the real one, and each record it keeps is published as
    /// what this node now is -- under `_lock`, while the disk work stays outside it.
    class RecordPublisher final: public Cluster::IFormationStore
    {
      public:
        /// @param owner The controller whose record each kept record becomes.
        explicit RecordPublisher(FormationController& owner) noexcept;

        /// @copydoc Cluster::IFormationStore::Load
        [[nodiscard]] std::expected<std::optional<Cluster::FormationRecord>, ConsensusError> Load() const override;

        /// @copydoc Cluster::IFormationStore::Save
        [[nodiscard]] std::expected<void, ConsensusError> Save(Cluster::FormationRecord const& record) override;

      private:
        FormationController& _owner;
    };

    [[nodiscard]] FireOutcome Fire(Cluster::FormationTrigger trigger, TriggerContext const& context, std::string_view why);
    void Publish(Cluster::FormationRecord const& record);
    [[nodiscard]] std::expected<Cluster::FormationRecord, std::string> Move(Cluster::FormationTransition const& row,
                                                                            TriggerContext const& context,
                                                                            Cluster::FormationRecord const& from);
    [[nodiscard]] std::expected<void, std::string> ApplyToRecord(Cluster::FormationEffect effect,
                                                                 TriggerContext const& context,
                                                                 Cluster::FormationRecord& next) const;
    [[nodiscard]] JoinerIdentity Joiner() const;
    void ResetPoll();
    void Queue(Cluster::ProvenFleet const& fleet);
    void NameUnaskable(std::string const& clusterId);
    [[nodiscard]] std::uint64_t WallSeconds() const;
    [[nodiscard]] bool RecentlyRejectedBy(std::string_view clusterId) const;
    [[nodiscard]] CompileCacheWire::FleetSummary CurrentLocked() const;
    [[nodiscard]] bool IsVoter(Consensus::NodeId const& id) const;

    void TickSolitary();
    void TickPending();
    void Reform(FireOutcome outcome);

    FormationParts _parts;
    SelfFacts _self;

    RecordPublisher _publisher; ///< Every move's saves go through it; it publishes what the store kept.

    std::mutex _effectLock; ///< Serializes moves. Taken first, and never while `_lock` is held.
    mutable std::mutex _lock;
    std::uint64_t _published { 0 }; ///< Records published so far; a move that published one moved.
    Cluster::FormationRecord _record;
    std::vector<Cluster::ProvenFleet> _seen;                      ///< Proven since the last decision, one per cluster id.
    std::vector<std::string> _typedAsked;                         ///< The typed seeds asked once already.
    std::size_t _probeCursor { 0 };                               ///< Which seed the next interval probe asks.
    core::platform::SteadyTimePoint _nextProbeAt;                 ///< When the next interval probe is due.
    std::string _leaderId;                                        ///< From the last applied state.
    std::string _leaderNodeEndpoint;                              ///< From the last applied state.
    std::vector<Consensus::NodeId> _voters;                       ///< The last applied state's voters.
    std::string _pollEndpoint;                                    ///< Where the next poll goes; a redirect moves it.
    int _redirects { 0 };                                         ///< Consecutive redirects followed.
    std::optional<core::platform::SteadyTimePoint> _failingSince; ///< When the fleet last stopped answering.
    std::string _lastSaid;                                        ///< The last reading told to the log.
    std::set<std::string> _namedUnaskable;                        ///< Fleet ids already named as never asked.
};

} // namespace FastCache::Node

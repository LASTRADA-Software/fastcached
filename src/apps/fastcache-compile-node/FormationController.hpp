// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollChannel.hpp"
#include "EnrollClient.hpp"
#include "FleetProbe.hpp"
#include "NodeConditions.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/FormationTransitions.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Cluster/SplitEvidence.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
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
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>
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
/// Ten minutes: an operator walking to another terminal, not one going home. Giving up changes
/// nothing anywhere -- the node never stopped serving its own cluster, and its row stays on the
/// leader's list until the window closes -- so the bound costs a later ask and never a wrong state.
inline constexpr std::chrono::minutes PendingGiveUpAfter { 10 };

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

/// How many of this fleet's machines seen speaking for another proven fleet a controller remembers,
/// newest first: the ids its summary lists first (`Cluster::SummaryMembers`). More than a datagram
/// carries, so the order past the cut is still the right one for a reply.
inline constexpr std::size_t MaxSpokeElsewhere = 64;

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

/// Whether other machines can be this node's fleet at all.
///
/// **Private**: never transmitted or persisted.
enum class FleetReachability : std::uint8_t
{
    Open,             ///< Its consensus address leaves this machine: it may ask a fleet, and be asked.
    ThisMachineAlone, ///< Its consensus is confined to this machine (`ConsensusConfinedToThisMachine`).
};

/// Who this node is, as every summary it announces and every `Enroll` it sends says it.
struct SelfFacts
{
    std::string nodeId;            ///< Its minted id.
    Ed25519PublicKey publicKey {}; ///< Its identity key.

    /// Where its `0xFC` port answers, read at every summary and every `Enroll` -- never captured, so
    /// an accepted reload of `--advertise` is what the next poll states and the leader records. The
    /// one object the worker registers under (`AnnouncedEndpoint`). Must outlive the controller.
    Cc::IAdvertisedEndpointSource const& advertised;

    /// Where its Raft port answers, read at every summary -- never captured, so the beacon states where
    /// a node that roamed answers NOW, and a peer's discovery desires it there (the resolver publishes
    /// it, `EndpointResolver`). Announced EMPTY while its listener is closed. Must outlive the controller.
    Cc::IAdvertisedEndpointSource const& raftAdvertised;

    /// Whether it may ask a fleet to take it. A node confined to this machine asks no seed and polls
    /// no fleet -- a member admitted at a loopback address is one nobody else can reach -- and so
    /// decides nothing on a beat, a pending one included: it stays its own cluster, serving its own
    /// scheduler, until a restart with a name other machines resolve resumes the join.
    FleetReachability reach { FleetReachability::Open };

    /// The one cluster `--fleet-id` lets it belong to, or none: asked of every yield it decides, every
    /// admission it takes and every dissolve it follows (`Cluster::AdmitsFleet`). Fixed for the life of
    /// the process -- the flag is `Reloadable::No` -- so a value, never a seam.
    Cluster::FleetPin pin;
};

/// Whether the startup rules refuse the shape a record would give this node: what EVERY formation
/// transition asks of the record it is about to write, before writing it.
///
/// A seam rather than a call, so the controller judges a move by the rules `main` judges a start by
/// without owning the configuration they read (`StartupShapeJudge`).
class IShapeJudge
{
  public:
    IShapeJudge() = default;
    IShapeJudge(IShapeJudge const&) = delete;
    IShapeJudge(IShapeJudge&&) = delete;
    IShapeJudge& operator=(IShapeJudge const&) = delete;
    IShapeJudge& operator=(IShapeJudge&&) = delete;
    virtual ~IShapeJudge() = default;

    /// @param next The record a move would write.
    /// @return The startup rule's refusal of the shape it gives this node, or nothing when it may serve it.
    [[nodiscard]] virtual std::optional<std::string> RefusalOf(Cluster::FormationRecord const& next) const = 0;
};

/// What a formation controller acts through. Every member outlives the controller.
struct FormationParts
{
    Cluster::IFormationStore& store;        ///< Where every change is written, before it is acted on.
    IEnrollChannel& enroll;                 ///< Asks a fleet to admit this node, once a beat.
    IFleetProbe& probe;                     ///< Asks a seed which fleet it is in.
    Cluster::FleetEndpointsFile& endpoints; ///< Where an approval remembers the fleet's voters as seeds.

    /// The join memos this fleet's members announced -- the leader's `SchedulerService`, where
    /// every member announces -- read as the announced half of the split evidence.
    Cluster::IAnnouncedJoinMemos const& announced;

    /// Where this node, while it leads its fleet, proposes the fleet's dissolve into the survivor of a
    /// split it verified: the consensus tier, which replicates it to every member.
    Distributed::IClusterAdmin& admin;

    /// Every seed to try, in `OrderSeeds`' order; re-asked at every probe round, so a DNS answer or
    /// a remembered endpoint that changed is seen. Its `--fleet-seed` candidates are the TYPED seeds:
    /// no fleet is decided on before each was asked once, so a seed that answers slower than a
    /// beacon still wins -- read from the candidates themselves, never from a flag beside them.
    std::function<std::vector<Cluster::SeedCandidate>()> seeds;

    IReformSignal& reform;               ///< Told when a move changes the node's shape.
    core::platform::IClock const& clock; ///< What the give-up and the probe interval are measured on.
    core::platform::WallClockRef wall;   ///< What the record's instants and the rejection window read.
    ISecureRandom& random;               ///< Where a newly minted cluster id, and each poll's nonce, come from.
    IMetricsSink& metrics;               ///< `FormationYields`, `FormationJoinsAbandoned`, the admission and pin refusals.
    ILogger& logger;                     ///< Every move, and every move that could not be written.

    /// Asked of every record a move is about to write: a move whose shape the startup rules refuse is
    /// not taken, whether or not it reforms.
    IShapeJudge const& judge;

    /// Where `formation-move-refused` is answered; null when nobody reads it.
    NodeConditions* conditions;
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
class FormationController final:
    public Cluster::IFleetSummarySource,
    public Cluster::IFleetObserver,
    public Cluster::IAskedJoinsSource,
    public Cluster::ISplitEvidenceSource
{
  public:
    /// @param parts What it acts through.
    /// @param self Who this node is.
    /// @param record The record `main` adopted; this controller owns every later write of it.
    FormationController(FormationParts parts, SelfFacts self, Cluster::FormationRecord record);

    /// What this node says about itself: the cluster it runs, in the state its mode announces.
    /// @return The summary.
    [[nodiscard]] CompileCacheWire::FleetSummary Current() const override;

    /// Queue a proven fleet of ANOTHER cluster; it is decided on at the next beat. This node's own fleet
    /// is dropped here by cluster id, never left to whatever observer stands in front.
    /// @param fleet What was proven, and how it arrived.
    void OnFleetProven(Cluster::ProvenFleet const& fleet) override;

    /// The fleets this node once asked, as its record keeps them now: what every announcement
    /// hands the leader.
    /// @return The memos, oldest first.
    [[nodiscard]] std::vector<Cluster::AskedJoin> AskedJoins() const override;

    /// What this node can say about @p seen: `Cluster::ReadSplit` over the state its own cluster last
    /// applied, its record and what members announced. No evidence before a state was applied.
    /// @param seen A proven fleet of another cluster.
    /// @return The reading.
    [[nodiscard]] Cluster::SplitReading ReadSplit(Cluster::ProvenFleetSummary const& seen) const override;

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
    void AdoptLocked(Cluster::FormationRecord record);
    [[nodiscard]] std::expected<Cluster::FormationRecord, std::string> Move(Cluster::FormationTransition const& row,
                                                                            TriggerContext const& context,
                                                                            Cluster::FormationRecord const& from);
    [[nodiscard]] std::expected<void, std::string> ApplyToRecord(Cluster::FormationEffect effect,
                                                                 TriggerContext const& context,
                                                                 Cluster::FormationRecord& next) const;
    [[nodiscard]] JoinerIdentity Joiner(std::optional<CompileCacheWire::EnrollChallenge> challenge) const;
    void ResetPoll();
    void ResetChain();
    /// An endpoint and the key the chain from the key that proved the fleet proved there.
    struct ChainLink
    {
        std::string endpoint;    ///< Where it answers.
        Ed25519PublicKey key {}; ///< The key it proved.
    };
    [[nodiscard]] std::optional<EnrollReading> ProvePollEndpoint(std::string const& endpoint,
                                                                 std::string const& clusterId,
                                                                 std::optional<Ed25519PublicKey> expected,
                                                                 std::optional<ChainLink> const& anchor);
    [[nodiscard]] EnrollReading PollProven(std::string const& endpoint,
                                           Cluster::JoinTarget const& target,
                                           Ed25519PublicKey const& pollKey,
                                           std::optional<CompileCacheWire::EnrollChallenge> challenge);
    void Queue(Cluster::ProvenFleet const& fleet);
    void NameUnaskable(std::string const& clusterId);
    void NamePinWithheld(std::string const& clusterId);
    void RefuseUnpinnedOrderLocked(std::string const& clusterId);
    [[nodiscard]] std::uint64_t WallSeconds() const;
    /// Count a reading nobody signed towards giving the join up. The caller holds the lock.
    /// @return True once `PendingGiveUpAfter` has passed with no answer a proven key signed.
    [[nodiscard]] bool GiveUpDueLocked();
    [[nodiscard]] bool RecentlyRejectedBy(std::string_view clusterId) const;
    [[nodiscard]] CompileCacheWire::FleetSummary CurrentLocked() const;
    [[nodiscard]] std::optional<Ed25519PublicKey> LeaderKeyLocked() const;
    [[nodiscard]] Cluster::ClusterState const* AppliedLocked() const;
    [[nodiscard]] bool IsVoter(Consensus::NodeId const& id) const;

    /// A pending node this one follows, and what it points at.
    struct FollowedPointer
    {
        std::string pendingId;                 ///< The pending node's own cluster.
        CompileCacheWire::JoinPointer pointer; ///< The fleet it asked, as it states it: a hint.
    };
    [[nodiscard]] std::optional<FollowedPointer> PointerOf(CompileCacheWire::FleetSummary const& own,
                                                           Cluster::ProvenFleet const& fleet) const;
    void TickSolitary();
    void TickPending();
    void TickMember();
    [[nodiscard]] std::optional<Cluster::Command> ProposalLocked(CompileCacheWire::FleetSummary const& own,
                                                                 Cluster::ProvenFleetSummary const& seen,
                                                                 Cluster::SplitReading const& reading,
                                                                 std::string& why);
    void Reform(FireOutcome outcome);

    FormationParts _parts;
    SelfFacts _self;

    RecordPublisher _publisher; ///< Every move's saves go through it; it publishes what the store kept.

    std::mutex _effectLock; ///< Serializes moves. Taken first, and never while `_lock` is held.

    /// The refusal `formation-move-refused` was last raised for, said once until it changes; guarded by
    /// `_effectLock`, under which every save runs.
    std::string _shapeRefusal;

    /// Whether the move in progress was refused by the judge rather than by the store; guarded by
    /// `_effectLock`.
    bool _refusedByJudge { false };
    mutable std::mutex _lock;
    Cluster::FormationRecord _record;
    /// Proven since the last decision, one per cluster id -- and since this node entered the mode it is
    /// in: each mode decides only what it queued (`AdoptLocked`).
    std::vector<Cluster::ProvenFleet> _seen;
    std::vector<std::string> _typedAsked;         ///< The typed seeds asked once already.
    std::size_t _probeCursor { 0 };               ///< Which seed the next interval probe asks.
    core::platform::SteadyTimePoint _nextProbeAt; ///< When the next interval probe is due.
    /// The state this node's cluster last applied, under the id it was applied for; a state of a
    /// cluster this node has since left is never read as the current one (`AppliedLocked`).
    std::optional<std::pair<std::string, Cluster::ClusterState>> _applied;
    std::vector<std::string> _spokeElsewhere; ///< Members seen speaking for another fleet, newest first.
    std::string _leaderId;                    ///< From the last applied state.
    std::string _leaderNodeEndpoint;          ///< From the last applied state.
    std::vector<Consensus::NodeId> _voters;   ///< The last applied state's voters.
    std::string _pollEndpoint;                ///< Where the next poll goes; a redirect moves it.
    /// The key this node PROVED for the fleet it asked, answering at `_pollEndpoint`, which an
    /// admission from there must be signed by; none until it is proved, and the beat that proves it
    /// polls nothing.
    std::optional<Ed25519PublicKey> _pollKey;
    /// The key `_pollEndpoint` must prove, as the chain from the key that proved the fleet reached it;
    /// none after a redirect, until `_anchor` says which key leads.
    std::optional<Ed25519PublicKey> _expectedKey;
    /// Where a key the chain proved answers -- the root's own endpoint where the decided summary
    /// states it, else the first endpoint proved, so rooted at `provenKey` transitively -- asked
    /// which key leads after a redirect.
    std::optional<ChainLink> _anchor;
    int _redirects { 0 };                                         ///< Consecutive redirects followed.
    std::optional<core::platform::SteadyTimePoint> _failingSince; ///< When the fleet last stopped answering.
    std::string _lastSaid;                                        ///< The last reading told to the log.
    std::set<std::string> _namedUnaskable;                        ///< Fleet ids already named as never asked.
    std::set<std::string> _namedPinWithheld;                      ///< Fleet ids already named as withheld by the pin.
    std::string _refusedOrderFor; ///< The survivor whose dissolve order the pin last refused, said once.
    /// The advertise this node last said it cannot join under, so it is said once per advertise.
    std::optional<std::string> _unstatableSaidFor;
    /// The challenge the last VERIFIED `Pending` answer issued, beside the endpoint that issued it:
    /// sent back only to that endpoint, so the row there refreshes (`EnrollRequest::challenge`).
    /// A challenge is one node's row's, so a poll anywhere else states none.
    std::optional<std::pair<std::string, CompileCacheWire::EnrollChallenge>> _challenge;
    std::string _proposedFor;                                   ///< The survivor this leader last proposed dissolving into.
    std::optional<core::platform::SteadyTimePoint> _proposedAt; ///< When; the same proposal waits `SeedProbeInterval`.
    /// When this node may next follow a pending node's pointer: one follow per `SeedProbeInterval`,
    /// whichever endpoint it names.
    core::platform::SteadyTimePoint _nextFollowAt;
};

} // namespace FastCache::Node

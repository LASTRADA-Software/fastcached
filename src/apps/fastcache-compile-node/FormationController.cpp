// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"
#include "FormationController.hpp"
#include "FormationEffects.hpp"
#include "NodeConfig.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/SelfForgotten.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/PeerText.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;
using Cluster::FormationEffect;
using Cluster::FormationTrigger;
using Cluster::NodeMode;
using Cluster::NodeModeRowFor;

namespace
{
    /// The `0xFC` endpoint this node STATES in its summary: @p advertised when a peer could dial it,
    /// else none.
    ///
    /// None rather than a loopback or wildcard spelling, because the summary's one rule for an
    /// endpoint only the dialler reaches is to withhold the whole announcement
    /// (`Cluster::AnnouncesOnlyThisMachine`) -- right for the endpoints a peer NEEDS, wrong for this
    /// one, which only makes the node a target a peer holding its key may ask again. A node that
    /// states none is simply not one. The rule is `IsPeerDialableEndpoint`, the one every route
    /// into a member record asks.
    /// @param advertised The endpoint `AdvertisedEndpoint` names.
    /// @return The endpoint to state, or empty.
    [[nodiscard]] std::string StatedNodeEndpoint(std::string_view advertised)
    {
        return PeerDialableOrNone(advertised);
    }
} // namespace

/// What a trigger carries into the record it changes.
struct FormationController::TriggerContext
{
    /// The cluster the node was in when the trigger was DECIDED. `Fire` drops a trigger whose node has
    /// since moved to another cluster: it was decided about a record that is not this one any more.
    std::string decidedFor;
    Cluster::ProvenFleet const* fleet { nullptr };   ///< `YieldDecided`: the fleet it yields to.
    std::span<std::byte const> roster {};            ///< `Approved`: the roster the fleet handed over.
    Cluster::DissolveOrder const* order { nullptr }; ///< `DissolvedInto`: the survivor the fleet leaves for.
    /// `Approved`: the key the admission was verified under, which the record keeps for the pin to judge.
    std::optional<Ed25519PublicKey> admittedBy {};
};

FormationController::FormationController(FormationParts parts, SelfFacts self, Cluster::FormationRecord record):
    _parts { std::move(parts) },
    _self { std::move(self) },
    _publisher { *this },
    _record { std::move(record) },
    _nextProbeAt { _parts.clock.now() }
{
    ResetPoll();
    // Answered as the controller starts: no move has been refused yet, which is a checked answer.
    if (_parts.conditions != nullptr)
        _parts.conditions->Clear(NodeCondition::FormationMoveRefused);
}

Wire::FleetSummary FormationController::Current() const
{
    std::scoped_lock const lock { _lock };
    return CurrentLocked();
}

/// This node's summary; the caller holds the lock.
Wire::FleetSummary FormationController::CurrentLocked() const
{
    auto const& row = NodeModeRowFor(_record.mode);
    // The age of the cluster it RUNS: a member announces its fleet's, which it was told at approval,
    // and a node that founded its cluster announces its own.
    auto const createdAt =
        _record.fleet.has_value() ? _record.fleet->createdAtUnixSeconds : _record.own.createdAtUnixSeconds;
    // The machines its cluster records, in the order a carrier's cut keeps -- or, before its cluster
    // applied a state, this node alone, which is every cluster at its start.
    auto const* const applied = AppliedLocked();
    auto members =
        applied != nullptr ? Cluster::SummaryMembers(*applied, _spokeElsewhere) : std::vector<std::string> { _self.nodeId };
    auto const total = members.size();
    auto summary = Wire::FleetSummary {
        .clusterId = Cluster::CurrentClusterId(_record),
        .state = row.announces,
        .createdAtUnixSeconds = createdAt,
        .leaderId = _leaderId,
        .leaderNodeEndpoint = _leaderNodeEndpoint,
        .nodeId = _self.nodeId,
        .raftEndpoint =
            row.raftListener == Cluster::RaftListenerState::Open ? _self.raftAdvertised.Current() : std::string {},
        .members = std::move(members),
        .memberTotal = total,
        .nodeEndpoint = StatedNodeEndpoint(_self.advertised.Current()),
        .leaderKey = LeaderKeyLocked(),
        .pointsAt = {},
    };
    // A pending node is a POINTER (`Wire::FleetStateTable`): its leader slots name the fleet it
    // asked, so a node meeting it asks that fleet rather than joining a cluster about to be left.
    // With no target recorded it points nowhere, which nobody follows.
    if (Wire::LeaderSlotsNameAskedFleet(summary.state))
    {
        summary.leaderId.clear();
        summary.leaderNodeEndpoint.clear();
        summary.leaderKey.reset();
        if (_record.joining.has_value())
            summary.pointsAt = Wire::JoinPointer { .leaderId = _record.joining->summary.leaderId,
                                                   .leaderNodeEndpoint = _record.joining->summary.leaderNodeEndpoint,
                                                   .leaderKey = _record.joining->summary.leaderKey };
    }
    return summary;
}

/// The identity key of the leader this node knows, as a summary states it: its own when it leads,
/// else the one its cluster's applied state records for that id. The caller holds the lock.
/// @return The key, or nothing when no leader, or no key for it, is known.
std::optional<Ed25519PublicKey> FormationController::LeaderKeyLocked() const
{
    if (_leaderId.empty())
        return std::nullopt;
    if (_leaderId == _self.nodeId)
        return _self.publicKey;
    auto const* const applied = AppliedLocked();
    if (applied == nullptr)
        return std::nullopt;
    auto const* const leader = core::findOrNull(applied->members, _leaderId, &Cluster::ClusterMember::id);
    return leader != nullptr ? std::optional { leader->publicKey } : std::nullopt;
}

Cluster::ClusterState const* FormationController::AppliedLocked() const
{
    if (!_applied.has_value() || _applied->first != Cluster::CurrentClusterId(_record))
        return nullptr;
    return &_applied->second;
}

void FormationController::OnFleetProven(Cluster::ProvenFleet const& fleet)
{
    std::scoped_lock const lock { _lock };
    // This node's OWN fleet is dropped here, by cluster id, whatever stands in front of this observer:
    // discovery hands every authenticated reply on, and a member of this fleet speaking for it is not
    // one speaking for ANOTHER -- listed among those it would be evidence of a split that is not there.
    if (fleet.Summary().clusterId == Cluster::CurrentClusterId(_record))
        return;
    Queue(fleet);

    // A machine of this fleet speaking for another is one that fleet may have admitted: listed first
    // in this node's summary, so the other side's evidence survives a datagram's cut.
    auto const* const applied = AppliedLocked();
    if (applied == nullptr || !Cluster::SpeakerIsOurMember(*applied, fleet.Proven()))
        return;
    auto const& speaker = fleet.Summary().nodeId;
    std::erase(_spokeElsewhere, speaker);
    _spokeElsewhere.insert(_spokeElsewhere.begin(), speaker);
    if (_spokeElsewhere.size() > MaxSpokeElsewhere)
        _spokeElsewhere.pop_back();
}

/// Hold @p fleet for the next decision: one entry per cluster id, the newest proof kept, and the
/// oldest dropped past `MaxQueuedFleets`. The caller holds the lock.
void FormationController::Queue(Cluster::ProvenFleet const& fleet)
{
    std::erase_if(
        _seen, [&fleet](Cluster::ProvenFleet const& held) { return held.Summary().clusterId == fleet.Summary().clusterId; });
    _seen.push_back(fleet);
    if (_seen.size() > MaxQueuedFleets)
        _seen.erase(_seen.begin());
}

void FormationController::Tick()
{
    // Confined to this machine, nothing is decided at all: every arm below asks another machine
    // something, a seed, a fleet or a member, and none of them could reach this node back.
    if (_self.reach == FleetReachability::ThisMachineAlone)
        return;

    // A closed set whose arms are different work, the `EnrollRoleTable` precedent: only the two
    // modes that are still their own cluster decide anything on a beat.
    switch (Mode())
    {
        case NodeMode::Solitary:
            TickSolitary();
            return;
        case NodeMode::Pending:
            TickPending();
            return;
        case NodeMode::Voter:
            TickMember();
            return;
        case NodeMode::Learner:
            return;
    }
}

/// Where @p fleet points, when it is a pending node this one follows (`Cluster::Encounter::Follow`).
/// Spends nothing: whether a pointer is followed on this beat is the beat's decision, made once it
/// knows nothing better is due.
/// @param own This node's summary.
/// @param fleet A proven fleet.
/// @return The pending fleet's id and what it points at, or nothing.
std::optional<FormationController::FollowedPointer> FormationController::PointerOf(Wire::FleetSummary const& own,
                                                                                   Cluster::ProvenFleet const& fleet) const
{
    auto const& summary = fleet.Summary();
    auto const& pointer = summary.pointsAt;
    // A pointer back at this node is this node's own fleet, which it has nothing to ask about.
    if (Cluster::ClassifyEncounter(own, fleet.Proven(), Cluster::SplitEvidence::None, _self.pin)
            != Cluster::Encounter::Follow
        || !ParseDialEndpoint(pointer.leaderNodeEndpoint).has_value() || pointer.leaderId == _self.nodeId)
        return std::nullopt;
    return FollowedPointer { .pendingId = summary.clusterId, .pointer = pointer };
}

/// A solitary beat: ask a typed seed not yet asked; else decide over what was proven; else, when one
/// is due, ask the next seed; else follow a pending node to the fleet it asked, at most once per
/// `SeedProbeInterval`.
///
/// **A pointer is followed on a beacon's word, so following one is bounded for the whole node, never
/// per endpoint.** Every pending beacon names an endpoint of its sender's choosing: bounded per
/// endpoint, one beacon a beat pointing somewhere new would have this node dial a chosen host:port
/// every beat, and -- were a pointer to pre-empt the probe -- never ask its own seeds again. So the
/// seed probe keeps its turn, and a pointer spends the one follow an interval allows only on a beat
/// that has nothing better to do.
void FormationController::TickSolitary()
{
    auto const seeds = _parts.seeds ? _parts.seeds() : std::vector<Cluster::SeedCandidate> {};

    auto unaskedTyped = std::optional<Cluster::SeedCandidate> {};
    {
        std::scoped_lock const lock { _lock };
        for (auto const& seed: seeds)
            if (seed.source == Cluster::SeedSource::FleetSeedFlag && !std::ranges::contains(_typedAsked, seed.endpoint))
            {
                unaskedTyped = seed;
                break;
            }
    }

    // A typed seed first, one per beat, and nothing decided on the beat that asks one: the seed an
    // administrator typed must be in the queue before any beacon's fleet can be chosen over it.
    if (unaskedTyped.has_value())
    {
        auto answer = _parts.probe.Ask(*unaskedTyped);
        std::scoped_lock const lock { _lock };
        _typedAsked.push_back(unaskedTyped->endpoint);
        if (answer.has_value())
            Queue(*answer);
        else
            _parts.logger.Logf(LogLevel::Info,
                               "formation: the typed seed {} answered nothing usable: {}",
                               unaskedTyped->endpoint,
                               answer.error());
        return;
    }

    auto target = std::optional<Cluster::ProvenFleet> {};
    auto pointer = std::optional<FollowedPointer> {};
    auto probeDue = std::optional<Cluster::SeedCandidate> {};
    auto decidedFor = std::string {};
    {
        std::scoped_lock const lock { _lock };
        if (_record.mode != NodeMode::Solitary)
            return;
        decidedFor = Cluster::CurrentClusterId(_record);
        auto const own = CurrentLocked();

        auto eligible = std::vector<Cluster::ProvenFleet> {};
        // A fleet is asked only if it can be: it has not just refused this node, it names where it
        // answers, and its id could name the archive of its store should it ever forget this node --
        // the dissolve refuses one that could not, so asking it would be a join that cannot finish.
        auto candidate = std::optional<FollowedPointer> {};
        for (auto const& fleet: _seen)
        {
            if (!candidate.has_value())
                candidate = PointerOf(own, fleet);
            if (!IsArchivableClusterId(fleet.Summary().clusterId))
            {
                NameUnaskable(fleet.Summary().clusterId);
                continue;
            }
            if (!RecentlyRejectedBy(fleet.Summary().clusterId)
                && ParseDialEndpoint(fleet.Summary().leaderNodeEndpoint).has_value())
                eligible.push_back(fleet);
        }
        _seen.clear();

        // What the pin withholds is told and counted per proof, before the decision: a node that sits
        // alone beside a fleet it would have joined must say why, and the attack the pin stops -- a
        // fleet proving itself older to be joined -- reads as exactly this.
        for (auto const& fleet: eligible)
            if (Cluster::ClassifyEncounter(own, fleet.Proven(), Cluster::SplitEvidence::None, _self.pin)
                == Cluster::Encounter::PinnedElsewhere)
            {
                _parts.metrics.Increment(IMetricsSink::Counter::FormationYieldsRefusedPin);
                NamePinWithheld(fleet.Summary().clusterId);
            }
        target = Cluster::PreferredTarget(own, eligible, _self.pin);
        // A decision spends neither turn: the pointer is not followed, and the probe stays due.
        auto const now = _parts.clock.now();
        auto const probeIsDue = !target.has_value() && !seeds.empty() && now >= _nextProbeAt;
        if (probeIsDue)
        {
            probeDue = seeds[_probeCursor % seeds.size()];
            ++_probeCursor;
            _nextProbeAt = now + SeedProbeInterval;
        }
        else if (!target.has_value() && candidate.has_value() && now >= _nextFollowAt)
        {
            pointer = std::move(candidate);
            _nextFollowAt = now + SeedProbeInterval;
        }
    }

    // **A join this node could not finish is not decided.** A learner states where its `0xFC` port
    // answers and the fleet records it, so a node advertising none a peer can dial would yield, send
    // nothing (`PollProven`), give the join up after `PendingGiveUpAfter` and yield again -- its record
    // rewritten twice a cycle, and `Pending` announced to the segment meanwhile. Said once per
    // advertise; the next beat that reads a dialable one decides as usual.
    if (target.has_value())
    {
        auto const joiner = Joiner(std::nullopt);
        auto const statable = !EnrollRoleRowFor(joiner.role).statesEndpoint || !joiner.nodeEndpoint.empty();
        auto const advertised = _self.advertised.Current();
        std::scoped_lock const lock { _lock };
        if (statable)
            _unstatableSaidFor.reset();
        else
        {
            if (_unstatableSaidFor != advertised)
            {
                _unstatableSaidFor = advertised;
                _parts.logger.Logf(LogLevel::Warn,
                                   "formation: not asking {} to admit this node: it advertises no 0xFC endpoint another "
                                   "machine can dial ({}), and a fleet records where each member answers -- set "
                                   "--advertise to one that is not loopback, localhost or a wildcard",
                                   target->Summary().clusterId,
                                   advertised.empty() ? std::string { "none" } : advertised);
            }
            return;
        }
    }

    if (target.has_value())
    {
        auto const why = std::format("asking {} at {}", target->Summary().clusterId, target->Summary().leaderNodeEndpoint);
        auto const outcome = Fire(FormationTrigger::YieldDecided,
                                  TriggerContext { .decidedFor = decidedFor, .fleet = &*target, .roster = {} },
                                  why);
        if (outcome.fired)
        {
            _parts.metrics.Increment(IMetricsSink::Counter::FormationYields);
            std::scoped_lock const lock { _lock };
            ResetPoll();
        }
        Reform(outcome);
        return;
    }

    if (pointer.has_value())
    {
        auto const& pendingId = pointer->pendingId;
        auto const& endpoint = pointer->pointer.leaderNodeEndpoint;
        auto answer = _parts.probe.AskSummary(endpoint);
        std::scoped_lock const lock { _lock };
        // What answers there is decided on as its own beacon would be -- its own proof, over this
        // node's question -- and a pointer is followed ONE step: an answer that is pending itself
        // points again, and is not followed from here. The pointer is a HINT: a key it states is one
        // the answer must be signed by, never one taken as proven, and a key it does not state
        // leaves the answer to be judged as any beacon is. What is followed carries a beacon's
        // origin, the least preferred -- the demotion `ProvenFleet` leaves open -- since nothing
        // typed named it.
        if (!answer.has_value())
            _parts.logger.Logf(LogLevel::Info,
                               "formation: {} is on its way to the fleet at {}, which proved nothing: {}",
                               pendingId,
                               endpoint,
                               answer.error());
        else if (pointer->pointer.leaderKey.has_value() && answer->Key() != *pointer->pointer.leaderKey)
            _parts.logger.Logf(LogLevel::Info,
                               "formation: {} is on its way to the fleet at {}, but that endpoint proves key {}, not "
                               "the {} it named; not followed",
                               pendingId,
                               endpoint,
                               FormatEd25519PublicKey(answer->Key()),
                               FormatEd25519PublicKey(*pointer->pointer.leaderKey));
        else if (answer->Summary().state == Wire::FleetState::Pending)
            _parts.logger.Logf(LogLevel::Info,
                               "formation: {} is on its way to the fleet at {}, which is on its way elsewhere too; a "
                               "pointer is followed one step",
                               pendingId,
                               endpoint);
        else
            Queue(Cluster::ProvenFleet::FromBeaconProof(std::move(*answer)));
        return;
    }

    if (!probeDue.has_value())
        return;
    auto answer = _parts.probe.Ask(*probeDue);
    std::scoped_lock const lock { _lock };
    if (answer.has_value())
        Queue(*answer);
    else
        _parts.logger.Logf(
            LogLevel::Debug, "formation: the seed {} answered nothing usable: {}", probeDue->endpoint, answer.error());
}

/// The dissolve this leader proposes for @p seen, read as @p reading, or nothing: only a split proven
/// by evidence that heals AUTOMATICALLY (`Cluster::SplitHealing`) that this side LOSES, whose survivor
/// names a leader every member could dial and a key for that leader the proof reaches, and not the one
/// proposed less than `SeedProbeInterval` ago. The caller holds the lock.
/// @param own This node's summary.
/// @param seen The fleet, as proven.
/// @param reading What this node can say about it.
/// @param why Set to the sentence the proposal is logged with.
/// @return The command, or nothing.
std::optional<Cluster::Command> FormationController::ProposalLocked(Wire::FleetSummary const& own,
                                                                    Cluster::ProvenFleetSummary const& seen,
                                                                    Cluster::SplitReading const& reading,
                                                                    std::string& why)
{
    // Two guards, and the first is NOT dead though no case can show it red alone. `ClassifyEncounter`
    // picks its split column through the same `HealingOf`, so today the encounter table already says
    // Stay wherever this line would refuse; it is reachable only when that table is wrong. It stays
    // because it is C-1's invariant stated at the ACT -- a voter proposes a dissolve only on evidence
    // a voter's key verifies -- and a guard folded into the operation outlives a table edit that makes
    // a non-split row yield. Its proof is the JOINT neuter `fleet-yields-unguarded` (both guards gone:
    // the fleet cases go red); with this line alone removed, everything stays green by construction.
    if (Cluster::HealingOf(reading.evidence) != Cluster::SplitHealing::Automatically
        || Cluster::ClassifyEncounter(own, seen, reading.evidence, _self.pin) != Cluster::Encounter::Yield)
        return std::nullopt;
    auto const& summary = seen.Summary();
    // Every member dials it next, so a survivor naming no leader it could reach is not followed --
    // and every member holds that leader's answers to one key, reached from the key that proved the
    // survivor: the speaker's own when it speaks as the leader, else the key its signed summary states
    // for the leader. A survivor stating neither names a leader nobody could hold to anything.
    auto const speakerLeads = !summary.nodeId.empty() && summary.leaderId == summary.nodeId;
    auto const leaderKey = speakerLeads ? std::optional { seen.Key() } : summary.leaderKey;
    if (!ParseDialEndpoint(summary.leaderNodeEndpoint).has_value() || !leaderKey.has_value())
        return std::nullopt;
    auto const now = _parts.clock.now();
    if (_proposedFor == summary.clusterId && _proposedAt.has_value() && now - *_proposedAt < SeedProbeInterval)
        return std::nullopt;
    _proposedFor = summary.clusterId;
    _proposedAt = now;
    why = std::format("{} is this fleet split in two ({}), and {}",
                      summary.clusterId,
                      Cluster::SplitEvidenceNames[static_cast<std::size_t>(reading.evidence)].name,
                      Cluster::TieBreakReason(own, summary));
    return Cluster::Command { .kind = Cluster::CommandKind::DissolveInto,
                              .key = summary.clusterId,
                              .value = summary.leaderNodeEndpoint,
                              .schedulerEndpoint = {},
                              .publicKey = seen.Key(),
                              .createdAtUnixSeconds = summary.createdAtUnixSeconds,
                              .leaderKey = leaderKey };
}

/// A voter's beat: while it LEADS, whether a fleet it proved is this one split in two, on evidence
/// that heals automatically, and this side loses the heal -- and if so, one proposal of the fleet's
/// dissolve, repeated at most once per `SeedProbeInterval` until the order is applied.
///
/// Only the leader, and only on evidence a VOTER's key verifies (`Cluster::ReadSplit`,
/// `Cluster::SplitHealing`): a foreign fleet is never yielded to, whatever it lists, and a split only
/// an operator heals -- a memo's, or a learner's -- is told as `fleet-split-healing` and moves nothing.
/// The members follow the ORDER, not evidence of their own, so every one of them leaves on the same
/// decision.
void FormationController::TickMember()
{
    // Read before the lock: what members announced is another component's, with a lock of its own.
    auto const announced = _parts.announced.AnnouncedJoinMemos();
    auto proposal = std::optional<Cluster::Command> {};
    auto why = std::string {};
    {
        std::scoped_lock const lock { _lock };
        auto const seen = std::exchange(_seen, {});
        auto const* const applied = AppliedLocked();
        if (_record.mode != NodeMode::Voter || applied == nullptr || _leaderId != _self.nodeId
            || applied->dissolveOrder.has_value())
            return;
        auto const own = CurrentLocked();
        for (auto const& fleet: seen)
        {
            proposal = ProposalLocked(
                own, fleet.Proven(), Cluster::ReadSplit(*applied, _self.nodeId, _record, announced, fleet.Proven()), why);
            if (proposal.has_value())
                break;
        }
    }

    if (!proposal.has_value())
        return;
    if (auto proposed = _parts.admin.ProposeToCluster(*proposal); !proposed.has_value())
        _parts.logger.Logf(LogLevel::Warn,
                           "formation: cannot propose dissolving this fleet into {}: {}",
                           proposal->key,
                           proposed.error().context);
    else
        _parts.logger.Logf(LogLevel::Info, "formation: proposed dissolving this fleet into {}: {}", proposal->key, why);
}

/// A pending beat: one poll of the fleet it asked, and what the answer moves.
void FormationController::TickPending()
{
    auto endpoint = std::string {};
    auto target = Cluster::JoinTarget {};
    auto pollKey = std::optional<Ed25519PublicKey> {};
    auto expectedKey = std::optional<Ed25519PublicKey> {};
    auto anchor = std::optional<ChainLink> {};
    auto challenge = std::optional<Wire::EnrollChallenge> {};
    {
        std::scoped_lock const lock { _lock };
        if (_record.mode != NodeMode::Pending || !_record.joining.has_value())
            return;
        endpoint = _pollEndpoint;
        target = *_record.joining;
        pollKey = _pollKey;
        expectedKey = _expectedKey;
        anchor = _anchor;
        if (_challenge.has_value() && _challenge->first == endpoint)
            challenge = _challenge->second;
    }
    auto const& clusterId = target.summary.clusterId;

    // An answer is believed only when it is signed by a key this node PROVED for the fleet, at the
    // endpoint it asks, through the chain from the key that proved the fleet: so an endpoint whose key
    // it has not proved -- a leader the summary it decided on only named, or one a redirect named -- is
    // proved first, on a beat of its own, and polled on the next.
    auto reading = EnrollReading {};
    auto const proving = !pollKey.has_value();
    if (proving)
    {
        auto failed = ProvePollEndpoint(endpoint, clusterId, expectedKey, anchor);
        if (!failed.has_value())
            return;
        reading = *std::move(failed);
    }
    else
        reading = PollProven(endpoint, target, *pollKey, challenge);

    // What the answer asks for is decided under the lock; the move it asks for is made outside it.
    auto trigger = std::optional<FormationTrigger> {};
    auto why = std::string {};
    auto decidedFor = std::string {};
    {
        std::scoped_lock const lock { _lock };
        // The applied state may have moved the node while the poll was out -- its own cluster
        // recorded another machine -- and an answer about a join it no longer holds decides nothing.
        if (_record.mode != NodeMode::Pending || !_record.joining.has_value()
            || _record.joining->summary.clusterId != clusterId)
            return;
        decidedFor = Cluster::CurrentClusterId(_record);

        // Only an answer a key this node proved SIGNED keeps the join alive: every other reading --
        // a redirect, a closed window, a refusal on the wire, a failure -- is signed by nobody, so
        // whoever answers the poll's connection could say it every beat, and counts towards giving
        // the join up exactly as silence does.
        // A reading is told once until it changes: a fleet saying the same thing every beat is one line.
        auto const sayOnce = [this, &reading](LogLevel level, std::string const& line) {
            if (reading.detail == _lastSaid)
                return;
            _lastSaid = reading.detail;
            _parts.logger.Logf(level, "formation: {}", line);
        };
        auto const abandon = [&] {
            trigger = FormationTrigger::Abandoned;
            why = std::format(
                "{} gave no answer it signed for {} minute(s): {}", clusterId, PendingGiveUpAfter.count(), reading.detail);
        };
        // **Every answer a pinned node acts on is one a pinned voter SIGNED** -- an admission, a refusal
        // and a not-yet alike: each moves the node, sends it away, or keeps it waiting. Asked at the act,
        // though the key polled is pinned already (the root passed `ClassifyEncounter`, every other link
        // `ProvePollEndpoint`): a record somebody else wrote reaches here too, and the answer is the
        // move. Refused as no answer at all, counted towards giving the join up.
        auto const signedAnswer = reading.progress == EnrollProgress::Admitted || reading.progress == EnrollProgress::Refused
                                  || reading.progress == EnrollProgress::Waiting;
        if (signedAnswer && pollKey.has_value() && !Cluster::AdmitsFleet(_self.pin, clusterId, *pollKey))
        {
            _parts.metrics.Increment(IMetricsSink::Counter::FormationAdmissionsRefusedPin);
            reading = EnrollReading { .progress = EnrollProgress::Fatal,
                                      .detail = std::format("{}'s answer is signed by key {}, which --fleet-id ({}) does "
                                                            "not pin; it is not taken",
                                                            clusterId,
                                                            FormatEd25519PublicKey(*pollKey),
                                                            Cluster::PinText(_self.pin)),
                                      .roster = {} };
        }
        switch (reading.progress)
        {
            case EnrollProgress::Admitted:
                trigger = FormationTrigger::Approved;
                why = std::format("{} admitted it", clusterId);
                break;
            case EnrollProgress::Refused:
                trigger = FormationTrigger::Rejected;
                why = std::format("{} refused it ({})", clusterId, reading.detail);
                break;
            case EnrollProgress::Redirect:
                if (GiveUpDueLocked())
                {
                    abandon();
                    break;
                }
                // A CHAIN bound, as `RunEnrollAdmin`'s: past it the next poll goes back to the fleet's
                // own leader endpoint rather than following two stale leaders around each other.
                if (_redirects >= MaxEnrollRedirects)
                {
                    ResetChain();
                    _parts.logger.Logf(LogLevel::Info,
                                       "formation: gave up after {} leader redirect(s); asking {} again",
                                       MaxEnrollRedirects,
                                       _pollEndpoint);
                }
                else
                {
                    // Whoever answers there is proved before it is asked: a redirect is not signed,
                    // so the endpoint it names carries no key this node proved -- and no key is
                    // expected there until the chain's anchor says which one leads.
                    ++_redirects;
                    _pollEndpoint = reading.detail;
                    _pollKey.reset();
                    _expectedKey.reset();
                }
                break;
            case EnrollProgress::Waiting:
                // The fleet answered on its own behalf, signed: the chain is broken and the fleet is
                // answering, so the join is alive.
                _redirects = 0;
                _failingSince.reset();
                // Its challenge is answered by the next poll of the same endpoint, which is what lets
                // that poll refresh the row -- the endpoint this node states there above all. Only a
                // VERIFIED answer reaches this arm, so the challenge is the fleet's own.
                if (reading.challenge.has_value())
                    _challenge.emplace(endpoint, *reading.challenge);
                else
                    _challenge.reset();
                sayOnce(LogLevel::Info, std::format("{} ({}); asking again", reading.detail, clusterId));
                break;
            case EnrollProgress::Closed:
                // A node answered on its own behalf, and the chain is broken -- but a closed window is
                // the wire's word, and waits out the same bound silence does.
                _redirects = 0;
                if (GiveUpDueLocked())
                {
                    abandon();
                    break;
                }
                sayOnce(LogLevel::Info, std::format("{} ({}); asking again", reading.detail, clusterId));
                break;
            case EnrollProgress::Fatal:
                if (GiveUpDueLocked())
                {
                    abandon();
                    break;
                }
                // A redirect target that proves nothing is never asked, and is not asked to prove
                // itself again either: one injected redirect, or a leader briefly unreachable, would
                // otherwise cost the whole join. The next poll goes back to the endpoint the join was
                // decided on, whose key is the chain's root.
                if (proving && _redirects > 0)
                {
                    ResetChain();
                    _parts.logger.Logf(LogLevel::Info, "formation: {}; asking {} again", reading.detail, _pollEndpoint);
                    break;
                }
                sayOnce(LogLevel::Warn, std::format("asking {} failed: {}", clusterId, reading.detail));
                break;
        }
    }
    if (!trigger.has_value())
        return;

    auto const outcome = Fire(
        *trigger,
        TriggerContext {
            .decidedFor = decidedFor, .fleet = nullptr, .roster = reading.roster, .order = nullptr, .admittedBy = pollKey },
        why);
    if (outcome.fired && *trigger == FormationTrigger::Abandoned)
        _parts.metrics.Increment(IMetricsSink::Counter::FormationJoinsAbandoned);
    Reform(outcome);
}

void FormationController::OnClusterState(Cluster::ClusterState const& state,
                                         std::string_view clusterId,
                                         std::optional<Consensus::NodeId> const& leader,
                                         std::string_view leaderNodeEndpoint)
{
    // What the state asks for is decided under the lock; each move it asks for is made outside it,
    // in order, and each re-reads the mode the one before it left.
    auto triggers = std::vector<std::pair<FormationTrigger, std::string_view>> {};
    auto order = std::optional<Cluster::DissolveOrder> {};
    {
        std::scoped_lock const lock { _lock };
        if (clusterId != Cluster::CurrentClusterId(_record))
        {
            _parts.logger.Logf(LogLevel::Debug,
                               "formation: ignoring a state of cluster {}: this node is in {} now",
                               clusterId,
                               Cluster::CurrentClusterId(_record));
            return;
        }
        _applied.emplace(std::string { clusterId }, state);
        _leaderId = leader.value_or(Consensus::NodeId {});
        _leaderNodeEndpoint = std::string { leaderNodeEndpoint };
        _voters.clear();
        for (auto const& member: state.members)
            if (member.seat == Cluster::MemberSeat::Voter)
                _voters.push_back(member.id);

        // Forgotten first: a state that forgot this node says nothing more about it, and whatever
        // else it holds is about a cluster this node is leaving.
        if (Cluster::IsSelfForgotten(state, _self.nodeId))
            triggers.emplace_back(FormationTrigger::SelfForgotten, "the cluster forgot this node");
        // Then a dissolve: the whole fleet is leaving, so nothing else it says about a seat matters.
        // Never into the cluster this node is in, which no leader proposes and no member follows.
        // **A pinned node follows no dissolve, and the ID decides it**: the start check holds it in the
        // cluster it is pinned to, and a dissolve leaves for ANOTHER one, which the pin never names. The
        // order's keys are fields of the replicated state -- claimed, never proven to this node -- so no
        // signer is offered: the one predicate, asked with none, refuses whatever is pinned and admits
        // whatever is not. The rest of the fleet leaves, and this node stays where its operator put it,
        // saying so.
        else if (state.dissolveOrder.has_value() && state.dissolveOrder->clusterId != clusterId
                 && Cluster::AdmitsFleet(_self.pin, state.dissolveOrder->clusterId, std::span<Ed25519PublicKey const> {}))
        {
            order = state.dissolveOrder;
            triggers.emplace_back(FormationTrigger::DissolvedInto, "its fleet dissolved into the survivor of a split");
        }
        else
        {
            if (state.dissolveOrder.has_value() && state.dissolveOrder->clusterId != clusterId)
                RefuseUnpinnedOrderLocked(state.dissolveOrder->clusterId);
            for (auto const& member: state.members)
            {
                if (member.id != _self.nodeId)
                    continue;
                if (member.seat != Cluster::MemberSeat::Voter)
                {
                    triggers.emplace_back(FormationTrigger::SeatedLearner, "the cluster seated it");
                    continue;
                }
                // **A voter's seat opens this node's scheduler, and its own worker proves itself to
                // it** -- which `NodeRoster::StandingOf` answers from THIS state, by this id's recorded
                // KEY. So the seat moves the mode only once the state this node APPLIED records it as a
                // voter under its own key: a seat with no key, or another machine's, would open a
                // scheduler that calls its own worker `NotVoter`. Closing one (the learner arm above)
                // cannot mislead anybody, so it asks no key.
                if (member.publicKey != _self.publicKey)
                {
                    _parts.logger.Logf(LogLevel::Warn,
                                       "formation: the cluster seats {} as a voter without this machine's key; staying "
                                       "{} until it records the key",
                                       _self.nodeId,
                                       NodeModeRowFor(_record.mode).name);
                    continue;
                }
                triggers.emplace_back(FormationTrigger::SeatedVoter, "the cluster seated it");
            }
            if (std::ranges::any_of(state.members,
                                    [this](Cluster::ClusterMember const& member) { return member.id != _self.nodeId; }))
                triggers.emplace_back(FormationTrigger::Established, "its cluster recorded another machine");
        }
    }

    auto outcome = FireOutcome {};
    for (auto const& [trigger, why]: triggers)
    {
        auto const next = Fire(trigger,
                               TriggerContext { .decidedFor = std::string { clusterId },
                                                .fleet = nullptr,
                                                .roster = {},
                                                .order = order.has_value() ? &*order : nullptr },
                               why);
        outcome.fired = outcome.fired || next.fired;
        outcome.reform = outcome.reform || next.reform;
    }
    Reform(outcome);
}

void FormationController::OnOwnKeyRevoked(Consensus::NodeId const& acceptor)
{
    auto decidedFor = std::string {};
    {
        std::scoped_lock const lock { _lock };
        decidedFor = Cluster::CurrentClusterId(_record);
        if (!IsVoter(acceptor))
        {
            _parts.logger.Logf(LogLevel::Warn,
                               "formation: {} says this node's key is revoked, and it is no voter of this node's "
                               "cluster; ignored",
                               acceptor);
            return;
        }
    }
    Reform(Fire(FormationTrigger::SelfForgotten,
                TriggerContext { .decidedFor = decidedFor },
                std::format("the voter {} says this node's key is revoked", acceptor)));
}

/// Whether @p id is a voter of the cluster this node is in: of the last applied state, or of the
/// roster its fleet handed over while no state has been applied. The caller holds the lock.
bool FormationController::IsVoter(Consensus::NodeId const& id) const
{
    if (std::ranges::contains(_voters, id))
        return true;
    if (!_voters.empty() || !_record.fleet.has_value())
        return false;
    auto const roster = Cluster::DecodeRoster(_record.fleet->roster);
    return roster.has_value() && std::ranges::any_of(roster->members, [&id](Cluster::RosterMember const& member) {
               return member.id == id && member.seat == Cluster::MemberSeat::Voter;
           });
}

Cluster::NodeMode FormationController::Mode() const
{
    std::scoped_lock const lock { _lock };
    return _record.mode;
}

Cluster::FormationRecord FormationController::Record() const
{
    std::scoped_lock const lock { _lock };
    return _record;
}

std::vector<Cluster::AskedJoin> FormationController::AskedJoins() const
{
    std::scoped_lock const lock { _lock };
    return _record.askedJoins;
}

Cluster::SplitReading FormationController::ReadSplit(Cluster::ProvenFleetSummary const& seen) const
{
    // Read before the lock: what members announced is another component's, with a lock of its own.
    auto const announced = _parts.announced.AnnouncedJoinMemos();
    std::scoped_lock const lock { _lock };
    auto const* const applied = AppliedLocked();
    if (applied == nullptr)
        return Cluster::SplitReading {};
    return Cluster::ReadSplit(*applied, _self.nodeId, _record, announced, seen);
}

FormationController::RecordPublisher::RecordPublisher(FormationController& owner) noexcept:
    _owner { owner }
{
}

std::expected<std::optional<Cluster::FormationRecord>, ConsensusError> FormationController::RecordPublisher::Load() const
{
    return _owner._parts.store.Load();
}

/// Every move writes its record through here -- one save each -- so this is where EVERY transition is
/// judged: the record it is about to write is asked of the startup rules first, and a shape they
/// refuse is never written, whether the move reforms or not. Runs under `_effectLock`.
std::expected<void, ConsensusError> FormationController::RecordPublisher::Save(Cluster::FormationRecord const& record)
{
    auto& owner = _owner;
    if (auto const refusal = owner._parts.judge.RefusalOf(record); refusal.has_value())
    {
        owner._refusedByJudge = true;
        auto const from = owner.Mode();
        auto const detail = std::format("staying {} rather than move to {}: the startup rules refuse that shape: {}",
                                        NodeModeRowFor(from).name,
                                        NodeModeRowFor(record.mode).name,
                                        *refusal);
        if (detail != owner._shapeRefusal)
        {
            owner._shapeRefusal = detail;
            owner._parts.logger.Logf(LogLevel::Warn, "formation: {}", detail);
        }
        if (owner._parts.conditions != nullptr)
            owner._parts.conditions->Raise(NodeCondition::FormationMoveRefused, detail);
        return std::unexpected { InvalidConfiguration(detail) };
    }
    auto saved = owner._parts.store.Save(record);
    if (!saved.has_value())
        return saved;
    owner.Publish(record);
    if (!owner._shapeRefusal.empty())
    {
        owner._shapeRefusal.clear();
        if (owner._parts.conditions != nullptr)
            owner._parts.conditions->Clear(NodeCondition::FormationMoveRefused);
    }
    return saved;
}

/// Fire @p trigger: move the record by its row -- written BEFORE the node acts on it -- and adopt
/// what was written. The caller holds NO lock, and asks for the reform once this returns.
///
/// **No disk work under `_lock`**: every beacon and proof reads `Current` through it, and a slow disk
/// must not stall discovery. So the row and the record it moves from are read under the lock, the
/// move -- its save -- runs outside it, and the record the move writes is published under
/// the lock the moment the store has it (`RecordPublisher`), so `Current` says what the node is at
/// every step. Moves are serialized among themselves by `_effectLock`, taken first and never while
/// `_lock` is held.
FormationController::FireOutcome FormationController::Fire(FormationTrigger trigger,
                                                           TriggerContext const& context,
                                                           std::string_view why)
{
    std::scoped_lock const serialized { _effectLock };
    auto from = Cluster::FormationRecord {};
    Cluster::FormationTransition const* row = nullptr;
    {
        std::scoped_lock const lock { _lock };
        // Decided and fired are two moments, and a move may land between them -- the trigger waited on
        // `_effectLock` while another move took the node to another cluster. Its row is then read off
        // a record it was never about, so it is dropped: whatever still holds is decided again.
        if (Cluster::CurrentClusterId(_record) != context.decidedFor)
        {
            _parts.logger.Logf(LogLevel::Debug,
                               "formation: dropping '{}', decided in cluster {}: this node is in {} now",
                               why,
                               context.decidedFor,
                               Cluster::CurrentClusterId(_record));
            return FireOutcome {};
        }
        row = Cluster::TransitionFor(_record.mode, trigger);
        if (row == nullptr)
            return FireOutcome {};
        from = _record;
    }

    auto const fromName = NodeModeRowFor(from.mode).name;
    auto const to = NodeModeRowFor(row->to).name;
    _refusedByJudge = false;
    auto moved = Move(*row, context, from);

    // Every move is ONE save -- the store a move leaves is moved at the next start, never here -- so a
    // move either kept its record or kept nothing, and there is no part-way to account for.
    std::scoped_lock const lock { _lock };
    if (moved.has_value())
    {
        AdoptLocked(*std::move(moved));
        _parts.logger.Logf(LogLevel::Info, "formation: {} -> {} ({})", fromName, to, why);
        return FireOutcome { .fired = true, .reform = row->reform };
    }
    // A move the startup rules refuse was said -- once, and as a condition -- where it was judged.
    if (!_refusedByJudge)
        _parts.logger.Logf(LogLevel::Error,
                           "formation: cannot move {} -> {} ({}): {}; staying {}",
                           fromName,
                           to,
                           why,
                           moved.error(),
                           fromName);
    return FireOutcome {};
}

/// Adopt @p record as what this node is: the store has just kept it. Takes the lock.
void FormationController::Publish(Cluster::FormationRecord const& record)
{
    std::scoped_lock const lock { _lock };
    AdoptLocked(record);
}

/// Make @p record what this node is, and drop what was queued for a decision when it changes the
/// node's mode. The caller holds the lock.
///
/// **Each mode decides only what it queued.** A solitary beat queues a typed seed's answer and decides
/// on the NEXT beat -- and a node that became a voter in between (its cluster recorded a second
/// machine) would otherwise read that answer at its first member beat as evidence its fleet is split,
/// though it was asked for as a fleet to JOIN. The two beats read different things into one proof, so
/// the queue does not survive the mode it was filled in; whatever still holds is proven again.
void FormationController::AdoptLocked(Cluster::FormationRecord record)
{
    if (record.mode != _record.mode)
        _seen.clear();
    _record = std::move(record);
}

/// Perform @p row's move from @p from and return the record it wrote, every save through the
/// publisher. The two effects that change which cluster the node runs are `FormationEffects`' own
/// sequences; every other effect writes one record. Runs with no lock held.
std::expected<Cluster::FormationRecord, std::string> FormationController::Move(Cluster::FormationTransition const& row,
                                                                               TriggerContext const& context,
                                                                               Cluster::FormationRecord const& from)
{
    // A closed set whose arms are different work, the `EnrollRoleTable` precedent.
    switch (row.effect)
    {
        case FormationEffect::Dissolve:
            // An admission is acted on only once verified under a key this node proved, which the poll
            // holds whenever it reads one -- so no key here is a caller's mistake, refused rather than
            // recorded as a membership nothing vouches for.
            if (!context.admittedBy.has_value())
                return std::unexpected { std::string { "the admission names no key this node proved" } };
            return DissolveInto(from,
                                context.roster,
                                *context.admittedBy,
                                Joiner(std::nullopt),
                                _publisher,
                                _parts.endpoints,
                                _parts.logger);
        case FormationEffect::ArchiveAndMint:
            return ArchiveAndMint(from, _publisher, _parts.random, _parts.wall.get());
        case FormationEffect::LeaveForSurvivor:
            if (context.order == nullptr)
                return std::unexpected { std::string { "no survivor was named to leave for" } };
            return LeaveForSurvivor(from, *context.order, _publisher, _parts.random, _parts.wall.get());
        case FormationEffect::RecordJoinTarget:
        case FormationEffect::ClearJoinTarget:
        case FormationEffect::RememberRejection:
        case FormationEffect::AdoptSeat:
        case FormationEffect::None:
            break;
    }
    auto next = from;
    next.mode = row.to;
    if (auto applied = ApplyToRecord(row.effect, context, next); !applied.has_value())
        return std::unexpected { std::move(applied).error() };
    if (auto saved = _publisher.Save(next); !saved.has_value())
        return std::unexpected { std::format("cannot record it: {}", saved.error().context) };
    return next;
}

/// What a record-only @p effect writes into @p next.
std::expected<void, std::string> FormationController::ApplyToRecord(FormationEffect effect,
                                                                    TriggerContext const& context,
                                                                    Cluster::FormationRecord& next) const
{
    switch (effect)
    {
        case FormationEffect::RecordJoinTarget: {
            if (context.fleet == nullptr)
                return std::unexpected { std::string { "no fleet was named to ask" } };
            auto const asked = WallSeconds();
            next.joining = Cluster::JoinTarget { .summary = context.fleet->Summary(),
                                                 .provenKey = context.fleet->Key(),
                                                 .askedAtUnixSeconds = asked };
            Cluster::RememberAsked(next,
                                   Cluster::AskedJoin { .clusterId = context.fleet->Summary().clusterId,
                                                        .provenKey = context.fleet->Key(),
                                                        .askedAtUnixSeconds = asked });
            return {};
        }
        case FormationEffect::ClearJoinTarget:
            next.joining.reset();
            return {};
        case FormationEffect::RememberRejection:
            if (next.joining.has_value())
                next.rejectedBy =
                    Cluster::RejectionMemo { .clusterId = next.joining->summary.clusterId, .atUnixSeconds = WallSeconds() };
            next.joining.reset();
            return {};
        case FormationEffect::AdoptSeat:
        case FormationEffect::None:
            return {};
        case FormationEffect::Dissolve:
        case FormationEffect::ArchiveAndMint:
        case FormationEffect::LeaveForSurvivor:
            return std::unexpected { std::string { "a move of cluster is its own sequence, never one record" } };
    }
    return {};
}

/// Who this node asks to be admitted as.
///
/// A learner, stating its `0xFC` endpoint exactly when the leader's
/// `EnrollRoleTable` row for a learner says the role states one. The SAME column the responder judges
/// a request by: a joiner stating one the row says it does not was refused `MalformedFrame` at every
/// poll, and the zero-config join never reached the operator's list. Read NOW, at every poll, and
/// stated as every summary states it: never an endpoint that would send a client to itself.
/// @param challenge The asked node's latest challenge for this joiner, or none.
JoinerIdentity FormationController::Joiner(std::optional<Wire::EnrollChallenge> challenge) const
{
    constexpr auto role = Wire::EnrollRole::Learner;
    return JoinerIdentity { .nodeId = _self.nodeId,
                            .nodeEndpoint = EnrollRoleRowFor(role).statesEndpoint
                                                ? StatedNodeEndpoint(_self.advertised.Current())
                                                : std::string {},
                            .role = role,
                            .publicKey = _self.publicKey,
                            .challenge = challenge };
}

/// Point the next poll at the recorded fleet's leader, with a fresh redirect chain and give-up. The
/// caller holds the lock, or is the constructor.
///
/// **Every key an answer is held to is reached from the key that proved the fleet** (`provenKey`,
/// the chain's ROOT), never from whichever key answers at an endpoint. When the summary it signed
/// says its speaker LEADS, the leader's endpoint is the speaker's own and the root is the key polled.
/// When it names another leader, the key it states for that leader (`FleetSummary::leaderKey`) is
/// the one the leader's endpoint must prove; when it states none, the root's own endpoint is asked
/// for it, and failing that the leader's endpoint must prove the root itself.
void FormationController::ResetPoll()
{
    ResetChain();
    _failingSince.reset();
    _lastSaid.clear();
    _challenge.reset();
}

/// Back to the endpoint the join was decided on, with the keys `ResetPoll` describes, and nothing
/// else: the give-up clock keeps running. What a redirect chain ending, or a redirect target that
/// proves nothing, sends a node back with -- neither is an answer anybody signed, so neither may
/// buy the join more time. The caller holds the lock, or is the constructor.
void FormationController::ResetChain()
{
    _pollEndpoint = _record.joining.has_value() ? _record.joining->summary.leaderNodeEndpoint : std::string {};
    _pollKey.reset();
    _expectedKey.reset();
    _anchor.reset();
    if (_record.joining.has_value())
    {
        auto const& decided = _record.joining->summary;
        auto const& root = _record.joining->provenKey;
        if (!decided.nodeId.empty() && decided.leaderId == decided.nodeId)
        {
            _pollKey = root;
            _anchor = ChainLink { .endpoint = decided.leaderNodeEndpoint, .key = root };
        }
        else
        {
            if (ParseDialEndpoint(decided.nodeEndpoint).has_value())
                _anchor = ChainLink { .endpoint = decided.nodeEndpoint, .key = root };
            if (decided.leaderKey.has_value())
                _expectedKey = *decided.leaderKey;
            else if (!_anchor.has_value())
                _expectedKey = root;
        }
    }
    _redirects = 0;
}

/// Prove who answers at @p endpoint for @p clusterId, and hold its key for the polls that follow --
/// but only the key the chain from the key that proved the fleet reaches.
///
/// **A fresh probe proves only that somebody at @p endpoint holds SOME key.** Whoever answers there
/// -- a redirect names an endpoint nobody signed -- would otherwise be held to its own key, which is
/// no binding at all. So the key it must prove is @p expected, reached through the chain; and when
/// none has been reached for this endpoint yet (a redirect moved it), @p anchor -- an endpoint whose
/// key the chain already proved, the root's own where it is known -- is asked which key leads now,
/// and its answer counts only under its own key. Where the decided summary states no endpoint for
/// the root, the anchor is the first endpoint the chain proved: the chain is rooted at `provenKey`,
/// TRANSITIVELY, since every link was reached from it -- and a root-only chain would leave a member
/// leaving on a dissolve, whose order names no speaker endpoint, unable to follow any redirect.
/// @param endpoint Where the next poll goes.
/// @param clusterId The fleet this node asked.
/// @param expected The key @p endpoint must prove, when the chain has reached one.
/// @param anchor Where a key the chain proved answers, to learn @p expected from.
/// @return Nothing when the key was proved (or the node moved meanwhile); else why not, as the
///         `Fatal` reading the beat decides on -- counted towards giving the join up like any
///         endpoint that does not answer.
std::optional<EnrollReading> FormationController::ProvePollEndpoint(std::string const& endpoint,
                                                                    std::string const& clusterId,
                                                                    std::optional<Ed25519PublicKey> expected,
                                                                    std::optional<ChainLink> const& anchor)
{
    auto const fatal = [](std::string detail) {
        return EnrollReading { .progress = EnrollProgress::Fatal, .detail = std::move(detail), .roster = {} };
    };

    if (!expected.has_value())
    {
        if (!anchor.has_value())
            return fatal(std::format("no key for the leader of {} can be reached from the key that proved it: the "
                                     "summary this node decided on names none, and nowhere that key answers",
                                     clusterId));
        auto const vouched = _parts.probe.AskSummary(anchor->endpoint);
        if (!vouched.has_value())
            return fatal(std::format("cannot ask {} which key leads {}: {}", anchor->endpoint, clusterId, vouched.error()));
        if (vouched->Key() != anchor->key || vouched->Summary().clusterId != clusterId)
            return fatal(std::format("{} no longer proves key {} for {}, so nothing it says about the leader is "
                                     "believed",
                                     anchor->endpoint,
                                     FormatEd25519PublicKey(anchor->key),
                                     clusterId));
        if (!vouched->Summary().leaderKey.has_value())
            return fatal(std::format("{} names no key for the leader of {} yet", anchor->endpoint, clusterId));
        expected = *vouched->Summary().leaderKey;
    }

    auto const proven = _parts.probe.AskSummary(endpoint);
    if (!proven.has_value())
        return fatal(std::format("cannot prove who answers at {}: {}", endpoint, proven.error()));
    if (proven->Summary().clusterId != clusterId)
        return fatal(std::format("{} proves a key for cluster {}, not for {}; it is not asked to admit this node",
                                 endpoint,
                                 proven->Summary().clusterId,
                                 clusterId));
    if (proven->Key() != *expected)
        return fatal(std::format("{} proves key {} for {}, not the key {} reached from the key that proved it; it is "
                                 "not asked to admit this node",
                                 endpoint,
                                 FormatEd25519PublicKey(proven->Key()),
                                 clusterId,
                                 FormatEd25519PublicKey(*expected)));
    // **A leader the pin does not name is never asked** -- reached through a redirect or named by the
    // summary decided on alike: its every answer would be signed by a key no pinned voter is. The
    // refusal names the key, because a voter promoted since the pin was written is the honest cause
    // and adding it is the remedy.
    if (!Cluster::AdmitsFleet(_self.pin, clusterId, proven->Key()))
    {
        _parts.metrics.Increment(IMetricsSink::Counter::FormationAdmissionsRefusedPin);
        auto const pin = Cluster::PinText(_self.pin);
        return fatal(std::format("{} leads {} under key {}, which --fleet-id does not pin; if it is a voter of the "
                                 "fleet you meant, add it: --fleet-id={},{}",
                                 endpoint,
                                 clusterId,
                                 FormatEd25519PublicKey(proven->Key()),
                                 pin,
                                 FormatEd25519PublicKey(proven->Key())));
    }

    std::scoped_lock const lock { _lock };
    // The key is held for the endpoint it was proved at, and only while it is still the one asked.
    if (_record.mode == NodeMode::Pending && _record.joining.has_value() && _record.joining->summary.clusterId == clusterId
        && _pollEndpoint == endpoint)
    {
        _pollKey = *expected;
        _expectedKey = *expected;
        if (!_anchor.has_value())
            _anchor = ChainLink { .endpoint = endpoint, .key = *expected };
        _parts.logger.Logf(LogLevel::Info,
                           "formation: {} proved key {} for {}; asking it to admit this node",
                           endpoint,
                           FormatEd25519PublicKey(*expected),
                           clusterId);
    }
    return std::nullopt;
}

/// One poll of @p endpoint, whose key this node proved, over a nonce drawn for it -- and what the
/// answer is worth: any answer of the fleet's is believed only when its signature binds it to this
/// request, its outcome and that key, and an admission then only when its roster records this node
/// and the key that proved the fleet.
/// @param endpoint Where the poll goes.
/// @param target The fleet this node asked.
/// @param pollKey The key proved at @p endpoint for that fleet.
/// @return The reading the beat decides on.
EnrollReading FormationController::PollProven(std::string const& endpoint,
                                              Cluster::JoinTarget const& target,
                                              Ed25519PublicKey const& pollKey,
                                              std::optional<Wire::EnrollChallenge> challenge)
{
    auto const fatal = [](std::string detail) {
        return EnrollReading { .progress = EnrollProgress::Fatal, .detail = std::move(detail), .roster = {} };
    };

    // **A request this node already knows the leader refuses is not sent.** The role states where this
    // node's `0xFC` port answers and the record keeps it, so an endpoint only this machine reaches -- a
    // loopback or wildcard advertise, or no node surface at all -- is stated as none, and a learner
    // stating none is refused `MalformedFrame` at every poll, counted as the LEADER's malformed request.
    // The reason is local, so it is said here, once (the beat says a reading once until it changes), and
    // the join is given up as any other unanswered one: until the advertised endpoint is one a peer can
    // dial -- an accepted reload of `--advertise` is read at the next beat -- there is nothing to ask.
    auto const joiner = Joiner(challenge);
    if (EnrollRoleRowFor(joiner.role).statesEndpoint && joiner.nodeEndpoint.empty())
        return fatal(std::format("this node advertises no 0xFC endpoint another machine can dial ({}), and a fleet "
                                 "records where each member answers -- so it does not ask {} to admit it until "
                                 "--advertise names one that is not loopback, localhost or a wildcard",
                                 _self.advertised.Current().empty() ? std::string { "none" } : _self.advertised.Current(),
                                 endpoint));

    // A nonce nobody can predict, or no request: one a relay could guess is one whose answer it could
    // have recorded from another ask.
    auto nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
    if (auto const drawn = _parts.random.Fill(nonce); !drawn.has_value())
        return fatal(std::format("no nonce could be drawn to ask {}: {}", endpoint, drawn.error().ToString()));

    auto reading = _parts.enroll.Poll(endpoint, joiner, nonce);
    // A wire refusal -- a redirect, a closed window, a failure -- is signed by nobody and is judged
    // where it is read: a redirect's endpoint is proved before it is asked.
    auto const outcome = SignedOutcomeOf(reading.progress);
    if (!outcome.has_value())
        return reading;

    // The roster is public -- every member's id and key rides every beacon -- so nothing in it says
    // who answered, and a refusal or a "not yet" carries nothing at all. The signature does: over this
    // request's nonce, this node, the fleet's id and the OUTCOME, by the key proved here. Anything
    // else is refused by name and counted, and reads as NO answer -- never as the refusal it claims
    // to be -- which counts towards giving the join up and is asked again on the next beat.
    auto const verdict =
        Cluster::VerifyAdmission(Cluster::AdmissionClaim { .nonce = nonce,
                                                           .joinerId = joiner.nodeId,
                                                           .joinerKey = joiner.publicKey,
                                                           .clusterId = target.summary.clusterId,
                                                           .outcome = *outcome,
                                                           .roster = reading.roster,
                                                           .challenge = Wire::ChallengeBytes(reading.challenge) },
                                 reading.signature,
                                 pollKey);
    if (verdict != Cluster::AdmissionSignature::Verified)
    {
        _parts.metrics.Increment(IMetricsSink::Counter::FormationAdmissionsUnverified);
        return fatal(std::format("{} answered {}", endpoint, Cluster::WordsFor(verdict)));
    }
    if (reading.progress != EnrollProgress::Admitted)
        return reading;

    // And the fleet's own answer is believed only when its roster records THIS node under THIS key,
    // and a member under the key that proved the fleet -- the one question `CheckAdmission` asks, and
    // the dissolve asks again before it writes anything. One that does not is the fleet answering
    // wrongly, counted apart.
    if (auto admitted = CheckAdmission(joiner, target, reading.roster); !admitted.has_value())
    {
        _parts.metrics.Increment(IMetricsSink::Counter::FormationAdmissionsRefused);
        return fatal(std::format("{}: {}", endpoint, admitted.error()));
    }
    return reading;
}

/// Count a reading nobody signed towards giving the join up: start the clock at the first, and say
/// whether `PendingGiveUpAfter` has passed since. The caller holds the lock.
/// @return True once the join has gone that long with no answer a proven key signed.
bool FormationController::GiveUpDueLocked()
{
    auto const now = _parts.clock.now();
    if (!_failingSince.has_value())
        _failingSince = now;
    return now - *_failingSince >= PendingGiveUpAfter;
}

/// The wall clock in whole seconds since the Unix epoch; zero for a clock that reads before it.
std::uint64_t FormationController::WallSeconds() const
{
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(_parts.wall.now().time_since_epoch()).count();
    return seconds < 0 ? 0 : static_cast<std::uint64_t>(seconds);
}

/// Whether @p clusterId refused this node less than `RejectedRetryAfter` ago. A wall clock that
/// stepped back past the refusal reads as still inside the window: compared BEFORE subtracting.
bool FormationController::RecentlyRejectedBy(std::string_view clusterId) const
{
    if (!_record.rejectedBy.has_value() || _record.rejectedBy->clusterId != clusterId)
        return false;
    auto const now = WallSeconds();
    auto const at = _record.rejectedBy->atUnixSeconds;
    auto const window = static_cast<std::uint64_t>(std::chrono::seconds { RejectedRetryAfter }.count());
    return now < at || now - at < window;
}

/// Say, once per fleet id, that a fleet is never asked because of its id: a node that never joins
/// with no reason given is a confident wrong signal. Latched per id, bounded so a segment shouting
/// ids cannot grow it without end. The caller holds the lock.
void FormationController::NameUnaskable(std::string const& clusterId)
{
    if (_namedUnaskable.contains(clusterId))
        return;
    if (_namedUnaskable.size() >= MaxNamedUnaskableFleets)
        return;
    _namedUnaskable.insert(clusterId);
    _parts.logger.Logf(LogLevel::Warn,
                       "formation: never asking the fleet '{}' to admit this node: its id is not one an archive can be "
                       "named by (letters, digits, '_' and '-', at most {} bytes, no device name), and this node would "
                       "have nowhere to keep that fleet's store if the fleet ever forgot it",
                       BoundedPeerText(clusterId, CompileCacheWire::MaxIdBytes),
                       CompileCacheWire::MaxIdBytes);
}

/// Say, once per fleet id, that the pin keeps this node from asking a fleet it would otherwise have
/// asked: latched and bounded as `NameUnaskable` is. The caller holds the lock.
void FormationController::NamePinWithheld(std::string const& clusterId)
{
    if (_namedPinWithheld.contains(clusterId) || _namedPinWithheld.size() >= MaxNamedUnaskableFleets)
        return;
    _namedPinWithheld.insert(clusterId);
    _parts.logger.Logf(LogLevel::Warn,
                       "formation: not asking the fleet {} to admit this node, which trust on first use would have: "
                       "--fleet-id pins it to {}",
                       BoundedPeerText(clusterId, CompileCacheWire::MaxIdBytes),
                       Cluster::PinText(_self.pin));
}

/// Refuse this node's fleet's order to dissolve into @p clusterId, which the pin does not admit:
/// counted and said once per survivor, every applied state after the first naming the same one. The
/// caller holds the lock.
void FormationController::RefuseUnpinnedOrderLocked(std::string const& clusterId)
{
    if (_refusedOrderFor == clusterId)
        return;
    _refusedOrderFor = clusterId;
    _parts.metrics.Increment(IMetricsSink::Counter::FormationAdmissionsRefusedPin);
    _parts.logger.Logf(LogLevel::Warn,
                       "formation: not following this fleet's dissolve into {}: --fleet-id pins this node to {}; "
                       "staying {} while the rest of the fleet leaves",
                       BoundedPeerText(clusterId, CompileCacheWire::MaxIdBytes),
                       Cluster::PinText(_self.pin),
                       NodeModeRowFor(_record.mode).name);
}

/// Ask for the reform a move called for, with the lock released.
void FormationController::Reform(FireOutcome outcome)
{
    if (outcome.reform)
        _parts.reform.RequestReform();
}

} // namespace FastCache::Node

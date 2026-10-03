// SPDX-License-Identifier: Apache-2.0
#include "FormationController.hpp"
#include "FormationEffects.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/SelfForgotten.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/PeerText.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <utility>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;
using Cluster::FormationEffect;
using Cluster::FormationTrigger;
using Cluster::NodeMode;
using Cluster::NodeModeRowFor;

/// What a trigger carries into the record it changes.
struct FormationController::TriggerContext
{
    Cluster::ProvenFleet const* fleet { nullptr }; ///< `YieldDecided`: the fleet it yields to.
    std::span<std::byte const> roster {};          ///< `Approved`: the roster the fleet handed over.
};

FormationController::FormationController(FormationParts parts, SelfFacts self, Cluster::FormationRecord record):
    _parts { std::move(parts) },
    _self { std::move(self) },
    _publisher { *this },
    _record { std::move(record) },
    _nextProbeAt { _parts.clock.now() }
{
    ResetPoll();
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
    return Wire::FleetSummary {
        .clusterId = Cluster::CurrentClusterId(_record),
        .state = row.announces,
        .createdAtUnixSeconds = createdAt,
        .leaderId = _leaderId,
        .leaderNodeEndpoint = _leaderNodeEndpoint,
        .nodeId = _self.nodeId,
        .raftEndpoint = row.raftListener == Cluster::RaftListenerState::Open ? _self.raftEndpoint : std::string {},
    };
}

void FormationController::OnFleetProven(Cluster::ProvenFleet const& fleet)
{
    std::scoped_lock const lock { _lock };
    Queue(fleet);
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
        case NodeMode::Learner:
        case NodeMode::Voter:
            return;
    }
}

/// A solitary beat: ask a typed seed not yet asked; else decide over what was proven; else, when one
/// is due, ask the next seed.
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
    auto probeDue = std::optional<Cluster::SeedCandidate> {};
    {
        std::scoped_lock const lock { _lock };
        if (_record.mode != NodeMode::Solitary)
            return;

        auto eligible = std::vector<Cluster::ProvenFleet> {};
        // A fleet is asked only if it can be: it has not just refused this node, it names where it
        // answers, and its id could name the archive of its store should it ever forget this node --
        // the dissolve refuses one that could not, so asking it would be a join that cannot finish.
        for (auto const& fleet: _seen)
        {
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

        target = Cluster::PreferredTarget(CurrentLocked(), eligible);
        if (!target.has_value() && !seeds.empty() && _parts.clock.now() >= _nextProbeAt)
        {
            probeDue = seeds[_probeCursor % seeds.size()];
            ++_probeCursor;
            _nextProbeAt = _parts.clock.now() + SeedProbeInterval;
        }
    }

    if (target.has_value())
    {
        auto const why = std::format("asking {} at {}", target->Summary().clusterId, target->Summary().leaderNodeEndpoint);
        auto const outcome = Fire(FormationTrigger::YieldDecided, TriggerContext { .fleet = &*target, .roster = {} }, why);
        if (outcome.fired)
        {
            _parts.metrics.Increment(IMetricsSink::Counter::FormationYields);
            std::scoped_lock const lock { _lock };
            ResetPoll();
        }
        Reform(outcome);
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

/// A pending beat: one poll of the fleet it asked, and what the answer moves.
void FormationController::TickPending()
{
    auto endpoint = std::string {};
    auto target = Cluster::JoinTarget {};
    {
        std::scoped_lock const lock { _lock };
        if (_record.mode != NodeMode::Pending || !_record.joining.has_value())
            return;
        endpoint = _pollEndpoint;
        target = *_record.joining;
    }
    auto const& clusterId = target.summary.clusterId;

    auto reading = _parts.enroll.Poll(endpoint, Joiner());

    // An admission is believed only when the roster records THIS node under THIS key, and a member
    // under the key that proved the fleet -- the one question `CheckAdmission` asks, and the dissolve
    // asks again before it writes anything. One that does not is counted, and is a fleet answering
    // wrongly, which counts towards giving the join up.
    if (reading.progress == EnrollProgress::Admitted)
        if (auto admitted = CheckAdmission(Joiner(), target, reading.roster); !admitted.has_value())
        {
            _parts.metrics.Increment(IMetricsSink::Counter::FormationAdmissionsRefused);
            reading = EnrollReading { .progress = EnrollProgress::Fatal,
                                      .detail = std::format("{}: {}", endpoint, admitted.error()),
                                      .roster = {},
                                      .certificate = {} };
        }

    // What the answer asks for is decided under the lock; the move it asks for is made outside it.
    auto trigger = std::optional<FormationTrigger> {};
    auto why = std::string {};
    {
        std::scoped_lock const lock { _lock };
        // The applied state may have moved the node while the poll was out -- its own cluster
        // recorded another machine -- and an answer about a join it no longer holds decides nothing.
        if (_record.mode != NodeMode::Pending || !_record.joining.has_value()
            || _record.joining->summary.clusterId != clusterId)
            return;

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
                // A CHAIN bound, as `RunEnrollClient`'s: past it the next poll goes back to the fleet's
                // own leader endpoint rather than following two stale leaders around each other.
                _failingSince.reset();
                if (_redirects >= MaxEnrollRedirects)
                {
                    ResetPoll();
                    _parts.logger.Logf(LogLevel::Info,
                                       "formation: gave up after {} leader redirect(s); asking {} again",
                                       MaxEnrollRedirects,
                                       _pollEndpoint);
                }
                else
                {
                    ++_redirects;
                    _pollEndpoint = reading.detail;
                }
                break;
            case EnrollProgress::Waiting:
            case EnrollProgress::Closed:
                // A node answered on its own behalf: the chain is broken and the fleet is answering.
                _redirects = 0;
                _failingSince.reset();
                if (reading.detail != _lastSaid)
                {
                    _lastSaid = reading.detail;
                    _parts.logger.Logf(LogLevel::Info, "formation: {} ({}); asking again", reading.detail, clusterId);
                }
                break;
            case EnrollProgress::Fatal: {
                auto const now = _parts.clock.now();
                if (!_failingSince.has_value())
                    _failingSince = now;
                if (now - *_failingSince < PendingGiveUpAfter)
                {
                    if (reading.detail != _lastSaid)
                    {
                        _lastSaid = reading.detail;
                        _parts.logger.Logf(LogLevel::Warn, "formation: asking {} failed: {}", clusterId, reading.detail);
                    }
                    break;
                }
                trigger = FormationTrigger::Abandoned;
                why = std::format(
                    "{} stopped answering for {} minute(s): {}", clusterId, PendingGiveUpAfter.count(), reading.detail);
                break;
            }
        }
    }
    if (!trigger.has_value())
        return;

    auto const outcome = Fire(*trigger, TriggerContext { .fleet = nullptr, .roster = reading.roster }, why);
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
        _leaderId = leader.value_or(Consensus::NodeId {});
        _leaderNodeEndpoint = std::string { leaderNodeEndpoint };
        _voters.clear();
        for (auto const& member: state.members)
            if (member.seat == Cluster::MemberSeat::Voter)
                _voters.push_back(member.id);

        // Forgotten first: a state that forgot this node says nothing more about it, and whatever
        // else it holds is about a cluster this node is leaving.
        if (Cluster::IsSelfForgotten(state, _self.nodeId, HostOfEndpoint(_self.raftEndpoint)))
            triggers.emplace_back(FormationTrigger::SelfForgotten, "the cluster forgot this node");
        else
        {
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
        auto const next = Fire(trigger, TriggerContext {}, why);
        outcome.fired = outcome.fired || next.fired;
        outcome.reform = outcome.reform || next.reform;
    }
    Reform(outcome);
}

void FormationController::OnOwnKeyRevoked(Consensus::NodeId const& acceptor)
{
    {
        std::scoped_lock const lock { _lock };
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
                TriggerContext {},
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

FormationController::RecordPublisher::RecordPublisher(FormationController& owner) noexcept:
    _owner { owner }
{
}

std::expected<std::optional<Cluster::FormationRecord>, ConsensusError> FormationController::RecordPublisher::Load() const
{
    return _owner._parts.store.Load();
}

std::expected<void, ConsensusError> FormationController::RecordPublisher::Save(Cluster::FormationRecord const& record)
{
    auto saved = _owner._parts.store.Save(record);
    if (saved.has_value())
        _owner.Publish(record);
    return saved;
}

/// Fire @p trigger: move the record by its row -- written BEFORE the node acts on it -- and adopt
/// what was written. The caller holds NO lock, and asks for the reform once this returns.
///
/// **No disk work under `_lock`**: every beacon and proof reads `Current` through it, and a slow disk
/// must not stall discovery. So the row and the record it moves from are read under the lock, the
/// move -- saves, the archive -- runs outside it, and each record the move writes is published under
/// the lock the moment the store has it (`RecordPublisher`), so `Current` says what the node is at
/// every step. Moves are serialized among themselves by `_effectLock`, taken first and never while
/// `_lock` is held.
FormationController::FireOutcome FormationController::Fire(FormationTrigger trigger,
                                                           TriggerContext const& context,
                                                           std::string_view why)
{
    std::scoped_lock const serialized { _effectLock };
    auto from = Cluster::FormationRecord {};
    auto publishedBefore = std::uint64_t { 0 };
    Cluster::FormationTransition const* row = nullptr;
    {
        std::scoped_lock const lock { _lock };
        row = Cluster::TransitionFor(_record.mode, trigger);
        if (row == nullptr)
            return FireOutcome {};
        from = _record;
        publishedBefore = _published;
    }

    auto const fromName = NodeModeRowFor(from.mode).name;
    auto const to = NodeModeRowFor(row->to).name;
    auto moved = Move(*row, context, from);

    std::scoped_lock const lock { _lock };
    if (moved.has_value())
    {
        _record = *std::move(moved);
        _parts.logger.Logf(LogLevel::Info, "formation: {} -> {} ({})", fromName, to, why);
        return FireOutcome { .fired = true, .reform = row->reform };
    }

    // A move stopped part-way may already have written the record that says where the node is going:
    // then THAT is what this node now is -- published as the store took it -- and the next start
    // finishes the move from it.
    if (_published != publishedBefore)
    {
        _parts.logger.Logf(LogLevel::Error,
                           "formation: {} -> {} ({}) was recorded and stopped part-way: {}; the next start finishes it",
                           fromName,
                           to,
                           why,
                           moved.error());
        return FireOutcome { .fired = true, .reform = true };
    }
    _parts.logger.Logf(
        LogLevel::Error, "formation: cannot move {} -> {} ({}): {}; staying {}", fromName, to, why, moved.error(), fromName);
    return FireOutcome {};
}

/// Adopt @p record as what this node is: the store has just kept it. Takes the lock.
void FormationController::Publish(Cluster::FormationRecord const& record)
{
    std::scoped_lock const lock { _lock };
    _record = record;
    ++_published;
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
            return DissolveInto(
                from, context.roster, Joiner(), _publisher, _parts.archiver, _parts.endpoints, _parts.logger);
        case FormationEffect::ArchiveAndMint:
            return ArchiveAndMint(from, _publisher, _parts.archiver, _parts.random, _parts.wall.get());
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
            return std::unexpected { std::string { "a move of cluster is its own sequence, never one record" } };
    }
    return {};
}

/// Who this node asks to be admitted as.
JoinerIdentity FormationController::Joiner() const
{
    return JoinerIdentity { .nodeId = _self.nodeId,
                            .nodeEndpoint = _self.nodeEndpoint,
                            .role = Wire::EnrollRole::Learner,
                            .publicKey = _self.publicKey };
}

/// Point the next poll at the recorded fleet's leader, with a fresh redirect chain and give-up. The
/// caller holds the lock, or is the constructor.
void FormationController::ResetPoll()
{
    _pollEndpoint = _record.joining.has_value() ? _record.joining->summary.leaderNodeEndpoint : std::string {};
    _redirects = 0;
    _failingSince.reset();
    _lastSaid.clear();
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

/// Ask for the reform a move called for, with the lock released.
void FormationController::Reform(FireOutcome outcome)
{
    if (outcome.reform)
        _parts.reform.RequestReform();
}

} // namespace FastCache::Node

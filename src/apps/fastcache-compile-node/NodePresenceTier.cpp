// SPDX-License-Identifier: Apache-2.0
#include "NodePresenceTier.hpp"

// Its own header FIRST and in a group of its own, for `WorkerLease.cpp`'s reason.
#include "HostEventInbox.hpp"

#include <FastCache/Distributed/SchedulerProtocol.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <span>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    namespace Wire = FastCache::CompileCacheWire;

    /// What this machine says about itself: the endpoint, the capacity, the load and the
    /// history buckets nobody has taken yet.
    ///
    /// The thin half of the announcement, exactly as `WorkerAnnouncement` is: every rule about
    /// WHICH scheduler to talk to lives in `DialAndAnnounce`, and only what is SAID differs.
    class PresenceAnnouncement final: public IAnnouncement
    {
      public:
        PresenceAnnouncement(std::string_view endpoint,
                             Wire::CapacityFields const& capacity,
                             Wire::LoadFields const& load,
                             std::span<std::byte const> endorsement,
                             ILogger& logger,
                             SchedulerReachability& reachability) noexcept:
            _endpoint { endpoint },
            _capacity { capacity },
            _load { load },
            _endorsement { endorsement },
            _logger { logger },
            _reachability { reachability }
        {
        }

        [[nodiscard]] AnnounceOutcome Attempt(core::net::ISocket& client, std::string_view endpoint) override
        {
            auto sent = Cc::AnnounceNodePresence(client, _endpoint, _capacity, _load, _endorsement);
            if (sent.has_value())
            {
                _accepted = true;
                _reply = *std::move(sent);
                if (auto const back = _reachability.Succeeded(AnnounceStage::Announcement, endpoint); back.has_value())
                    _logger.Log(back->level, back->message);
                return AnnounceOutcome { .accepted = 1, .leader = std::nullopt };
            }

            // Both addresses, because they are different facts -- the scheduler that refused, and the
            // machine it refused -- and a message carrying one of them reads as the other. A refusal
            // naming a LEADER is the redirect the round follows at once: Debug, never the tracker, or
            // every election spends a scheduler's Warn. Formatted from the table's own sentence
            // rather than restated here, so the words cannot drift from what `SchedulerReachability`
            // says for the same outcome.
            auto const reason = std::format("at {}: {}", _endpoint, sent.error().reason);
            if (sent.error().leader.has_value())
            {
                auto const subject = std::string_view {}; // No toolchain: presence names the machine, not one.
                _logger.Log(LogLevel::Debug,
                            std::vformat(SchedulerOutcomeRowOf(SchedulerOutcome::PresenceRefused).failure,
                                         std::make_format_args(endpoint, subject, reason)));
            }
            else
            {
                // A stall or a lost peer after the socket connected never told this node
                // anything to act on, so it is this scheduler being unreachable -- not a
                // refusal to record this machine. Without this split, a hung connection
                // logged and counted as `PresenceRefused` and never raised
                // `scheduler-unreachable`.
                auto const outcome = sent.error().kind == Cc::AnnounceRefusalKind::Transport
                                         ? SchedulerOutcome::Unreachable
                                         : SchedulerOutcome::PresenceRefused;
                auto const said = _reachability.Failed(outcome, SchedulerFailure { .endpoint = endpoint, .reason = reason });
                _logger.Log(said.level, said.message);
            }
            return AnnounceOutcome { .accepted = 0, .leader = sent.error().leader };
        }

        /// @return Whether some scheduler recorded this machine, which is what says the history
        ///         batch was delivered.
        [[nodiscard]] bool Accepted() const noexcept
        {
            return _accepted;
        }

        /// @return What the scheduler that recorded this machine answered with: an encoded
        ///         certified roster, or empty.
        [[nodiscard]] std::span<std::byte const> Reply() const noexcept
        {
            return _reply;
        }

      private:
        std::string_view _endpoint;
        Wire::CapacityFields const& _capacity;
        Wire::LoadFields const& _load;
        std::span<std::byte const> _endorsement;
        ILogger& _logger;
        SchedulerReachability& _reachability;
        bool _accepted = false;
        std::vector<std::byte> _reply;
    };
} // namespace

bool AnnounceMachineOnce(PresenceRound const& round, SchedulerLink& link, IEndpointDialer& dialer)
{
    // **Zero in-flight and not cordoned, and both are facts rather than defaults.** This verb
    // carries no job count -- the server arm forces zero and says why -- because a machine
    // announcing itself has no worker of THIS fleet for one to describe; where it does run
    // one, that worker owns heartbeat reports the number and `NodeReports()` prefers those
    // entries anyway. And a cordon is the worker PROCESS state, so a machine with no worker
    // cannot be cordoned: false is what is true, not what is convenient.
    auto load = SampleMachineLoad(round.loadSampler, round.cacheTier, round.metrics, 0, false);

    // This machine own closed buckets, so the fleet record of it survives an election.
    // Bounded per round: a node absent for a day has 1440 to hand over and the frame has a
    // payload ceiling, so a catch-up converges across rounds from the oldest end.
    auto const outbox = round.sampler.NextHistoryBatch(Wire::MaxHistoryBucketsPerHeartbeat);
    load.history = Distributed::HistoryToWire(outbox);

    // What is wrong with this machine, every row whatever its state (#1364). Nothing a receiver
    // could recompute travels with it: the leader files what arrived under the machine and renders
    // it, so a leader older than a row still shows it as this node wrote it. Taken per round, so a
    // live row that cleared is clear at the leader one interval later.
    load.conditions = round.conditions.Snapshot();

    auto const accepted = AnnouncePresence(
        PresenceMessage {
            .endpoint = round.endpoint,
            .capacity = round.capacity,
            .load = load,
            .logger = round.logger,
            .prover = round.prover,
            .reachability = round.reachability,
        },
        round.roster,
        link,
        dialer);

    if (accepted && !outbox.empty())
        round.sampler.HistoryHandedThrough(outbox.back().startMillis);
    return accepted;
}

bool AnnouncePresence(PresenceMessage const& message, IPresenceRoster* roster, SchedulerLink& link, IEndpointDialer& dialer)
{
    // The roster rides the same verb (#178): a voter's endorsement out, and back whatever roster
    // the leader can certify -- which a node that holds none adopts in this same round, from
    // whichever scheduler the round's redirects and fallbacks reached.
    auto const endorsement = roster != nullptr ? roster->Endorsement() : std::vector<std::byte> {};
    PresenceAnnouncement announcement { message.endpoint, message.capacity, message.load,
                                        endorsement,      message.logger,   message.reachability };
    (void) DialAndAnnounce(
        link, message.reachability, dialer, message.logger, announcement, AnnounceProof { .prover = message.prover });

    if (announcement.Accepted() && roster != nullptr)
        roster->Offered(announcement.Reply());
    return announcement.Accepted();
}

std::unique_ptr<NodePresence> NodePresence::Start(NodePresenceParts const& parts)
{
    auto link = SchedulerLink::For(parts.cfg.schedulers);
    if (!link.has_value())
        return nullptr;

    auto presence = std::unique_ptr<NodePresence> { new NodePresence(parts, *std::move(link)) };
    presence->Launch();
    return presence;
}

NodePresence::NodePresence(NodePresenceParts const& parts, SchedulerLink link):
    _announced { parts.announced },
    _cacheTier { parts.cacheTier },
    _metrics { parts.metrics },
    _sampler { parts.sampler },
    _logger { parts.logger },
    _conditions { parts.conditions },
    _roster { parts.roster },
    _prover { parts.prover },
    _reachability { parts.reachability },
    // `AnnouncedCapacity` rather than `Distributed::CapacityToWire` alone: the latter knows
    // nothing of the version, so a node with no worker sent NODE-ANNOUNCE with none, and the
    // leader recorded it exactly as absent as a build too old to know the field.
    _capacityWire { AnnouncedCapacity(parts.capacity) },
    _loadSampler { MakeHostLoadSampler(MakeSystemCounterSource()) },
    _dialer { parts.dialer },
    _link { std::move(link) },
    _hostSubscription { parts.hostEvents, _presenceWake }
{
}

void NodePresence::Launch()
{
    _thread = std::jthread { [this](std::stop_token const& stop) { Loop(stop); } };
}

void NodePresence::Loop(std::stop_token const& stop)
{
    while (!stop.stop_requested())
    {
        // Read once per round through the seam, never captured: this is the one value the
        // worker registrations and this loop must agree on, and #1279 is what a second reader
        // of the configuration costs. On a node with no worker nothing republishes it, which
        // is correct -- there is no re-survey to learn a new address from.
        //
        // An empty one is a node that advertises nothing. The startup table already refuses
        // that for a worker; for a node running none it is legal and simply means there is no
        // address to file a row under, so the round is skipped rather than sent. Silent,
        // because it is a configuration this node started with rather than an event.
        if (auto const endpoint = _announced.Current(); !endpoint.empty())
            (void) AnnounceMachineOnce(PresenceRound { .loadSampler = *_loadSampler,
                                                       .cacheTier = _cacheTier,
                                                       .metrics = _metrics,
                                                       .sampler = _sampler,
                                                       .capacity = _capacityWire,
                                                       .endpoint = endpoint,
                                                       .logger = _logger,
                                                       .conditions = _conditions,
                                                       .roster = _roster,
                                                       .prover = _prover,
                                                       .reachability = _reachability },
                                       _link,
                                       _dialer);

        if (WaitOutInterval(stop))
            break;
    }
}

bool NodePresence::WaitOutInterval(std::stop_token const& stop)
{
    auto const interval = _roster != nullptr && _roster->Wanting()
                              ? std::chrono::duration_cast<std::chrono::milliseconds>(RosterWantingInterval)
                              : std::chrono::duration_cast<std::chrono::milliseconds>(NodeAnnounceInterval);
    return _presenceWake.WaitOut(stop, interval) == PresenceWakeReason::Stopped;
}

void PresenceWake::OnHostEvent(HostEvent event)
{
    auto const& row = HostEventActionFor(event);
    {
        std::scoped_lock const lock { _mutex };
        // A wake still pending is older than a suspend, and superseded by it; one posted after is
        // owed as ever. See `HostEventActionRow::supersedesOlderWakes`.
        if (row.supersedesOlderWakes)
            _announceNow = false;
        if (!row.wakesPresence)
            return;
        _announceNow = true;
    }
    _wake.notify_all();
}

PresenceWakeReason PresenceWake::WaitOut(std::stop_token const& stop, std::chrono::milliseconds interval)
{
    // A named lock, because the stop-token `wait_for` takes it by non-const reference -- a
    // temporary does not bind, which is the compiler catching the lifetime question rather
    // than a style preference.
    std::unique_lock lock { _mutex };
    auto const woken = _wake.wait_for(lock, stop, interval, [this] { return _announceNow; });
    if (stop.stop_requested())
        return PresenceWakeReason::Stopped;
    if (!woken)
        return PresenceWakeReason::Elapsed;
    _announceNow = false;
    return PresenceWakeReason::HostEvent;
}

} // namespace FastCache::Node

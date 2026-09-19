// SPDX-License-Identifier: Apache-2.0
#include "NodePresenceTier.hpp"

#include <FastCache/Distributed/SchedulerProtocol.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    namespace Wire = FastCache::CompileCacheWire;

    constexpr std::chrono::milliseconds PresenceIoTimeout { 10'000 };

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
                             ICredentialSource const& credential,
                             Cc::CredentialNotice& notice,
                             ILogger& logger) noexcept:
            _endpoint { endpoint },
            _capacity { capacity },
            _load { load },
            _endorsement { endorsement },
            _credential { credential },
            _notice { notice },
            _logger { logger }
        {
        }

        [[nodiscard]] AnnounceOutcome Attempt(ISocket& client, std::string_view endpoint) override
        {
            auto sent =
                Cc::AnnounceNodePresence(client, _notice, _endpoint, _capacity, _load, _endorsement, _credential.Current());
            if (sent.has_value())
            {
                _accepted = true;
                _reply = *std::move(sent);
                return AnnounceOutcome { .accepted = 1, .leader = std::nullopt };
            }

            // At Warn rather than Error, for the reason a registration refusal is: a scheduler
            // mid-election and a peer too old to know the verb are both what a healthy fleet
            // looks like for a few seconds, and the round carries on either way. It names BOTH
            // addresses because they are different facts -- the scheduler that refused, and the
            // machine it refused -- and a message carrying one of them reads as the other.
            _logger.Logf(LogLevel::Warn,
                         "scheduler {} did not record this machine at {}: {}",
                         endpoint,
                         _endpoint,
                         sent.error().reason);
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
        ICredentialSource const& _credential;
        Cc::CredentialNotice& _notice;
        ILogger& _logger;
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
            .credential = round.credential,
            .notice = round.notice,
            .logger = round.logger,
            .prover = round.prover,
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
    PresenceAnnouncement announcement { message.endpoint,   message.capacity, message.load,  endorsement,
                                        message.credential, message.notice,   message.logger };
    (void) DialAndAnnounce(
        link,
        dialer,
        message.logger,
        announcement,
        AnnounceProof { .prover = message.prover, .credential = &message.credential, .notice = &message.notice });

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
    _credential { parts.credential },
    _logger { parts.logger },
    _conditions { parts.conditions },
    _roster { parts.roster },
    _prover { parts.prover },
    // Reported at Warn and once, exactly as the registrars' notice is: a credential the
    // scheduler did not want is a configuration fact, not a per-round event.
    _notice { [&logger = parts.logger](std::string_view text) { logger.Logf(LogLevel::Warn, "scheduler: {}", text); } },
    _capacityWire { Distributed::CapacityToWire(parts.capacity) },
    _loadSampler { MakeHostLoadSampler(MakeSystemCounterSource()) },
    _dialer { PresenceIoTimeout },
    _link { std::move(link) }
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
                                                       .credential = _credential,
                                                       .notice = _notice,
                                                       .capacity = _capacityWire,
                                                       .endpoint = endpoint,
                                                       .logger = _logger,
                                                       .conditions = _conditions,
                                                       .roster = _roster,
                                                       .prover = _prover },
                                       _link,
                                       _dialer);

        if (WaitOutInterval(stop))
            break;
    }
}

bool NodePresence::WaitOutInterval(std::stop_token const& stop)
{
    // A named lock, because the stop-token `wait_for` takes it by non-const reference -- a
    // temporary does not bind, which is the compiler catching the lifetime question rather
    // than a style preference.
    auto const interval = _roster != nullptr && _roster->Wanting()
                              ? std::chrono::duration_cast<std::chrono::seconds>(RosterWantingInterval)
                              : NodeAnnounceInterval;
    std::unique_lock lock { _wakeMutex };
    return _wake.wait_for(lock, stop, interval, [&stop] { return stop.stop_requested(); });
}

} // namespace FastCache::Node

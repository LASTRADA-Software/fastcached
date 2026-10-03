// SPDX-License-Identifier: Apache-2.0
#include "NodeAnnounce.hpp"
#include "NodeProofClient.hpp"
#include "NodeRoster.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Platform/DaemonControls.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = FastCache::CompileCacheWire;

    /// Send every queued withdrawal on @p client and clear the queue, whatever each answered.
    /// @param round What to withdraw and where to log.
    /// @param client A connected, proved scheduler.
    /// @param endpoint Where `client` is connected, for the diagnostics.
    /// @return How many the scheduler accepted.
    std::size_t WithdrawQueued(HeartbeatRound const& round, core::net::ISocket& client, std::string_view endpoint)
    {
        // Cleared unconditionally: see `HeartbeatRound::withdrawals` for why a failure is not
        // retried.
        auto retired = std::size_t { 0 };
        for (auto& retiring: round.withdrawals)
        {
            auto const done = retiring.Withdraw(client);
            if (done.has_value())
            {
                ++retired;
                continue;
            }
            // Logged and not acted on. Every refusal here -- an `UnknownOpcode` from a
            // scheduler too old to know the verb, a `NotLeader`, an unreachable host --
            // leaves the pre-existing expiry closing the window exactly as before, so
            // this must never redirect the round, abort it, or count against it.
            // Both halves of the registry's key, because since #1279 a withdrawal may
            // name a fingerprint this worker still serves -- so the fingerprint alone
            // reads as "it dropped a toolchain it is using" and names half an entry.
            round.logger.Logf(LogLevel::Info,
                              "scheduler {} did not retire {} at {}: {}; that registration will expire instead",
                              endpoint,
                              retiring.Fingerprint(),
                              retiring.Endpoint(),
                              done.error().reason);
        }
        round.withdrawals.clear();
        return retired;
    }

    /// Prove this machine on @p client and seal it, or hand back why not. With no prover the
    /// connection is returned as it is (a test's scripted fleet).
    /// @param client A connected scheduler.
    /// @param proof How this machine proves itself.
    /// @return The connection every later frame travels on, or the attempt that failed.
    [[nodiscard]] std::expected<std::unique_ptr<core::net::ISocket>, NodeProofAttempt> ProveConnection(
        std::unique_ptr<core::net::ISocket> client, AnnounceProof const& proof)
    {
        if (proof.prover == nullptr)
            return client;
        auto sealed = std::make_unique<SealedFrameSocket>(
            // No budget: a caller holds the replies to what it asked, one at a time.
            std::move(client),
            SealedFrameEnd::Caller,
            CompileCacheWire::MaxSealedReplyPayload,
            nullptr);
        auto attempt = proof.prover->Prove(*sealed);
        if (attempt.result != NodeProofResult::Proved)
            return std::unexpected(std::move(attempt));
        return std::unique_ptr<core::net::ISocket> { std::move(sealed) };
    }
} // namespace

Wire::CapacityFields AnnouncedCapacity(Distributed::NodeCapacity const& capacity)
{
    auto fields = Distributed::CapacityToWire(capacity);
    fields.version = AdvertisedVersion();
    return fields;
}

std::optional<EndpointChange> AdvertisedEndpointChange(std::string_view inForce,
                                                       std::shared_ptr<NodeConfig const> const& live)
{
    // No configuration file means no second moment at which anything could change,
    // which is `ConfiguredCredential`'s null-reloader arm too. Asked here
    // rather than at the call site so the rule is in the tested function rather than in
    // the heartbeat loop no test reaches.
    if (live == nullptr)
        return std::nullopt;

    auto candidate = AdvertisedEndpoint(*live);

    // An empty candidate is not a change, it is an answer nothing can be registered
    // under -- so the previous value stays in force and the worker goes on being
    // reachable. It arises only where a name that reaches only this machine is withheld
    // (`AdvertisedNameWithheld`), and the startup table refuses that for a worker -- at a
    // reload too, which is judged by the same table -- so a worker never reaches it; kept
    // because the alternative is a withdrawal followed by a registration under no address
    // at all, which is a node that disappears from the fleet with every counter reading
    // normal.
    if (candidate.empty() || candidate == inForce)
        return std::nullopt;

    // Formatted before the move, because a designated initializer's arguments are
    // evaluated in whatever order the compiler likes and `endpoint` is declared first.
    auto said = std::format("now advertising {} instead of {}: clients will be told the new address, and this worker's "
                            "registration under the old one is being retired rather than left to expire. A lease "
                            "already granted for {} is refused, which costs that client a local compile",
                            candidate,
                            inForce,
                            inForce);
    return EndpointChange { .endpoint = std::move(candidate), .announcement = std::move(said) };
}

Wire::LoadFields SampleMachineLoad(IHostLoadSampler& loadSampler,
                                   CacheTier const* cacheTier,
                                   IMetricsSink const& metrics,
                                   std::uint32_t inFlight,
                                   bool cordoned)
{
    auto const sampled = loadSampler.Sample();
    // The cache is sampled here too, and per MACHINE rather than per registrar: a node with
    // two `--toolchain` flags is two registry entries against one machine and one cache, so
    // both entries carry the same figures. Summing them across entries counts that cache
    // twice, which is what `WorkerRegistry::NodeCaches()` exists to prevent on the other end.
    return Distributed::LoadToWire(Distributed::NodeLoad { .inFlight = inFlight,
                                                           .cpuBusyPermille = sampled.cpuBusyPermille,
                                                           .availableMemoryBytes = sampled.availableMemoryBytes,
                                                           .freeScratchBytes = sampled.freeScratchBytes,
                                                           .cache = CacheLoadOf(cacheTier, metrics),
                                                           .cordoned = cordoned });
}

ReportableAddresses ReportableInterfaceAddresses(std::vector<std::string> addresses)
{
    std::erase_if(addresses, [](std::string const& address) {
        return std::ranges::any_of(UnreportedAddresses,
                                   [&address](UnreportedAddressRow const& row) { return row.applies(address); });
    });
    std::ranges::sort(addresses);
    auto const [first, last] = std::ranges::unique(addresses);
    addresses.erase(first, last);

    auto const overCap =
        addresses.size() > Wire::MaxInterfaceAddresses ? addresses.size() - Wire::MaxInterfaceAddresses : std::size_t { 0 };
    addresses.resize(addresses.size() - overCap);
    return ReportableAddresses { .addresses = std::move(addresses), .overCap = overCap };
}

namespace
{
    /// What this round reports this machine answers on: the locality oracle's set as of NOW.
    ///
    /// Asked per round, because the answer this exists for is the one that moves: a VPN that
    /// reconnects under a new address mid-session. The oracle refreshes on its own interval,
    /// never because a round asked -- and that interval is what keeps the report and the ticket
    /// audience one value (`HeartbeatRound::locality`).
    ///
    /// A cap that bit is said ONCE per process, at Info: the interface count is the
    /// machine's, so the line would otherwise repeat unchanged on every heartbeat.
    /// @param round Where the addresses, the latch and the logger are.
    /// @return The addresses to send on every REGISTER and HEARTBEAT of this round.
    [[nodiscard]] std::vector<std::string> AddressesToReport(HeartbeatRound const& round)
    {
        auto reportable = ReportableInterfaceAddresses(round.locality.Addresses());
        if (reportable.overCap > 0 && !round.addressCapNoticed.exchange(true))
            round.logger.Logf(LogLevel::Info,
                              "this machine answers on {} reportable addresses and a report carries at most {}: the "
                              "{} that sort last are left out of every registration and heartbeat, so no dial hint "
                              "can name them. Said once",
                              reportable.addresses.size() + reportable.overCap,
                              Wire::MaxInterfaceAddresses,
                              reportable.overCap);
        return std::move(reportable.addresses);
    }
} // namespace

AnnounceOutcome AnnounceOnce(HeartbeatRound const& round, core::net::ISocket& client, std::string_view endpoint)
{
    // Counted rather than short-circuited: one toolchain the scheduler refuses must
    // not stop the others from being announced, or a single bad entry silently
    // un-registers the whole worker.
    std::size_t accepted = 0;

    // And counted APART, because a heartbeat and a registration are different events
    // with different costs, and one number cannot carry both (#999). `accepted` was
    // the only tally, so a steady round of six heartbeats reported itself as "6 of 6
    // toolchain(s) registered" every interval forever -- which reads as a node
    // re-announcing its whole toolchain set on a timer, and prompted exactly that
    // question from an operator. It was not doing that; measured, six registrations
    // land in the startup second and every round after is heartbeats.
    //
    // The conflation was already known one field down: `handedOver` exists because
    // "`accepted` also counts a registration, which carries no history at all". That
    // consequence was fixed and this one was not.
    std::size_t beats = 0;
    std::size_t registrations = 0;
    auto const inFlight = static_cast<std::uint32_t>(round.capacity.InFlight());

    // Sampled once per round rather than once per registrar: every entry describes
    // the SAME machine, so sampling per toolchain would report several different
    // views of one host and, worse, would cut the CPU interval into pieces too short
    // to mean anything.
    //
    // **No history rides this verb any more, and the plumbing that carried it is gone
    // rather than left inert** (#1440): `handedOver`, the cursor acknowledgement and the
    // conditional heartbeat that existed to step it. History is the MACHINE's and travels
    // on NODE-ANNOUNCE, which a node running no worker also sends -- see
    // `SampleMachineLoad`, which owns the argument for one carrier.
    auto load = SampleMachineLoad(round.loadSampler, round.cacheTier, round.metrics, inFlight, round.capacity.IsCordoned());
    // Sampled beside the load for the load's reason -- one reading of one machine per round --
    // and handed to REGISTER as well, which carries no load record of its own.
    load.interfaceAddresses = AddressesToReport(round);

    // The first leader any entry was pointed at. One per round rather than one per
    // registrar: every entry here describes the same machine talking to the same
    // scheduler, so they either all get redirected or none does, and following the
    // first is what lets the whole round move together.
    std::optional<std::string> leader;

    // **No proof here, and that is where it moved rather than a gap** (#178): the connection this
    // arrives on was proved and sealed by `DialAndAnnounce` before this ran, because every verb
    // below requires a proven identity and a presence announcement needs the same one.

    // **Withdrawals first, and the ordering mirrors the adding direction's.** Adding
    // runs the compile port before the registration -- `ReplaceToolchains` then
    // `registrarsFor` -- so a worker never announces a fingerprint it is not yet ready
    // to serve. Dropping wants the mirror, and gets it from the same two lines: the
    // compile port has ALREADY stopped serving these fingerprints by the time a
    // registrar reaches this list, so the scheduler is told last, when the claim is
    // certainly true. That the ordering was deliberate in one direction and merely
    // incidental in the other is how #573 came to exist at all.
    //
    // **That reasoning covers a DROPPED TOOLCHAIN, which was the only way onto this
    // list until #1279 added a second: an endpoint that moved.** There the fingerprint
    // is still served and the compile port has stopped nothing -- what is being retired
    // is the address half of the key. The ordering is still right, for a reason of its
    // own rather than by inheritance: withdrawing first means the scheduler never holds
    // two live entries for one machine naming different addresses, one of which is
    // wrong. Registering first would make that window the normal case.
    (void) WithdrawQueued(round, client, endpoint);

    for (auto& registrar: round.registrars)
    {
        if (!registrar.WorkerId().empty())
        {
            auto const beat = registrar.Heartbeat(client, inFlight, load);
            if (beat.has_value())
            {
                ++accepted;
                ++beats;
                if (auto const back =
                        round.reachability.Succeeded(AnnounceStage::Announcement, endpoint, registrar.Fingerprint());
                    back.has_value())
                    round.logger.Log(back->level, back->message);
                continue;
            }
            if (!leader.has_value())
                leader = beat.error().leader;
        }

        // The scheduler's own reason, logged per toolchain. The summary below can
        // only say how many did not register, and "0 of 1" is exactly as much as an
        // operator knew about a node that had silently dropped out of the fleet -- a
        // fingerprint the scheduler will not accept, a cluster this node is not a
        // member of, a leader that has moved.
        if (auto const registered = registrar.Register(client, load.interfaceAddresses); registered.has_value())
        {
            // The fleet the scheduler named, adopted here rather than configured. Until
            // this runs the worker is unpinned and refuses every grant, which is the
            // window #401 closes; from here it refuses every grant naming another fleet.
            // Re-registration re-pins, because a node that has been accepted somewhere
            // else now serves whatever that scheduler leads.
            round.lease.fleet.Pin(registrar.ClusterId());
            ++accepted;
            ++registrations;
            if (auto const back =
                    round.reachability.Succeeded(AnnounceStage::Announcement, endpoint, registrar.Fingerprint());
                back.has_value())
                round.logger.Log(back->level, back->message);

            // A registration carries no load, so a scheduler that has just (re)admitted a
            // CORDONED worker believes it serving until its next heartbeat -- a new leader
            // after an election, most often, which is exactly when nothing else tells it
            // (#1303: nothing replicates a cordon). Said at once instead, and only then: a
            // serving worker's registration already reads as what it is.
            //
            // Not counted as a beat: `DescribeAnnounceRound` reads beats and registrations
            // as a partition of the registrars, and this one already registered.
            if (load.cordoned)
                (void) registrar.Heartbeat(client, inFlight, load);
        }
        else
        {
            if (!leader.has_value())
                leader = registered.error().leader;
            // A refusal naming a LEADER is the redirect this round follows at once, not a setback:
            // Debug, and never the tracker, or every election spends a scheduler's Warn. Formatted
            // from the table's own sentence rather than restated here, so the words cannot drift
            // from what `SchedulerReachability` says for the same outcome.
            if (registered.error().leader.has_value())
                round.logger.Log(
                    LogLevel::Debug,
                    std::vformat(SchedulerOutcomeRowOf(SchedulerOutcome::RegistrationRefused).failure,
                                 std::make_format_args(endpoint, registrar.Fingerprint(), registered.error().reason)));
            else
            {
                // A stall or a lost peer after the socket connected never told this node
                // anything to act on, so it is this scheduler being unreachable -- not a
                // toolchain it refused. Without this split, a hung connection logged and
                // counted as `RegistrationRefused` and never raised `scheduler-unreachable`.
                auto const outcome = registered.error().kind == Cc::AnnounceRefusalKind::Transport
                                         ? SchedulerOutcome::Unreachable
                                         : SchedulerOutcome::RegistrationRefused;
                auto const said = round.reachability.Failed(outcome,
                                                            SchedulerFailure { .endpoint = endpoint,
                                                                               .subject = registrar.Fingerprint(),
                                                                               .reason = registered.error().reason });
                round.logger.Log(said.level, said.message);
            }
        }
    }
    // What this round DID, and how loudly to say it -- see `DescribeAnnounceRound`,
    // which owns both because the wording is the defect it was written for (#999) and
    // `main.cpp` is in no test target (#909). A shortfall is Debug whatever caused it:
    // each refusal it counts was said above, through the tracker or as a redirect.
    auto const report = DescribeAnnounceRound(beats, registrations, round.registrars.size());
    round.logger.Logf(report.level, "scheduler {}: {}", endpoint, report.message);
    return AnnounceOutcome { .accepted = accepted, .leader = std::move(leader) };
}

std::size_t AnnounceRound(HeartbeatRound const& round, SchedulerLink& link, IEndpointDialer& dialer)
{
    /// The worker's announcement: registrations, withdrawals and the proof, as `AnnounceOnce`
    /// has always done them. A thin adapter so the dial rules below are reached by one path.
    class WorkerAnnouncement final: public IAnnouncement
    {
      public:
        explicit WorkerAnnouncement(HeartbeatRound const& round) noexcept:
            _round { round }
        {
        }

        [[nodiscard]] AnnounceOutcome Attempt(core::net::ISocket& client, std::string_view endpoint) override
        {
            return AnnounceOnce(_round, client, endpoint);
        }

      private:
        HeartbeatRound const& _round;
    };

    WorkerAnnouncement announcement { round };
    return DialAndAnnounce(
        link, round.reachability, dialer, round.logger, announcement, AnnounceProof { .prover = round.prover });
}

std::size_t DialAndAnnounce(SchedulerLink& link,
                            SchedulerReachability& reachability,
                            IEndpointDialer& dialer,
                            ILogger& logger,
                            IAnnouncement& announcement,
                            AnnounceProof const& proof)
{
    // Not dialled at all while this node's own cluster has not recorded it: every scheduler of that
    // cluster could only refuse the proof as unknown. HERE, the one place both loops dial through,
    // so neither can reach its own scheduler early; `HoldUntilRecorded` says why, once per hold.
    if (proof.prover != nullptr && proof.prover->HoldUntilRecorded(logger))
        return 0;

    for (link.BeginRound();;)
    {
        auto client = dialer.Dial(link.Target(), core::net::DialOptions { .connectTimeout = HeartbeatConnectTimeout });
        if (client == nullptr)
        {
            // Named BEFORE `Lost()` moves the target, and the fallback named after it:
            // with several scheduler endpoints (`SchedulersOf`) the sentence has to say where
            // this node went next, which "the configured endpoint" no longer identifies (#1310). How
            // LOUDLY is `SchedulerReachability`'s: on the transition, then on a cadence, never per
            // round.
            auto const unreachable = link.Target();
            auto const next = link.Lost();
            auto const said = reachability.Failed(SchedulerOutcome::Unreachable,
                                                  SchedulerFailure { .endpoint = unreachable, .next = next });
            logger.Log(said.level, said.message);
            // Another configured endpoint is tried now rather than a heartbeat interval
            // from now: this machine is out of the fleet for as long as it takes, and a
            // configured endpoint is the one still standing after an election the
            // remembered leader lost, or after the machine an earlier entry named was
            // retired.
            if (!next.has_value())
                return 0;
            continue;
        }
        if (auto const back = reachability.Succeeded(AnnounceStage::Dial, link.Target()); back.has_value())
            logger.Log(back->level, back->message);

        // Proved before anything is said, and sealed from then on (#178). A connection the proof did
        // not seal is one no joining verb can be heard on, so it counts as an endpoint that did not
        // answer: the next scheduler endpoint is tried in this same round.
        auto proved = ProveConnection(std::move(client), proof);
        if (!proved.has_value())
        {
            // What each unproved outcome is called, and which machine it names to fix, is the
            // `SchedulerOutcomeTable`'s proof rows; `Proved` never reaches this arm.
            auto const unproved = link.Target();
            auto const next = link.Lost();
            auto const said = reachability.Failed(
                SchedulerOutcomeOfProof(proved.error().result),
                SchedulerFailure { .endpoint = unproved, .reason = proved.error().reason, .next = next });
            logger.Log(said.level, said.message);
            if (!next.has_value())
                return 0;
            continue;
        }
        if (proof.prover != nullptr)
            if (auto const back = reachability.Succeeded(AnnounceStage::Proof, link.Target()); back.has_value())
                logger.Log(back->level, back->message);
        client = *std::move(proved);

        auto const outcome = announcement.Attempt(*client, link.Target());
        if (!outcome.leader.has_value())
        {
            // Committed only when this endpoint actually took an entry. It answered
            // either way, but an endpoint that refused every registrar for its own
            // reasons -- not a member, a fingerprint it will not have -- is not a
            // leader worth starting the next round at, and pinning to it would
            // outlast the election that caused it.
            if (outcome.accepted > 0)
            {
                link.Accepted();
                return outcome.accepted;
            }
            if (!link.Lost().has_value())
                return 0;
            continue;
        }

        logger.Logf(
            LogLevel::Info, "scheduler {} is not the leader; announcing to {} instead", link.Target(), *outcome.leader);
        if (!link.Redirect(*outcome.leader))
        {
            // Two schedulers naming each other, or a leader that moved again
            // mid-chain. Costs this round rather than the thread.
            logger.Logf(LogLevel::Warn,
                        "gave up following leader redirects after {} hop(s); retrying next heartbeat",
                        MaxAnnounceRedirects);
            return 0;
        }
    }
}

std::size_t WithdrawOnce(HeartbeatRound const& round, SchedulerLink const& link, IEndpointDialer& dialer)
{
    // Nothing accepted, nothing to retire: a dial would spend up to a second of the machine's
    // sleep saying nothing.
    if (round.withdrawals.empty())
        return 0;
    auto const target = std::string { link.Target() };
    auto client = dialer.Dial(target, core::net::DialOptions { .connectTimeout = SuspendDialTimeout });
    if (client == nullptr)
    {
        round.logger.Logf(LogLevel::Info,
                          "scheduler {} unreachable before this machine sleeps; {} registration(s) will expire instead",
                          target,
                          round.withdrawals.size());
        round.withdrawals.clear();
        return 0;
    }
    auto proved = ProveConnection(std::move(client), AnnounceProof { .prover = round.prover });
    if (!proved.has_value())
    {
        round.logger.Logf(LogLevel::Info,
                          "scheduler {} did not accept this machine's identity before it sleeps ({}); {} registration(s) "
                          "will expire instead",
                          target,
                          proved.error().reason,
                          round.withdrawals.size());
        round.withdrawals.clear();
        return 0;
    }
    return WithdrawQueued(round, **proved, target);
}

std::chrono::seconds NextAnnounceWait(NodeProofClient const* prover, bool rosterWanting)
{
    auto const recordAwaited = prover != nullptr && prover->OwnRecordNow() == OwnRecord::Awaited;
    return rosterWanting || recordAwaited ? std::chrono::duration_cast<std::chrono::seconds>(RosterWantingInterval)
                                          : NodeAnnounceInterval;
}

} // namespace FastCache::Node

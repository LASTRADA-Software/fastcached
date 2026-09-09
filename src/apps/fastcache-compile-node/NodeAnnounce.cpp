// SPDX-License-Identifier: Apache-2.0
#include "NodeAnnounce.hpp"
#include "SchedulerLink.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Platform/DaemonControls.hpp>

#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = FastCache::CompileCacheWire;
} // namespace

AnnounceOutcome AnnounceOnce(HeartbeatRound const& round, ISocket& client, std::string_view endpoint)
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
    auto const sampled = round.loadSampler.Sample();
    // The cache is sampled here too, and per ROUND rather than per registrar for the
    // same reason: a node with two `--toolchain` flags is two registry entries
    // against one machine and one cache, so both entries carry the same figures.
    // Summing them across entries counts that cache twice, which is what
    // `WorkerRegistry::NodeCaches()` exists to prevent on the other end.
    auto load = Distributed::LoadToWire(Distributed::NodeLoad { .inFlight = inFlight,
                                                                .cpuBusyPermille = sampled.cpuBusyPermille,
                                                                .availableMemoryBytes = sampled.availableMemoryBytes,
                                                                .freeScratchBytes = sampled.freeScratchBytes,
                                                                .cache = CacheLoadOf(round.cacheTier, round.metrics) });

    // This machine's own closed buckets, so the fleet's record of it survives an
    // election. Bounded per round: a node absent for a day has 1440 to hand over and
    // a heartbeat has a payload ceiling, so a catch-up converges across rounds from
    // the oldest end.
    //
    // Attached to the shared load and therefore sent once per registrar, which is
    // redundant for a machine serving several toolchains and deliberately left so:
    // the leader's high-water mark already makes a repeat a no-op, and threading a
    // per-registrar payload through would buy a few kilobytes at the cost of the one
    // place this is assembled.
    auto const outbox = round.sampler.NextHistoryBatch(Wire::MaxHistoryBucketsPerHeartbeat);
    load.history = Distributed::HistoryToWire(outbox);
    // Set by a HEARTBEAT and by nothing else. `accepted` also counts a registration,
    // which carries no history at all -- so a round where every heartbeat failed and
    // one re-register succeeded would step the cursor over a batch never sent.
    auto handedOver = false;

    // The first leader any entry was pointed at. One per round rather than one per
    // registrar: every entry here describes the same machine talking to the same
    // scheduler, so they either all get redirected or none does, and following the
    // first is what lets the whole round move together.
    std::optional<std::string> leader;

    // **Withdrawals first, and the ordering mirrors the adding direction's.** Adding
    // runs the compile port before the registration -- `ReplaceToolchains` then
    // `registrarsFor` -- so a worker never announces a fingerprint it is not yet ready
    // to serve. Dropping wants the mirror, and gets it from the same two lines: the
    // compile port has ALREADY stopped serving these fingerprints by the time a
    // registrar reaches this list, so the scheduler is told last, when the claim is
    // certainly true. That the ordering was deliberate in one direction and merely
    // incidental in the other is how #573 came to exist at all.
    //
    // Cleared unconditionally afterwards: see `HeartbeatRound::withdrawals` for why a
    // failure is not retried.
    for (auto& retiring: round.withdrawals)
    {
        if (auto const retired = retiring.Withdraw(client, round.credential.Current()); !retired.has_value())
            // Logged and not acted on. Every refusal here -- an `UnknownOpcode` from a
            // scheduler too old to know the verb, a `NotLeader`, an unreachable host --
            // leaves the pre-existing expiry closing the window exactly as before, so
            // this must never redirect the round, abort it, or count against it.
            round.logger.Logf(LogLevel::Info,
                              "scheduler {} did not retire {}: {}; its registration will expire instead",
                              endpoint,
                              retiring.Fingerprint(),
                              retired.error().reason);
    }
    round.withdrawals.clear();

    for (auto& registrar: round.registrars)
    {
        if (!registrar.WorkerId().empty())
        {
            auto const beat = registrar.Heartbeat(client, inFlight, load, round.credential.Current());
            if (beat.has_value())
            {
                ++accepted;
                ++beats;
                handedOver = true;
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
        if (auto const registered = registrar.Register(client, round.credential.Current()); registered.has_value())
        {
            // `--cluster-id` is an ASSERTION, not a source and not an override. The
            // identity comes from registration; the flag, when the operator NAMED one,
            // says which fleet they expected to be admitted to. Disagreement is a
            // provisioning fault -- this node is serving a fleet somebody did not mean
            // -- so it is fatal and names both sides rather than silently preferring
            // either. Preferring the config would put configuration back above
            // registration and reopen the default-`fastcache` cross-fleet accept;
            // preferring the registration silently would make the flag a lie.
            //
            // Asked on `clusterIdExplicit` rather than on the VALUE, because the
            // default is a real fleet name and comparing against it cannot see the
            // operator who typed it. That is the option table's own provenance rule.
            if (!FleetAssertionHolds(round.cfg.clusterIdExplicit, round.cfg.clusterId, registrar.ClusterId()))
            {
                round.logger.Logf(LogLevel::Error,
                                  "scheduler {} registered this node into fleet '{}', but --cluster-id asserts "
                                  "'{}'. Refusing to serve a fleet that was not asked for: correct the flag or "
                                  "the scheduler this node is pointed at",
                                  endpoint,
                                  registrar.ClusterId(),
                                  round.cfg.clusterId);
                round.fleetMismatch = true;
                DaemonControls::Instance().RequestStop();
                return AnnounceOutcome { .accepted = accepted, .leader = std::move(leader) };
            }

            // The fleet the scheduler named, adopted here rather than configured. Until
            // this runs the worker is unpinned and refuses every grant, which is the
            // window #401 closes; from here it refuses every grant naming another fleet.
            // Re-registration re-pins, because a node that has been accepted somewhere
            // else now serves whatever that scheduler leads.
            round.lease.fleet.Pin(registrar.ClusterId());
            ++accepted;
            ++registrations;
        }
        else
        {
            if (!leader.has_value())
                leader = registered.error().leader;
            round.logger.Logf(LogLevel::Warn,
                              "scheduler {} did not register {}: {}",
                              endpoint,
                              registrar.Fingerprint(),
                              registered.error().reason);
        }
    }
    if (handedOver && !outbox.empty())
        round.sampler.HistoryHandedThrough(outbox.back().startMillis);

    // What this round DID, and how loudly to say it -- see `DescribeAnnounceRound`,
    // which owns both because the wording is the defect it was written for (#999) and
    // `main.cpp` is in no test target (#909). The shortfall-behind-a-leader rule it
    // carries is the one that used to live here: nothing is wrong with a fleet that has
    // just elected, and the caller follows the redirect inside this same round.
    auto const report = DescribeAnnounceRound(beats, registrations, round.registrars.size(), leader.has_value());
    round.logger.Logf(report.level, "scheduler {}: {}", endpoint, report.message);
    return AnnounceOutcome { .accepted = accepted, .leader = std::move(leader) };
}

} // namespace FastCache::Node

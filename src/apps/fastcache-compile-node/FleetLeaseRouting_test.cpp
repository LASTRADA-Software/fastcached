// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CompileCorrelation.hpp>
#include <Dispatch.hpp>
#include <tests/FleetHarness.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace
{
namespace Wire = CompileCacheWire;

constexpr std::string_view SchedulerA = "sched-a:6676";
constexpr std::string_view SchedulerB = "sched-b:6676";
constexpr std::string_view Worker = "worker-1:6677";
/// A learner: runs consensus and no scheduler, so its `0xFC` port redirects the scheduling verbs.
constexpr std::string_view Learner = "learner-c:6674";
constexpr std::string_view Toolchain = "gcc-14-x86_64";
constexpr std::string_view Key = "obj-abcdef";

/// The verb byte of one logged exchange, so a test can name verbs rather than bytes.
/// @param op The verb.
/// @return Its wire byte.
[[nodiscard]] std::uint8_t Raw(Wire::Op op) noexcept
{
    return static_cast<std::uint8_t>(op);
}

/// A dispatch request for `Key` against `Toolchain`, pointed at @p scheduler.
/// @param scheduler The `--scheduler` endpoint this client was configured with.
/// @return The request.
[[nodiscard]] Cc::DispatchRequest RequestVia(std::string_view scheduler)
{
    return Cc::DispatchRequest { .schedulerEndpoint = scheduler,
                                 .fingerprint = Toolchain,
                                 .objectKey = Key,
                                 .args = {},
                                 .family = Cc::DriverFamily::Gnu,
                                 .preprocessed = "int main() { return 0; }",
                                 .sourceName = "main.cpp",
                                 .compileDir = {},
                                 .compileDirReplacement = {},
                                 .sourceRoot = {},
                                 .sourceRootReplacement = {} };
}

/// Ask @p scheduler for a lease on `Key`, and return what it answered.
/// @param fleet The fleet.
/// @param scheduler Who to ask.
/// @return The exchange's outcome, hit or refusal.
[[nodiscard]] Cc::CacheOutcome AskForLease(Testing::FleetHarness& fleet, std::string_view scheduler)
{
    return fleet.Exchange(
        scheduler,
        Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = Toolchain, .key = Key, .acceptedCodecs = {} }),
        Cc::Credential {},
        Cc::ExchangeBudget {});
}

/// Take a lease for `Key` from @p scheduler the way a second client would.
///
/// Through the harness's own exchange rather than by calling `SchedulerService`
/// directly, so this second client frames its request exactly as the first one
/// does — the collision the case is about is between two *clients*, and reaching
/// past the wire to arrange it would be arranging something else.
/// @param fleet The fleet.
/// @param scheduler Who to ask.
/// @return The granted token.
[[nodiscard]] std::string LeaseFrom(Testing::FleetHarness& fleet, std::string_view scheduler)
{
    auto const outcome = AskForLease(fleet, scheduler);
    REQUIRE(outcome.IsHit());
    auto const grant = Wire::DecodeLeaseGrant(outcome.value);
    REQUIRE(grant.has_value());
    return std::string { Wire::AsStringView(Unwrap(grant).leaseToken) };
}

/// Put a node into @p fleet the way `WorkerBody` would: registered as a worker exactly
/// when `WorkerSlotsOf` gives it a slot count, and not at all otherwise.
///
/// Through the function `WorkerBody` decides with rather than a copy of its condition,
/// because the case is about that decision: a node whose count is absent starts no
/// heartbeat thread, so nothing ever registers it (#206).
/// @param fleet The fleet.
/// @param endpoint Where the node answers.
/// @param cfg Its configuration.
/// @param capacity What the machine is.
void JoinAsConfigured(Testing::FleetHarness& fleet,
                      std::string_view endpoint,
                      Node::NodeConfig const& cfg,
                      Distributed::NodeCapacity const& capacity)
{
    if (auto const slots = Node::WorkerSlotsOf(cfg, capacity))
        fleet.RegisterWorker(SchedulerA, endpoint, Toolchain, *slots);
}

/// The reply an honest worker sends for @p request's job: an object, and the correlation the
/// launcher recomputes, signed under the key every grant here names -- so `Compiled` means the
/// compile was ACCEPTED, not merely that some address answered.
/// @param request The job the launcher dispatches.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> CompiledReply(Cc::DispatchRequest const& request)
{
    constexpr std::string_view Object = "OBJ";
    auto const correlation =
        Cc::CompileCorrelation(Cc::CorrelatedCompile { .preprocessed = request.preprocessed,
                                                       .args = request.args,
                                                       .fingerprint = request.fingerprint,
                                                       .sourceName = request.sourceName,
                                                       .compileDir = request.compileDir,
                                                       .compileDirReplacement = request.compileDirReplacement,
                                                       .sourceRoot = request.sourceRoot,
                                                       .sourceRootReplacement = request.sourceRootReplacement });
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(Object.size()), Wire::AsBytes(Object));
    return Testing::FleetHarness::SignedWorkerReply(enveloped, correlation);
}

/// Every counter's reading -- EVERY counter, so one that moved and was not expected is visible.
/// @param metrics The sink.
/// @return The readings.
[[nodiscard]] std::map<IMetricsSink::Counter, std::uint64_t> Readings(IMetricsSink const& metrics)
{
    std::map<IMetricsSink::Counter, std::uint64_t> readings;
    for (auto const counter: Enumerators<IMetricsSink::Counter>())
        readings.emplace(counter, metrics.Read(counter));
    return readings;
}

/// Prove @p fleet's learner counts into the sink `LearnerMetrics` returns, through the surface a
/// launcher reaches: a `MINT-TICKET` from another machine is refused by the session component
/// beside the redirect, COUNTED. Without it a sink nothing writes would read "no counter moved"
/// whatever the redirect did.
/// @param fleet The fleet; its caller is left on loopback, as it was found.
void ShowLearnerSinkIsLive(Testing::FleetHarness& fleet)
{
    auto const& metrics = fleet.LearnerMetrics(Learner);
    auto const before = metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedNotLocal);
    fleet.SetCallerHost("192.0.2.7");
    auto const minted = fleet.Exchange(Learner,
                                       Wire::Detail::EncodeRequest(Wire::CurrentVersion, Wire::Op::MintTicket, {}),
                                       Cc::Credential {},
                                       Cc::ExchangeBudget {});
    fleet.SetCallerHost("127.0.0.1");
    REQUIRE(minted.kind == Cc::CacheOutcomeKind::Rejected);
    REQUIRE(minted.code == Wire::ErrorCode::NotAMember);
    REQUIRE(metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedNotLocal) == before + 1);
}

/// The calls logged from @p from on that went to @p endpoint.
/// @param fleet The fleet.
/// @param from How many calls to skip.
/// @param endpoint Who was asked.
/// @return How many.
[[nodiscard]] std::ptrdiff_t CallsTo(Testing::FleetHarness const& fleet, std::size_t from, std::string_view endpoint)
{
    return std::ranges::count(std::span { fleet.Calls() }.subspan(from), endpoint, &Testing::FleetHarness::Call::endpoint);
}

} // namespace

TEST_CASE("A release goes to the scheduler that issued the lease, not to the configured one", "[node][fleet]")
{
    // **The interleaving is the whole case.** Every part of this is asserted
    // somewhere in isolation already -- `LeaseTable` resolves a token, `Gate()`
    // refuses a follower, `SchedulerLink` follows a redirect -- and none of that
    // reaches the rule, which is about a SEQUENCE across two machines: a lease
    // granted by one scheduler, leadership moving, and the release still going to
    // the machine that actually holds the lease.
    //
    // Leases are per-scheduler state. They are not replicated, so a release sent to
    // the wrong one does not merely fail to resolve anything -- it can resolve
    // SOMEBODY ELSE'S, because two schedulers number their leases independently and
    // both start at one. That is the harm, and it is why this is a safety property
    // rather than a tidiness one.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddScheduler(std::string { SchedulerB });

    // Each scheduler has to be leading to accept a registration -- `Gate()` runs for
    // every verb -- so the fleet is built one leadership at a time. Both end up
    // knowing the same worker, which is what an election between them looks like.
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);
    fleet.ElectLeader(SchedulerB);
    fleet.RegisterWorker(SchedulerB, Worker, Toolchain);

    // Between the grant and the release: leadership moves back to A, and a second
    // client takes A's own lease on the SAME key. A is now the leader, so this is
    // an ordinary thing for a second client to do -- and A's lease table, being
    // A's, hands out the same first token B did.
    std::string secondToken;
    fleet.OnCompile([&] {
        fleet.ElectLeader(SchedulerA);
        secondToken = LeaseFrom(fleet, SchedulerA);
    });

    // The first client, configured with A, is redirected to B and leases there.
    auto const request = RequestVia(SchedulerA);
    auto const result = Cc::Dispatch(fleet, request, Cc::DispatchBudgets {}, Cc::Credential {}, {});
    CHECK(result.status == Cc::DispatchStatus::Declined); // the harness worker refuses; the release still happens

    // The two clients really did collide on a token, or the case below would pass
    // for the wrong reason -- a release to the wrong scheduler is only *harmful*
    // when the number means something there too.
    CHECK_FALSE(secondToken.empty());

    // The release went to the issuer.
    auto const& calls = fleet.Calls();
    REQUIRE_FALSE(calls.empty());
    auto const& last = calls.back();
    CHECK(last.opRaw == Raw(Wire::Op::Release));
    CHECK(last.endpoint == SchedulerB);

    // And the consequence, which is the part worth having: the second client's
    // lease is INTACT. Sent to the configured endpoint instead, this release would
    // have matched -- both tables number from one and both leases name the same key
    // -- and freed a key another client is still building, on the machine that would
    // then hand it to a third.
    CHECK(fleet.IsInFlight(SchedulerA, Key));

    // **And the release SETTLED, which is #371.** The issuer is a follower by now,
    // and this used to be refused `NotLeader` -- so the key stayed pinned on the one
    // machine that could free it until it expired, while the client had done exactly
    // the right thing. A release is not a scheduling decision: it resolves a lease
    // this node minted, in a table nobody else holds a copy of.
    //
    // These three lines were written the other way round when the harness first found
    // this, asserting the refusal and pointing at #371. They flip together, which is
    // the point of having pinned the wrong behaviour explicitly rather than leaving it
    // as a silent surprise.
    CHECK(last.kind == Cc::CacheOutcomeKind::Hit);
    CHECK_FALSE(fleet.IsInFlight(SchedulerB, Key));
}

TEST_CASE("A demoted scheduler settles its own lease and still refuses one it never issued", "[node][fleet]")
{
    // The other half of #371's acceptance, and the half that turns a fix into a hole
    // if it is missing. Letting a release through after demotion must not mean letting
    // ANY release through: a token this node never issued resolves nothing, and saying
    // so is the only place "this job outlived its lease" can be observed.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddScheduler(std::string { SchedulerB });

    fleet.ElectLeader(SchedulerB);
    fleet.RegisterWorker(SchedulerB, Worker, Toolchain);
    auto const token = LeaseFrom(fleet, SchedulerB);
    REQUIRE(fleet.IsInFlight(SchedulerB, Key));

    // B is demoted with the lease still outstanding.
    fleet.ElectLeader(SchedulerA);

    // A token B never minted is refused, by name, and does not disturb the real one. Every
    // grant is signed since #178, so one B never signed fails authentication before B's table
    // is read.
    auto const bogus = fleet.Exchange(SchedulerB,
                                      Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = "not-a-token", .key = Key }),
                                      Cc::Credential {},
                                      Cc::ExchangeBudget {});
    CHECK(bogus.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(bogus.code == Wire::ErrorCode::LeaseUnauthorized);
    CHECK(fleet.IsInFlight(SchedulerB, Key));

    // The right token against the WRONG key is refused too -- the token names its key and
    // `LeaseTable` matches on both, so a release cannot free a key it does not name. The
    // token's own key is the check that answers first, and it answers as an unauthorised
    // release (#323).
    auto const wrongKey =
        fleet.Exchange(SchedulerB,
                       Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = token, .key = "obj-somebody-else" }),
                       Cc::Credential {},
                       Cc::ExchangeBudget {});
    CHECK(wrongKey.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(wrongKey.code == Wire::ErrorCode::LeaseUnauthorized);
    CHECK(fleet.IsInFlight(SchedulerB, Key));

    // And the genuine one settles, from a node that is no longer the leader.
    auto const real = fleet.Exchange(SchedulerB,
                                     Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = token, .key = Key }),
                                     Cc::Credential {},
                                     Cc::ExchangeBudget {});
    CHECK(real.kind == Cc::CacheOutcomeKind::Hit);
    CHECK_FALSE(fleet.IsInFlight(SchedulerB, Key));

    // Releasing it a second time is refused rather than silently accepted: the entry
    // is gone, and a client told nothing has nothing to report.
    auto const again = fleet.Exchange(SchedulerB,
                                      Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = token, .key = Key }),
                                      Cc::Credential {},
                                      Cc::ExchangeBudget {});
    CHECK(again.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(again.code == Wire::ErrorCode::UnknownLease);
}

TEST_CASE("A lease taken from the configured leader is released back to it", "[node][fleet]")
{
    // The straight case, so the one above cannot pass by accident: with no redirect
    // and no election, the issuer and the configured endpoint are the same machine,
    // and a client that always released to whoever granted would look identical to
    // one that always released to its configured address. Only the redirect tells
    // them apart -- which is the point, and the reason both cases are here.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);

    auto const request = RequestVia(SchedulerA);
    (void) Cc::Dispatch(fleet, request, Cc::DispatchBudgets {}, Cc::Credential {}, {});

    auto const& calls = fleet.Calls();
    REQUIRE(calls.size() == 3); // LEASE, COMPILE, RELEASE -- no redirect hop
    CHECK(calls.front().endpoint == SchedulerA);
    // The compile goes to the worker the lease NAMES, never back to the scheduler that granted it:
    // the e2e runs both on one node, so a client dialling the scheduler instead would pass there.
    CHECK(calls[1].opRaw == Raw(Wire::Op::Compile));
    CHECK(calls[1].endpoint == Worker);
    CHECK(calls.back().opRaw == Raw(Wire::Op::Release));
    CHECK(calls.back().endpoint == SchedulerA);
    CHECK_FALSE(fleet.IsInFlight(SchedulerA, Key));
}

TEST_CASE("A lease outliving its holder stops suppressing its key once time moves", "[node][fleet]")
{
    // Expiry is the third of a lease's three transitions and the only one no client
    // performs, so it is the one an in-process fleet can assert without a process
    // that has to actually die. Time moves in `Step` and nowhere else.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);

    auto const token = LeaseFrom(fleet, SchedulerA);
    CHECK_FALSE(token.empty());
    CHECK(fleet.IsInFlight(SchedulerA, Key));

    // Nothing releases it -- this is the client that died.
    fleet.Step(Distributed::LeaseTable::DefaultLeaseTimeout + std::chrono::seconds { 1 });
    CHECK_FALSE(fleet.IsInFlight(SchedulerA, Key));
}

TEST_CASE("A node running no worker is never leased, and a worker beside it still is", "[node][fleet]")
{
    // #206, both directions. The scheduler-only machine is the one an operator excluded
    // from the work, and it shares the toolchain fingerprint with the worker beside it --
    // which is the NORMAL case, and the one the old fake-toolchain trick could not cover.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);

    Node::NodeConfig schedulerOnly;
    schedulerOnly.slots = 0;
    // A small always-on box, so that if it WERE registered it would still lose every
    // pick to the worker below -- which is what keeps the second section from going red
    // for the first section's reason.
    Distributed::NodeCapacity const smallBox { .logicalCores = 2 };

    Node::NodeConfig worker;
    Distributed::NodeCapacity const buildServer { .logicalCores = 16, .nodeClass = Distributed::NodeClass::Dedicated };

    SECTION("alone, the fleet has no worker for the toolchain")
    {
        JoinAsConfigured(fleet, SchedulerA, schedulerOnly, smallBox);

        auto const outcome = AskForLease(fleet, SchedulerA);
        REQUIRE_FALSE(outcome.IsHit());
        // `NoWorker` -- *the fleet has none* -- and not `NoCapacity` or `Withdrawn`, which
        // is what a registered node offering zero, or a cordoned one, would be answered.
        CHECK(outcome.code == Wire::ErrorCode::NoWorker);
    }

    SECTION("beside a worker, the worker takes the lease")
    {
        JoinAsConfigured(fleet, SchedulerA, schedulerOnly, smallBox);
        JoinAsConfigured(fleet, Worker, worker, buildServer);

        auto const outcome = AskForLease(fleet, SchedulerA);
        REQUIRE(outcome.IsHit());
        auto const grant = Wire::DecodeLeaseGrant(outcome.value);
        REQUIRE(grant.has_value());
        CHECK(Wire::AsStringView(Unwrap(grant).endpoint) == Worker);
    }
}

TEST_CASE("A grant the client reads carries where the worker was last seen, beside the name it advertises",
          "[node][fleet][dialhint]")
{
    // The laptop advertises a DNS name, and its VPN reconnected: the scheduler saw the new
    // address on the heartbeat, and the grant the launcher's own framing decodes names it as
    // the hint while the endpoint -- what the token signs -- stays the name.
    constexpr std::string_view Laptop = "laptop.corp:6677";
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);
    auto const id = fleet.RegisterWorkerNamed(SchedulerA, Laptop, Toolchain);

    // Registered through the harness's loopback setup caller, so no hint yet: loopback is
    // vetoed, which is also the control that the hint below comes from the heartbeat.
    auto const unseen = AskForLease(fleet, SchedulerA);
    REQUIRE(unseen.IsHit());
    auto const before = Wire::DecodeLeaseGrant(unseen.value);
    REQUIRE(before.has_value());
    CHECK(Unwrap(before).dialHint.empty());
    REQUIRE(fleet.Calls().back().endpoint == SchedulerA);

    fleet.HeartbeatFrom(SchedulerA, id, "10.8.0.42", { "10.8.0.42" });
    auto const outcome = fleet.Exchange(
        SchedulerA,
        Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = Toolchain, .key = "obj-second", .acceptedCodecs = {} }),
        Cc::Credential {},
        Cc::ExchangeBudget {});
    REQUIRE(outcome.IsHit());
    CHECK(fleet.Calls().back().endpoint == SchedulerA);
    auto const grant = Wire::DecodeLeaseGrant(outcome.value);
    REQUIRE(grant.has_value());
    CHECK(Wire::AsStringView(Unwrap(grant).endpoint) == Laptop);
    CHECK(Wire::AsStringView(Unwrap(grant).dialHint) == "10.8.0.42:6677");
}

TEST_CASE("A launcher pointed at its own learner is redirected to the leader and releases there",
          "[node][fleet][scheduling-redirect]")
{
    // #1639 end to end: a launcher whose `--scheduler` is its own machine, and that machine a
    // learner -- consensus, no scheduler. The learner's port answers LEASE `NotLeader` naming the
    // leader's scheduling endpoint, the launcher follows it, compiles on the worker the leader
    // granted, and releases to the leader that ISSUED the lease. Every object on both sides is
    // production's: the redirect behind the merged router, and the launcher's own `Cc::Dispatch`.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddLearner(std::string { Learner });
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);
    auto const request = RequestVia(Learner);
    fleet.SetWorkerReply(CompiledReply(request));

    ShowLearnerSinkIsLive(fleet);
    auto const counted = Readings(fleet.LearnerMetrics(Learner));
    auto const from = fleet.Calls().size();

    auto const result = Cc::Dispatch(fleet, request, Cc::DispatchBudgets {}, Cc::Credential {}, {});

    CHECK(result.status == Cc::DispatchStatus::Compiled);
    CHECK(result.workerEndpoint == Worker);
    CHECK(result.leaseEndpoint == SchedulerA);

    auto const calls = std::span { fleet.Calls() }.subspan(from);
    REQUIRE(calls.size() == 4); // LEASE@learner, LEASE@leader, COMPILE@worker, RELEASE@leader

    // The learner refused with the REDIRECT -- never `UnimplementedVerb`, which a launcher reads as
    // *this node's build is too old* and stops asking.
    CHECK(calls[0].endpoint == Learner);
    CHECK(calls[0].opRaw == Raw(Wire::Op::Lease));
    CHECK(calls[0].kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(calls[0].code == Wire::ErrorCode::NotLeader);
    CHECK(calls[0].code != Wire::UnimplementedVerb);

    CHECK(calls[1].endpoint == SchedulerA);
    CHECK(calls[1].opRaw == Raw(Wire::Op::Lease));
    CHECK(calls[1].kind == Cc::CacheOutcomeKind::Hit);

    CHECK(calls[2].endpoint == Worker);
    CHECK(calls[2].opRaw == Raw(Wire::Op::Compile));
    CHECK(calls[2].kind == Cc::CacheOutcomeKind::Hit);

    // The release goes to the ISSUER: the learner granted nothing, and would refuse it.
    CHECK(calls[3].endpoint == SchedulerA);
    CHECK(calls[3].opRaw == Raw(Wire::Op::Release));
    CHECK(calls[3].kind == Cc::CacheOutcomeKind::Hit);
    CHECK_FALSE(fleet.IsInFlight(SchedulerA, Key));

    // The learner was asked once, and only once: after the redirect it is out of the exchange.
    CHECK(CallsTo(fleet, from + 1, Learner) == 0);

    // And it counted nothing doing it: a redirect is every launcher's ordinary traffic, spelled
    // `RefuseWithoutCounter`, on a sink just shown to be the one this surface counts into.
    CHECK(Readings(fleet.LearnerMetrics(Learner)) == counted);
}

TEST_CASE("A launcher pointed at a learner that knows no leader compiles locally, after one call",
          "[node][fleet][scheduling-redirect]")
{
    // The companion, and the control the case above needs: here nothing was elected, so the learner
    // has heard of no leader and says `NotLeader` naming nobody -- what a follower says during an
    // election. A redirect naming nothing is not followed: the lease is declined `NoLeader` and the
    // launcher compiles locally, having asked exactly one machine. A learner that named a leader it
    // had not heard of, or a launcher that dialled an empty endpoint, is red here and green above.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddLearner(std::string { Learner });

    ShowLearnerSinkIsLive(fleet);
    auto const counted = Readings(fleet.LearnerMetrics(Learner));
    auto const from = fleet.Calls().size();

    auto const result = Cc::Dispatch(fleet, RequestVia(Learner), Cc::DispatchBudgets {}, Cc::Credential {}, {});

    CHECK(result.status == Cc::DispatchStatus::Declined);
    CHECK(result.decline == Cc::DeclineCause::NoLeader);

    auto const calls = std::span { fleet.Calls() }.subspan(from);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].endpoint == Learner);
    CHECK(calls[0].opRaw == Raw(Wire::Op::Lease));
    CHECK(calls[0].kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(calls[0].code == Wire::ErrorCode::NotLeader);
    CHECK(calls[0].code != Wire::UnimplementedVerb);
    CHECK(Readings(fleet.LearnerMetrics(Learner)) == counted);
}

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FleetHistoryFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A caller the fleet has admitted.
constexpr CallerContext Insider { .membership = Membership::Member, .peerId = "peer-1" };

/// A caller that holds no membership -- the leech this policy exists for.
constexpr CallerContext Outsider { .membership = Membership::Outsider, .peerId = "stranger" };

/// A worker offering one slot of the given toolchain.
[[nodiscard]] WorkerRegistration OneSlot(std::string_view fingerprint, std::string_view endpoint)
{
    return WorkerRegistration { .fingerprint = fingerprint, .endpoint = endpoint, .slots = 1, .codecs = {} };
}

/// A lease request for one key against one toolchain.
[[nodiscard]] Wire::LeaseRequest Ask(std::string_view fingerprint, std::string_view key)
{
    return Wire::LeaseRequest { .fingerprint = fingerprint, .key = key, .acceptedCodecs = {} };
}

/// The token out of a granted lease's payload.
/// @param reply What `Lease` answered.
/// @return The token the client would present.
[[nodiscard]] std::string TokenOf(SchedulerReply const& reply)
{
    auto const grant = Wire::DecodeLeaseGrant(reply.payload);
    REQUIRE(grant.has_value());
    return std::string { Wire::AsStringView(Unwrap(grant).leaseToken) };
}

/// A service that already leads, which is the precondition of every verb.
struct Leading
{
    Leading()
    {
        service.SetRole(SchedulerRole::Leader, {}, StandaloneSchedulerTerm);
    }

    ManualClock clock;
    AtomicMetricsSink metrics;

    /// Capturing rather than null, so the one line this service writes is readable
    /// from any case that cares and costs the rest nothing.
    CapturingLogger logger;
    ManualWallClock wallClock;
    SchedulerService service { clock, wallClock, metrics, logger, {}, {} };
};

/// A fixed wall-clock instant, so a grant's expiry is a value a test can name.
constexpr std::chrono::system_clock::time_point Noon { 1'767'225'600s };

/// The same, with a cluster key, so its grants are signed.
///
/// A separate fixture rather than a parameter on `Leading`, because unsigned is not
/// a variation of signed: it is the boundary this surface had before, kept working
/// for the single machine that runs no cluster, and the cases about it assert a
/// warning that a signing fleet must never write.
struct Signing
{
    Signing()
    {
        service.SetRole(SchedulerRole::Leader, {}, StandaloneSchedulerTerm);
    }

    /// The fleet this scheduler leads.
    ///
    /// Named rather than empty so the grants it mints carry a real cluster id: empty
    /// on both sides of the comparison passes whether the check runs or not (#322).
    static constexpr std::string_view TestCluster = "fleet-under-test";

    /// Thirty-two bytes, as `--cluster-key-file` would supply them.
    std::vector<std::byte> key = std::vector<std::byte>(32, std::byte { 0x5A });

    ManualClock clock;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    ManualWallClock wallClock;
    SchedulerService service { clock, wallClock, metrics, logger, key, TestCluster };
};
} // namespace

TEST_CASE("A granted lease reaches the fleet page's compiling figure", "[distributed][scheduler][fleetview]")
{
    // A WIRING assertion, in the sense `.agent/rules/wire-and-protocol.md` means it,
    // and it is here because its absence is what let #215 stand.
    //
    // Every capacity case in `FleetView_test.cpp` assigns `fleetJobsInFlight` onto a
    // snapshot literal, and every registry case calls `JobStarted` by hand. Neither
    // can fail for a `compiling` figure that production never increments: nothing
    // anywhere ran `Lease -> WorkerRegistry -> NodeReports -> TotalsFor`, so a
    // counter that had come unwired from either end would have stayed green.
    //
    // Asserted through `CollectFleet` rather than on the registry, because the page
    // is the consumer that was wrong -- and on the rendered HTML too, since a total
    // that never reaches the markup is a total nobody reads.
    Leading fleet;
    (void) fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100"));

    auto const before =
        CollectFleet(FleetSources { .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    CHECK(TotalsFor(before).inFlight == 0);

    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "key-1"));
    REQUIRE(granted.status == Wire::Status::Ok);

    auto const after =
        CollectFleet(FleetSources { .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    CHECK(TotalsFor(after).inFlight == 1);
    CHECK(RenderFleetHtml(after, FleetHistoryView {}, 0).contains("<b>1</b> <span>compiling"));
}

TEST_CASE("A granted lease reaches the fleet page as a row, with its holder's address",
          "[distributed][scheduler][fleetview]")
{
    // The wiring assertion for #142, in the sense the rulebook means it. Every case
    // in `FleetView_test.cpp` assigns `outstandingLeases` onto a snapshot literal,
    // so a listing that had come unwired at either end -- the scheduler never asked,
    // or the endpoint join never joined -- would leave all of them green.
    //
    // The endpoint is the half worth pinning: `LeaseTable` knows only the worker id,
    // and the address an operator goes and looks at is joined in from the registry.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "stuck-key")).status == Wire::Status::Ok);
    fleet.clock.Advance(2s);

    auto const snapshot =
        CollectFleet(FleetSources { .scheduler = &fleet.service, .cluster = nullptr, .metrics = &fleet.metrics });
    REQUIRE(snapshot.outstandingLeases.size() == 1);
    CHECK(snapshot.outstandingLeases.front().key == "stuck-key");
    CHECK(snapshot.outstandingLeases.front().workerEndpoint == "10.0.0.2:7100");
    CHECK(snapshot.outstandingLeases.front().age == 2s);
    CHECK(snapshot.liveLeases == 1);

    // And it reaches the markup, because a row that never renders is a row nobody
    // reads.
    CHECK(RenderFleetHtml(snapshot, FleetHistoryView {}, 0).contains("stuck-key"));
}

TEST_CASE("Only the leader hands out capacity", "[distributed][scheduler]")
{
    // A follower's registry is a stale copy of somebody else's, so admitting a
    // worker here would put it in a fleet nothing schedules onto -- and it would
    // heartbeat into that void forever. The refusal carries the leader's address so
    // the caller can redirect instead.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    SchedulerService service { clock, wallClock, metrics, schedulerLogger, {}, {} };

    SECTION("an undecided node refuses, and names nobody")
    {
        // The default: no election has concluded. Distinct from a follower only in
        // that there is no address to redirect to, which is why the message is the
        // carrier rather than a second error code.
        REQUIRE(service.Role() == SchedulerRole::Undecided);
        auto const reply = service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100"));
        CHECK(reply.status == Wire::Status::Error);
        CHECK(reply.error == Wire::ErrorCode::NotLeader);
        CHECK(reply.message.empty());
    }

    SECTION("a follower refuses and redirects")
    {
        service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000", StandaloneSchedulerTerm);
        auto const reply = service.Lease(Insider, Ask("gcc-14", "abc"));
        CHECK(reply.status == Wire::Status::Error);
        CHECK(reply.error == Wire::ErrorCode::NotLeader);
        CHECK(reply.message == "10.0.0.1:7000");
    }

    SECTION("leadership gates every verb that decides something")
    {
        // The rule is on the gate rather than on each handler, so a verb added
        // without thinking about it is refused rather than served. Asserted per
        // verb because "I added a handler that skips the gate" is exactly the
        // regression this arrangement exists to make impossible.
        service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000", StandaloneSchedulerTerm);
        CHECK(service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).error == Wire::ErrorCode::NotLeader);
        CHECK(service.Heartbeat(Insider, "whoever", NodeLoad {}).error == Wire::ErrorCode::NotLeader);
        CHECK(service.Lease(Insider, Ask("gcc-14", "abc")).error == Wire::ErrorCode::NotLeader);
    }

    SECTION("and does not gate the one verb that settles rather than decides")
    {
        // This section used to assert the opposite, under a comment reading
        // "resolving is a write against the LEADER's own lease table, so a follower
        // has nothing to resolve". The second half of that is what #371 disproved: a
        // follower that WAS the leader holds every lease it granted, in a table that
        // is never replicated, and it is the only node that can free them. Refusing
        // here pinned the key until it expired.
        //
        // `NotLeader` is what must not come back. `UnknownLease` is the right answer
        // for this particular token, because this service never granted `l1` -- and
        // that is asserted rather than skipped past, since "the release got through"
        // and "the release resolved something" are the two halves that must not be
        // confused.
        service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000", StandaloneSchedulerTerm);
        auto const reply = service.Release(Insider, "l1", "abc");
        CHECK(reply.error != Wire::ErrorCode::NotLeader);
        CHECK(reply.error == Wire::ErrorCode::UnknownLease);
    }
}

TEST_CASE("A non-member is refused the fleet but not the cache", "[distributed][scheduler]")
{
    // The anti-leeching rule. What it refuses is CPU time; the cache is a separate
    // service this class cannot reach, so a non-member's FETCH and STORE are
    // untouched by anything here -- which is the whole design, and is why the
    // refusal is its own code rather than `Unauthenticated`.
    Leading fleet;

    CHECK(fleet.service.Register(Outsider, OneSlot("gcc-14", "10.0.0.2:7100")).error == Wire::ErrorCode::NotAMember);
    CHECK(fleet.service.Heartbeat(Outsider, "whoever", NodeLoad {}).error == Wire::ErrorCode::NotAMember);
    CHECK(fleet.service.Lease(Outsider, Ask("gcc-14", "abc")).error == Wire::ErrorCode::NotAMember);
    CHECK(fleet.service.Release(Outsider, "l1", "abc").error == Wire::ErrorCode::NotAMember);

    // And nothing was admitted along the way: a refused registration must not leave
    // the worker in the registry, or the next Lease would hand out an endpoint the
    // policy just declined.
    CHECK(fleet.service.Workers().LiveWorkers().empty());
}

TEST_CASE("Membership is checked after leadership", "[distributed][scheduler]")
{
    // Order matters for the *diagnostic*, not for the outcome. A follower cannot
    // know the cluster's membership any better than it knows the fleet, so
    // answering `NotAMember` there would send an operator looking at a policy that
    // was never consulted. Cheapest and most certain fact first.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger schedulerLogger;
    ManualWallClock wallClock;
    SchedulerService service { clock, wallClock, metrics, schedulerLogger, {}, {} };
    service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000", StandaloneSchedulerTerm);

    CHECK(service.Lease(Outsider, Ask("gcc-14", "abc")).error == Wire::ErrorCode::NotLeader);

    // A release is where the order becomes visible rather than cosmetic. It skips the
    // leadership rule entirely (#371), so a non-member asking a demoted node to settle
    // a lease is told the thing that actually applies to it -- and membership is the
    // half that is never relaxed, whoever leads.
    CHECK(service.Release(Outsider, "l1", "abc").error == Wire::ErrorCode::NotAMember);
}

TEST_CASE("A member registers, heartbeats and is leased", "[distributed][scheduler]")
{
    Leading fleet;

    auto const admitted = fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100"));
    REQUIRE(admitted.status == Wire::Status::Ok);
    REQUIRE_FALSE(admitted.payload.empty());
    auto const workerId = std::string { Wire::AsStringView(admitted.payload) };

    CHECK(fleet.service.Heartbeat(Insider, workerId, NodeLoad {}).status == Wire::Status::Ok);

    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "key-1"));
    REQUIRE(granted.status == Wire::Status::Ok);
    auto const grant = Wire::DecodeLeaseGrant(granted.payload);
    REQUIRE(grant.has_value());
    CHECK(Wire::AsStringView(Unwrap(grant).endpoint) == "10.0.0.2:7100");
    CHECK_FALSE(Unwrap(grant).leaseToken.empty());
}

TEST_CASE("A heartbeat reply states the term this scheduler is leading under", "[distributed][scheduler][epoch]")
{
    // The scheduler half of #421, and the assertion is that the reply carries THE
    // TERM THIS SERVICE IS IN -- not that the payload decodes, which a round trip
    // against a literal would also satisfy. So the role is set to a term nothing else
    // in this file uses and the reply is read back through the production decoder.
    Leading fleet;
    fleet.service.SetRole(SchedulerRole::Leader, {}, 23);

    auto const admitted = fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100"));
    REQUIRE(admitted.status == Wire::Status::Ok);
    auto const workerId = std::string { Wire::AsStringView(admitted.payload) };

    auto const beat = fleet.service.Heartbeat(Insider, workerId, NodeLoad {});
    REQUIRE(beat.status == Wire::Status::Ok);

    auto const stated = Wire::DecodeSchedulerTerm(beat.payload);
    CHECK(stated.state == Wire::SchedulerTermState::Stated);
    CHECK(stated.term == 23);

    // And it FOLLOWS the role rather than being stamped once: an election is the only
    // event that moves this, and a worker holding the pre-election term is exactly
    // what the channel exists to correct.
    fleet.service.SetRole(SchedulerRole::Leader, {}, 24);
    auto const later = fleet.service.Heartbeat(Insider, workerId, NodeLoad {});
    REQUIRE(later.status == Wire::Status::Ok);
    CHECK(Wire::DecodeSchedulerTerm(later.payload).term == 24);
}

TEST_CASE("A heartbeat's history is routed under the machine, not the worker id", "[distributed][scheduler][fleethistory]")
{
    // A machine with two `--toolchain` flags registers twice and heartbeats twice
    // with the same figures. Keyed per worker id the fleet would hold one machine's
    // series once per toolchain and sum it that many times -- the rule
    // `WorkerRegistry::NodeCaches()` already exists to enforce on the neighbouring
    // number.
    Leading fleet;
    Testing::RecordingHistorySink sink;
    fleet.service.SetHistorySink(&sink);

    auto const first = fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100"));
    auto const second = fleet.service.Register(Insider, OneSlot("clang-20", "10.0.0.2:7100"));
    REQUIRE(first.status == Wire::Status::Ok);
    REQUIRE(second.status == Wire::Status::Ok);

    auto const batch = std::array { Testing::ClosedBucket(60'000), Testing::ClosedBucket(120'000) };
    for (auto const& reply: { first, second })
        CHECK(fleet.service.Heartbeat(Insider, std::string { Wire::AsStringView(reply.payload) }, NodeLoad {}, batch).status
              == Wire::Status::Ok);

    REQUIRE(sink.calls.size() == 2);
    for (auto const& call: sink.calls)
    {
        CHECK(call.endpoint == "10.0.0.2:7100");
        CHECK(call.buckets.size() == 2);
    }
}

TEST_CASE("History from a worker the scheduler does not know is not routed", "[distributed][scheduler][fleethistory]")
{
    // The refusal is what tells the node to register again. Taking the readings
    // anyway would file them under an endpoint this scheduler cannot name, because
    // the endpoint comes from the registry entry that is missing.
    Leading fleet;
    Testing::RecordingHistorySink sink;
    fleet.service.SetHistorySink(&sink);

    auto const batch = std::array { Testing::ClosedBucket(60'000) };
    CHECK(fleet.service.Heartbeat(Insider, "worker-that-never-was", NodeLoad {}, batch).error
          == Wire::ErrorCode::UnknownLease);
    CHECK(sink.calls.empty());

    // And a heartbeat carrying nothing reaches no sink at all, so an empty batch is
    // not a machine reporting an empty window.
    auto const admitted = fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.3:7100"));
    REQUIRE(admitted.status == Wire::Status::Ok);
    CHECK(fleet.service.Heartbeat(Insider, std::string { Wire::AsStringView(admitted.payload) }, NodeLoad {}).status
          == Wire::Status::Ok);
    CHECK(sink.calls.empty());
}

TEST_CASE("A scheduler with no history sink still heartbeats", "[distributed][scheduler][fleethistory]")
{
    // The sink is wiring the admin surface installs, and a node that runs no
    // dashboard installs none. The verb must not depend on it.
    Leading fleet;
    auto const admitted = fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.4:7100"));
    REQUIRE(admitted.status == Wire::Status::Ok);

    auto const batch = std::array { Testing::ClosedBucket(60'000) };
    CHECK(fleet.service.Heartbeat(Insider, std::string { Wire::AsStringView(admitted.payload) }, NodeLoad {}, batch).status
          == Wire::Status::Ok);
}

TEST_CASE("A worker that names no slot count is sized from its own hardware", "[distributed][scheduler]")
{
    // Zero used to be refused as "a worker that will never be picked". It now means
    // "size me from what I told you about myself", which is the spelling a node
    // should prefer: a node that did the arithmetic itself would be the one place a
    // workstation's reserve could be got wrong with nothing able to tell. What the
    // old refusal protected against is closed by `OfferableSlots` never returning
    // zero, so the worker below must be pickable rather than refused.
    Leading fleet;

    auto sized = OneSlot("gcc-14", "10.0.0.2:7100");
    sized.slots = 0;
    sized.capacity = NodeCapacity { .logicalCores = 8,
                                    .totalMemoryBytes = 32ULL << 30,
                                    .nodeClass = NodeClass::Workstation,
                                    .reservedCores = 0,
                                    .reserveIsExplicit = false };
    REQUIRE(fleet.service.Register(Insider, sized).status == Wire::Status::Ok);

    // Eight cores less the workstation reserve of two.
    auto const live = fleet.service.Workers().LiveWorkers();
    REQUIRE(live.size() == 1);
    CHECK(live[0].slots == 6);

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).status == Wire::Status::Ok);
}

TEST_CASE("An empty fleet and a busy one are different refusals", "[distributed][scheduler]")
{
    // They are different *operator* problems even though the client answers both by
    // compiling locally: no worker means a fingerprint nobody serves, no capacity
    // means the fleet is too small. Summing them would hide the first behind the
    // second exactly when a fleet is busy.
    Leading fleet;

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).error == Wire::ErrorCode::NoWorker);

    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).status == Wire::Status::Ok);

    // The one slot is spent, so a *different* key now has nowhere to go.
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-2")).error == Wire::ErrorCode::NoCapacity);

    // And a toolchain nobody serves is still NoWorker, not NoCapacity -- the two
    // conditions coexist and must not collapse into whichever is checked first.
    CHECK(fleet.service.Lease(Insider, Ask("clang-20", "key-3")).error == Wire::ErrorCode::NoWorker);
}

TEST_CASE("The same key is not compiled twice at once", "[distributed][scheduler]")
{
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    auto second = OneSlot("gcc-14", "10.0.0.3:7100");
    second.slots = 4;
    REQUIRE(fleet.service.Register(Insider, second).status == Wire::Status::Ok);

    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "shared-key")).status == Wire::Status::Ok);

    // Capacity remains -- five slots, one spent -- so this refusal is about the key
    // rather than about the fleet, which is why it has a code of its own.
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "shared-key")).error == Wire::ErrorCode::AlreadyInFlight);
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "other-key")).status == Wire::Status::Ok);
}

TEST_CASE("An unknown worker is told to register again", "[distributed][scheduler]")
{
    // Silence would leave it heartbeating into a void forever while the fleet ran
    // without it. Reachable in the ordinary course: the scheduler restarts, or
    // leadership moves, and the worker's id means nothing to the node that now
    // answers.
    Leading fleet;

    auto const reply = fleet.service.Heartbeat(Insider, "worker-that-never-was", NodeLoad {});
    CHECK(reply.status == Wire::Status::Error);
    CHECK(reply.error == Wire::ErrorCode::UnknownLease);
    CHECK(reply.message == "unknown worker; register again");
}

TEST_CASE("A worker that stops heartbeating leaves the fleet", "[distributed][scheduler]")
{
    // The expiry rule, asserted with a ManualClock rather than a sleep -- which is
    // the entire reason this class is I/O-free.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).status == Wire::Status::Ok);

    fleet.clock.Advance(WorkerRegistry::DefaultHeartbeatTimeout + 1s);

    CHECK(fleet.service.Workers().LiveWorkers().empty());
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-2")).error == Wire::ErrorCode::NoWorker);
}

TEST_CASE("Refusals an operator sizes a fleet from are counted; client defects are not", "[distributed][scheduler][metrics]")
{
    // The split is the point. A malformed frame is a broken client, and counting it
    // beside the capacity refusals would put one build's noise into the numbers a
    // fleet is sized from.
    Leading fleet;

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).error == Wire::ErrorCode::NoWorker);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoWorker) == 1);

    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrations) == 1);

    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).status == Wire::Status::Ok);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesGranted) == 1);

    // The one slot is now spent, so this asks about a key that is in flight AT a
    // fleet that is full -- both conditions at once, which is the ordinary shape of
    // a wide parallel build. It must be reported as the duplicate it is.
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-1")).error == Wire::ErrorCode::AlreadyInFlight);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesDuplicate) == 1);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 0);

    // A *different* key at the same full fleet is the genuine capacity refusal.
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-2")).error == Wire::ErrorCode::NoCapacity);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 1);

    // The client-defect arm lives one layer up now: a malformed frame is a decode
    // failure, so `SchedulerProtocol` is what produces it and what must not count it.
    // What this layer still owes the counter is that a registration it ACCEPTS moves
    // it exactly once -- including one that named no slot count, which used to be
    // refused here and is now the ordinary way a node asks to be sized.
    auto sized = OneSlot("gcc-14", "10.0.0.9:7100");
    sized.slots = 0;
    sized.capacity = NodeCapacity { .logicalCores = 4, .nodeClass = NodeClass::Dedicated };
    CHECK(fleet.service.Register(Insider, sized).status == Wire::Status::Ok);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrations) == 2);
}

TEST_CASE("Every refusal this service makes is describable on the wire", "[distributed][scheduler][wire]")
{
    // A code the error table does not know reaches a client as a number with no
    // name, which is the shape of refusal an operator cannot act on -- the defect
    // the typed error codes replaced a bare string to avoid. The two codes this PR
    // added are the ones at risk, so the check is over the set this class can
    // actually produce.
    constexpr std::array Producible {
        Wire::ErrorCode::NotLeader,       Wire::ErrorCode::NotAMember,     Wire::ErrorCode::NoWorker,
        Wire::ErrorCode::NoCapacity,      Wire::ErrorCode::Withdrawn,      Wire::ErrorCode::UnknownLease,
        Wire::ErrorCode::AlreadyInFlight, Wire::ErrorCode::MalformedFrame,
    };

    for (auto const code: Producible)
    {
        auto const* described = Wire::Describe(code);
        REQUIRE(described != nullptr);
        CHECK_FALSE(described->name.empty());
        CHECK_FALSE(described->defaultMessage.empty());
    }
}

TEST_CASE("A duplicate at a full fleet is reported as a duplicate", "[distributed][scheduler]")
{
    // The ordering the move corrected. `LeaseTable::Acquire` needs a worker id, so
    // the code this was lifted from had to Pick before it could ask about the key --
    // and a second client missing the same key at a busy fleet was therefore told
    // `NoCapacity`. Both conditions genuinely hold; which one the operator is shown
    // is the whole question, and "this build asked for the same object twice" is
    // the actionable half.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "hot-key")).status == Wire::Status::Ok);

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "hot-key")).error == Wire::ErrorCode::AlreadyInFlight);

    // And the fleet really is full, so this is not the check passing by accident.
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "cold-key")).error == Wire::ErrorCode::NoCapacity);
}

TEST_CASE("An expired lease stops suppressing its key", "[distributed][scheduler]")
{
    // `IsInFlight` reports liveness rather than presence: an expired entry is left
    // behind until the next `Acquire` for that key sweeps it, so a check on the map
    // alone would refuse a key forever once one client had abandoned it -- one
    // object permanently undistributable, with nothing saying so.
    Leading fleet;
    auto twoSlots = OneSlot("gcc-14", "10.0.0.2:7100");
    twoSlots.slots = 2;
    REQUIRE(fleet.service.Register(Insider, twoSlots).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "abandoned")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "abandoned")).error == Wire::ErrorCode::AlreadyInFlight);

    // Heartbeat across the lease lifetime so the worker stays live while the lease
    // does not; expiring both would prove nothing about the key. The step has to
    // stay under the *heartbeat* timeout (90s) while the total clears the *lease*
    // timeout (600s), which is why this is a loop rather than one Advance.
    constexpr auto Step = WorkerRegistry::DefaultHeartbeatTimeout / 2;
    constexpr auto Steps = (LeaseTable::DefaultLeaseTimeout / Step) + 1;
    for ([[maybe_unused]] auto const tick: std::views::iota(0, static_cast<int>(Steps)))
    {
        fleet.clock.Advance(Step);
        auto const workers = fleet.service.Workers().LiveWorkers();
        REQUIRE(workers.size() == 1);
        REQUIRE(fleet.service.Heartbeat(Insider, workers.front().id, NodeLoad {}).status == Wire::Status::Ok);
    }

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "abandoned")).status == Wire::Status::Ok);
}

TEST_CASE("A resolved lease stops suppressing its key at once", "[distributed][scheduler]")
{
    // The regression for #212, and it could not have been written before the verb
    // existed: nothing anywhere could tell the scheduler a job had ended, so a key
    // stayed marked in-flight for the full 600-second lease timeout and every later
    // compile of it was refused `AlreadyInFlight`. Expiry -- documented as the
    // safety net for a client that DIED -- was doing the work of the ordinary path.
    //
    // The clock does not move here, which is the assertion: releasing is what frees
    // the key, not waiting.
    Leading fleet;
    auto twoSlots = OneSlot("gcc-14", "10.0.0.2:7100");
    twoSlots.slots = 2;
    REQUIRE(fleet.service.Register(Insider, twoSlots).status == Wire::Status::Ok);

    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "hot-key"));
    REQUIRE(granted.status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "hot-key")).error == Wire::ErrorCode::AlreadyInFlight);

    REQUIRE(fleet.service.Release(Insider, TokenOf(granted), "hot-key").status == Wire::Status::Ok);

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "hot-key")).status == Wire::Status::Ok);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReleased) == 1);
}

TEST_CASE("A resolved lease gives the worker back its slot", "[distributed][scheduler]")
{
    // The other half of the same missing transition. `JobStarted` at `Lease` is what
    // stops the scheduler over-assigning inside one heartbeat window, and with no
    // pair it only ever climbed: the single-slot worker below was full until its
    // next heartbeat overwrote the count, so a burst of leases took a machine out of
    // rotation for up to twenty seconds at a time.
    //
    // A *different* key each time, so what is being asserted is capacity rather than
    // duplicate suppression.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);

    auto const first = fleet.service.Lease(Insider, Ask("gcc-14", "key-1"));
    REQUIRE(first.status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "key-2")).error == Wire::ErrorCode::NoCapacity);

    REQUIRE(fleet.service.Release(Insider, TokenOf(first), "key-1").status == Wire::Status::Ok);

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "key-2")).status == Wire::Status::Ok);
}

TEST_CASE("A lease that is already gone is refused, not waved through", "[distributed][scheduler]")
{
    // Reachable two ways, and both are worth telling an operator about: the lease
    // expired under a job that outlived it -- a fleet whose lease timeout is shorter
    // than its slowest translation unit -- or the same token is being resolved
    // twice. Silence would leave the first of those with no diagnostic anywhere,
    // which is the shape of failure this whole issue was.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "key-1"));
    REQUIRE(granted.status == Wire::Status::Ok);
    REQUIRE(fleet.service.Release(Insider, TokenOf(granted), "key-1").status == Wire::Status::Ok);

    auto const again = fleet.service.Release(Insider, TokenOf(granted), "key-1");
    CHECK(again.status == Wire::Status::Error);
    CHECK(again.error == Wire::ErrorCode::UnknownLease);
    CHECK(fleet.service.Release(Insider, "no-such-token", "key-1").error == Wire::ErrorCode::UnknownLease);

    // Uncounted by design: it is a statement about one client's timing, not about
    // the capacity an operator sizes a fleet from.
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReleased) == 1);
}

TEST_CASE("A machine that goes away takes its leases with it", "[distributed][scheduler]")
{
    // The case `LeaseTable::ReleaseWorker` was written for and had no caller: a
    // worker dying mid-job left its keys marked in-flight, so every client that
    // later missed on one was refused `AlreadyInFlight` and compiled locally until
    // the lease timed out ten minutes later -- the fleet losing a machine quietly
    // stopping distributing part of the build, with nothing anywhere saying so.
    //
    // The second worker is what makes this an assertion about the LEASE rather than
    // about capacity: the fleet still has somewhere to send the job.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "orphaned")).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "orphaned")).error == Wire::ErrorCode::AlreadyInFlight);

    // Past the heartbeat timeout but well inside the lease's own: expiry must not be
    // what frees this key, or the case is proving nothing.
    fleet.clock.Advance(WorkerRegistry::DefaultHeartbeatTimeout + 1ms);
    static_assert(WorkerRegistry::DefaultHeartbeatTimeout < LeaseTable::DefaultLeaseTimeout);

    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.3:7100")).status == Wire::Status::Ok);
    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "orphaned")).status == Wire::Status::Ok);

    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkersExpired) == 1);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReclaimed) == 1);

    // And the dead machine is gone from the fleet rather than merely hidden.
    auto const live = fleet.service.Workers().LiveWorkers();
    REQUIRE(live.size() == 1);
    CHECK(live.front().endpoint == "10.0.0.3:7100");
}

TEST_CASE("A re-registering worker keeps the leases it may still be running", "[distributed][scheduler]")
{
    // The tempting symmetry, and it is wrong. `WorkerRegistry::Register` resets
    // `inFlight` on the reasoning that a re-registering worker restarted and
    // whatever it was running is gone -- but a node re-registers after ANY refused
    // heartbeat, `EndpointBusy` or a `NotLeader` during an election included, and
    // not only after a restart. Releasing here would wipe leases for compiles that
    // are still running and hand a second client the same work. `inFlight` recovers
    // from that guess at the next heartbeat; a lease does not.
    //
    // A worker that genuinely restarted needs nothing here: its client's compile
    // exchange fails and the client resolves its own lease on that path.
    Leading fleet;
    auto twoSlots = OneSlot("gcc-14", "10.0.0.2:7100");
    twoSlots.slots = 2;
    REQUIRE(fleet.service.Register(Insider, twoSlots).status == Wire::Status::Ok);
    REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "still-running")).status == Wire::Status::Ok);

    REQUIRE(fleet.service.Register(Insider, twoSlots).status == Wire::Status::Ok);

    CHECK(fleet.service.Lease(Insider, Ask("gcc-14", "still-running")).error == Wire::ErrorCode::AlreadyInFlight);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReclaimed) == 0);
}

TEST_CASE("A release names its key, so a reissued token resolves nothing", "[distributed][scheduler]")
{
    // Tokens are `l1`, `l2`, ... from a counter that starts again at one in every
    // freshly constructed table -- so after a scheduler restart a client still
    // holding `l1` from the previous instance would otherwise resolve whatever this
    // one has since issued under that number, freeing a key somebody is building and
    // decrementing a worker that is busy. Two schedulers here is exactly that: the
    // second is the restarted process, and `before` is a token minted by the first.
    Leading first;
    REQUIRE(first.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    auto const before = TokenOf(first.service.Lease(Insider, Ask("gcc-14", "old-key")));

    Leading restarted;
    REQUIRE(restarted.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    auto const after = restarted.service.Lease(Insider, Ask("gcc-14", "new-key"));
    REQUIRE(after.status == Wire::Status::Ok);
    REQUIRE(TokenOf(after) == before); // the collision this guards, spelled out

    CHECK(restarted.service.Release(Insider, before, "old-key").error == Wire::ErrorCode::UnknownLease);
    // And the live lease it collided with is untouched -- neither resolved nor
    // erased, so the client that holds it can still resolve it itself.
    CHECK(restarted.service.Lease(Insider, Ask("gcc-14", "new-key")).error == Wire::ErrorCode::AlreadyInFlight);
    CHECK(restarted.service.Release(Insider, TokenOf(after), "new-key").status == Wire::Status::Ok);
}

TEST_CASE("A job that outlived its lease is told so", "[distributed][scheduler]")
{
    // The reason that refusal is worth having at all, and the case a key-index check
    // cannot reach: nothing re-leased this key, so the entry is still sitting in the
    // lease table when its holder finally reports -- presence rather than liveness.
    // Answering `Ok` would leave a fleet whose lease timeout is shorter than its
    // slowest translation unit with no diagnostic anywhere, which is the shape of
    // silence this whole issue was.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).status == Wire::Status::Ok);
    auto const slow = fleet.service.Lease(Insider, Ask("gcc-14", "slow-key"));
    REQUIRE(slow.status == Wire::Status::Ok);

    fleet.clock.Advance(LeaseTable::DefaultLeaseTimeout + 1ms);

    CHECK(fleet.service.Release(Insider, TokenOf(slow), "slow-key").error == Wire::ErrorCode::UnknownLease);
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReleased) == 0);
}

TEST_CASE("A worker that cannot name itself in UTF-8 is refused", "[distributed][scheduler]")
{
    // Everything a peer states about itself is copied into the leader's view of the
    // fleet and read back out of it by an operator. `/fleet.json` is the surface
    // that cannot survive it: RFC 8259 requires UTF-8 of JSON exchanged between
    // systems, so one worker's stray byte makes the whole fleet's answer a document
    // a strict parser may reject -- not merely that worker's row in it.
    //
    // Refused where the wire becomes fleet state, so every surface rendered from
    // that state is clean, rather than each renderer repairing it and the ones that
    // forget carrying the originals.
    Leading fleet;

    auto const refused = [&fleet](WorkerRegistration const& registration, std::string_view field) {
        auto const reply = fleet.service.Register(Insider, registration);
        CHECK(reply.status == Wire::Status::Error);
        CHECK(reply.error == Wire::ErrorCode::MalformedRegistration);
        // The field is named, because all three arrive over one verb and each sends
        // an operator somewhere different: a `--toolchain` override, an
        // `--endpoint`, or a peer running something this fleet did not build.
        CHECK(reply.message == std::format("{} is not valid UTF-8", field));

        // Refused means refused: nothing reached the registry, so no later snapshot
        // can carry it and no renderer has to know about this at all.
        CHECK(fleet.service.Workers().LiveWorkers().empty());
        CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrations) == 0);
        CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed) == 1);
    };

    SECTION("the toolchain fingerprint")
    {
        refused(OneSlot("gcc-14-a1\x80\x80", "10.0.0.2:7100"), "fingerprint");
    }

    SECTION("the endpoint clients would be sent to")
    {
        refused(OneSlot("gcc-14", "10.0.0.2:7100\xFF"), "endpoint");
    }

    SECTION("the version it says it is running")
    {
        // Free-form on the wire, and free-form is not the same claim as "any bytes":
        // it is the field an operator reads during a rolling upgrade, and the one a
        // peer this fleet did not build is likeliest to fill with something
        // surprising.
        auto registration = OneSlot("gcc-14", "10.0.0.2:7100");
        registration.version = "1.2.3-\xE2\x82"; // a three-byte sequence, two bytes long
        refused(registration, "version");
    }

    SECTION("the label a person reads the toolchain by")
    {
        // The likeliest of the four to arrive as something that is not text, because
        // it is the only one this project did not compose: it is read out of a
        // compiler's own `--version` banner, which a machine in a non-UTF-8 locale
        // prints in whatever its code page happens to be (#194).
        auto registration = OneSlot("gcc-14", "10.0.0.2:7100");
        registration.toolchainLabel = "cl 19.44\xC3"; // a lead byte with nothing after it
        refused(registration, "toolchain label");
    }
}

TEST_CASE("A toolchain named in multi-byte UTF-8 registers like any other", "[distributed][scheduler]")
{
    // The rule is about ENCODING, not about ASCII. A fingerprint is opaque by
    // design -- the launcher computes it and the scheduler matches it byte for
    // byte -- so narrowing it to ASCII would be a second, unannounced restriction
    // on a field the wire deliberately does not constrain.
    Leading fleet;

    auto registration = OneSlot("gcc-14-\xC3\xA9\xE2\x82\xAC", "10.0.0.2:7100");
    registration.version = "1.2.3-\xF0\x9F\x92\xA9";
    REQUIRE(fleet.service.Register(Insider, registration).status == Wire::Status::Ok);

    auto const live = fleet.service.Workers().LiveWorkers();
    REQUIRE(live.size() == 1);
    CHECK(live.front().fingerprint == "gcc-14-\xC3\xA9\xE2\x82\xAC");
    CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed) == 0);
}

TEST_CASE("An endpoint is measured against the caller's own address, not refused", "[distributed][scheduler]")
{
    // Issue #242, and what lands here is the MEASUREMENT rather than a refusal --
    // `.agent/rules/distributed-compilation.md` carries why an address comparison is
    // both too strict to deploy and too weak to be the control. So every row below
    // asserts the registration is still ACCEPTED; only the counting varies.
    struct Row
    {
        char const* what;            ///< The shape, for the failure message.
        char const* peer;            ///< Where the registration arrived from.
        char const* endpoint;        ///< What it advertised.
        std::uint64_t mismatches {}; ///< 1 when this should be counted.
    };

    auto const rows = std::to_array<Row>({
        { .what = "the same host, with a port the caller could not have shown",
          .peer = "10.0.0.2",
          .endpoint = "10.0.0.2:7100",
          .mismatches = 0 },
        // A bare host is a legitimate spelling that `HostOfEndpoint` keeps whole.
        // Reporting a mismatch here would accuse a worker that named its host
        // perfectly well.
        { .what = "a bare host with no port at all", .peer = "10.0.0.2", .endpoint = "10.0.0.2", .mismatches = 0 },
        // Bracketed on the wire, bare from the kernel. Splitting has to strip the
        // brackets or every IPv6 worker reports a mismatch forever.
        { .what = "an IPv6 endpoint against the bare host a kernel reports",
          .peer = "2001:db8::1",
          .endpoint = "[2001:db8::1]:7100",
          .mismatches = 0 },
        // The mapped form is a property of how the LISTENER was bound, not of the
        // peer; `Core/HostPort` carries why that has to be folded.
        { .what = "a dual-stack listener's mapped spelling of the same address",
          .peer = "::ffff:10.0.0.2",
          .endpoint = "10.0.0.2:7100",
          .mismatches = 0 },
        { .what = "a mapped endpoint against an unmapped caller",
          .peer = "10.0.0.2",
          .endpoint = "[::ffff:10.0.0.2]:7100",
          .mismatches = 0 },

        // And the mismatches. This is the one the ticket is about: a third host,
        // named by a caller that is neither of them.
        { .what = "a third host entirely", .peer = "10.0.0.2", .endpoint = "10.0.0.9:7100", .mismatches = 1 },
        // Whole-string, never a prefix -- the rule `ClusterMembership` records for
        // the same reason: `10.0.0.2` must not pass for `10.0.0.20`.
        { .what = "a host the caller's is a prefix of", .peer = "10.0.0.2", .endpoint = "10.0.0.20:7100", .mismatches = 1 },
        // The shape this project's own getting-started page builds: a node
        // registering with its own scheduler over loopback while advertising a name
        // clients can route to. Counted, and ADMITTED -- refusing it would refuse the
        // first fleet in the documentation.
        { .what = "the documented first node: loopback caller, DNS-named endpoint",
          .peer = "127.0.0.1",
          .endpoint = "scheduler.internal:6676",
          .mismatches = 1 },
        // A caller this machine cannot name has not shown that its endpoint names
        // itself, so the empty host matches nothing rather than everything.
        { .what = "a peer whose address the kernel would not give up",
          .peer = "",
          .endpoint = "10.0.0.2:7100",
          .mismatches = 1 },
        // And the case that makes the empty-host rule load-bearing rather than
        // incidental: under a plain string compare these two are EQUAL, so an
        // unnameable peer would be reported as having named itself.
        { .what = "an unnameable peer advertising nothing at all", .peer = "", .endpoint = "", .mismatches = 1 },
    });

    for (auto const& row: rows)
    {
        INFO(row.what);
        Leading fleet;

        auto const caller = CallerContext { .membership = Membership::Member, .peerId = row.peer };
        CHECK(fleet.service.Register(caller, OneSlot("gcc-14", row.endpoint)).status == Wire::Status::Ok);

        // Accepted means IN the fleet, not merely answered `Ok`: a worker counted and
        // then dropped would satisfy the status check and be a refusal in everything
        // but name.
        CHECK(fleet.service.Workers().LiveWorkers().size() == 1);
        CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrations) == 1);
        CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchWorkerEndpointMismatch) == row.mismatches);
    }
}

TEST_CASE("A mismatch names both addresses, because a counter cannot", "[distributed][scheduler]")
{
    // The counter says the condition is happening; deciding whether a strict rule is
    // viable needs to know WHICH worker and against what, and only a line carries
    // that. Asserted rather than assumed because the logger is an optional seam --
    // one nothing set would leave the counter rising with nothing able to explain it,
    // which is the defect the seam exists to prevent.
    Leading fleet;

    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "10.0.0.9:7100")).status == Wire::Status::Ok);

    auto const records = fleet.logger.Snapshot();
    REQUIRE(records.size() == 1);
    // `Info`, not a warning: on a fleet advertising DNS names this is every
    // registration and nothing is wrong, and a signal that fires permanently on
    // correct deployments is one operators learn to filter -- so it would not be
    // there on the day it means something. Info is the default production level, so
    // it is still read.
    CHECK(records.front().level == LogLevel::Info);
    CHECK(records.front().message.contains("peer-1"));
    CHECK(records.front().message.contains("10.0.0.9:7100"));

    // It says what it is. On a fleet using DNS names this line is every registration,
    // so one that read as an accusation would train an operator to ignore it.
    CHECK(records.front().message.contains("#242"));

    // The unnameable-peer wording, which is a second branch inside the same line and
    // was previously written by nothing any case read.
    fleet.logger.Clear();
    REQUIRE(
        fleet.service
            .Register(CallerContext { .membership = Membership::Member, .peerId = "" }, OneSlot("gcc-16", "10.0.0.4:7100"))
            .status
        == Wire::Status::Ok);
    REQUIRE(fleet.logger.Snapshot().size() == 1);
    CHECK(fleet.logger.Snapshot().front().message.contains("an unnameable peer"));

    // And a matching registration is silent, or the line means nothing.
    fleet.logger.Clear();
    REQUIRE(fleet.service
                .Register(CallerContext { .membership = Membership::Member, .peerId = "10.0.0.3" },
                          OneSlot("gcc-15", "10.0.0.3:7100"))
                .status
            == Wire::Status::Ok);
    CHECK(fleet.logger.Snapshot().empty());
}

TEST_CASE("A grant is signed for exactly one worker, and only that worker's is valid", "[distributed][scheduler][lease]")
{
    // The hole this closes: a lease used to be a small decimal serial, and the
    // worker's validator was `[](...){ return true; }`. Anyone who could reach a
    // compile port and present any string got a compile.
    Signing fleet;
    fleet.wallClock.SetNow(Noon);

    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "peer-1:7100")).status == Wire::Status::Ok);
    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "obj-1"));
    REQUIRE(granted.status == Wire::Status::Ok);
    auto const token = TokenOf(granted);

    // The grant is no longer the lease table's handle -- it WRAPS one. Asserted
    // directly, because everything below would also pass for a token that merely
    // happened to verify while carrying no binding at all.
    auto const wrapped = AuthenticateLeaseToken(fleet.key, token);
    REQUIRE(wrapped.has_value());
    CHECK(wrapped->serial != token);

    SECTION("the worker it names accepts it")
    {
        auto const verified = VerifyLeaseToken(fleet.key,
                                               token,
                                               LeaseExpectation { .endpoint = "peer-1:7100",
                                                                  .fingerprint = "gcc-14",
                                                                  .clusterId = Signing::TestCluster,
                                                                  .epoch = LeaseEpochCheck::NotKnownHere() },
                                               Noon);
        REQUIRE(verified.has_value());
        CHECK(verified->key == "obj-1");

        // Derived from the lease table's own lifetime rather than written beside it.
        // A token outliving its lease would be a capability nothing has a record of;
        // one dying first would refuse work whose key is still being suppressed.
        CHECK(verified->expiresAt == Noon + LeaseTable::DefaultLeaseTimeout);
    }

    SECTION("a second worker does not")
    {
        // The replay the endpoint is inside the MAC for. Without it, one grant is a
        // grant on every machine that trusts the key -- which is every machine in
        // the fleet.
        auto const refusal = VerifyLeaseToken(fleet.key,
                                              token,
                                              LeaseExpectation { .endpoint = "peer-2:7100",
                                                                 .fingerprint = "gcc-14",
                                                                 .clusterId = Signing::TestCluster,
                                                                 .epoch = LeaseEpochCheck::NotKnownHere() },
                                              Noon);
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::EndpointMismatch);
    }

    SECTION("and it stops being good once it has expired")
    {
        auto const refusal = VerifyLeaseToken(fleet.key,
                                              token,
                                              LeaseExpectation { .endpoint = "peer-1:7100",
                                                                 .fingerprint = "gcc-14",
                                                                 .clusterId = Signing::TestCluster,
                                                                 .epoch = LeaseEpochCheck::NotKnownHere() },
                                              Noon + LeaseTable::DefaultLeaseTimeout + LeaseTokenClockSkewSlack + 1s);
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Expired);
    }
}

TEST_CASE("A release names a lease this scheduler actually signed", "[distributed][scheduler][lease]")
{
    Signing fleet;
    fleet.wallClock.SetNow(Noon);

    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "peer-1:7100")).status == Wire::Status::Ok);
    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "obj-1"));
    REQUIRE(granted.status == Wire::Status::Ok);
    auto const token = TokenOf(granted);

    SECTION("the signed token resolves it")
    {
        CHECK(fleet.service.Release(Insider, token, "obj-1").status == Wire::Status::Ok);
        CHECK_FALSE(fleet.service.Leases().IsInFlight("obj-1"));
    }

    SECTION("a bare serial does not")
    {
        // What a member could once do by guessing: a small integer, plus a key it can
        // see in its own build, was enough to free somebody else's lease.
        auto const refused = fleet.service.Release(Insider, "1", "obj-1");
        CHECK(refused.status == Wire::Status::Error);
        CHECK(refused.error == Wire::ErrorCode::LeaseUnauthorized);
        CHECK(fleet.service.Leases().IsInFlight("obj-1"));

        // Counted apart from `UnknownLease`, which names a lease this scheduler DID
        // issue and has since forgotten. This one was never issued at all.
        CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesUnauthorized) == 1);
        CHECK(fleet.metrics.Read(IMetricsSink::Counter::DispatchLeasesReleased) == 0);
    }

    SECTION("nor does one signed with another cluster's key")
    {
        // The claims name THIS cluster and this term. Only the signing key differs, so
        // what the case pins is the MAC and nothing else -- a foreign `clusterId` here
        // would leave it passing whichever of the two checks happened to run first.
        auto const foreign = MintLeaseToken(std::vector<std::byte>(32, std::byte { 0x11 }),
                                            LeaseClaims { .serial = "1",
                                                          .endpoint = "peer-1:7100",
                                                          .fingerprint = "gcc-14",
                                                          .key = "obj-1",
                                                          .expiresAt = Noon + 10min,
                                                          // `std::string`, not the
                                                          // `string_view` the
                                                          // EXPECTATIONS above take:
                                                          // claims come back by value
                                                          // from a decode and must not
                                                          // borrow the bytes they were
                                                          // decoded from.
                                                          .clusterId = std::string { Signing::TestCluster },
                                                          .epoch = 0 });
        CHECK(fleet.service.Release(Insider, foreign, "obj-1").error == Wire::ErrorCode::LeaseUnauthorized);
        CHECK(fleet.service.Leases().IsInFlight("obj-1"));
    }

    SECTION("nor does an authentic token used to release a DIFFERENT key")
    {
        // #323. The token names the key it covers, and until now `Release` discarded
        // that claim and trusted the caller's, leaving `LeaseTable` to notice.
        //
        // It did notice -- which is exactly the problem. The guard was correct by
        // coincidence of another component's strictness, not by construction, and a
        // lookup made more permissive later would have removed the only check that
        // the released lease is the one the token names, with nothing on this path
        // to fail.
        //
        // A SECOND live lease, so the release cannot be refused merely for naming
        // something unknown: both keys are in flight, the token is authentic, and the
        // only thing wrong is that it does not name this one.
        //
        // On a second WORKER, because the one above has a single slot and obj-1 is
        // holding it -- otherwise this lease is refused for capacity and the case
        // proves nothing about keys.
        REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "peer-2:7100")).status == Wire::Status::Ok);
        REQUIRE(fleet.service.Lease(Insider, Ask("gcc-14", "obj-2")).status == Wire::Status::Ok);
        REQUIRE(fleet.service.Leases().IsInFlight("obj-2"));

        // The CODE is the assertion, not the refusal. Without the claim check this
        // is still refused -- as `UnknownLease`, measured -- because `LeaseTable`
        // finds no live lease under obj-2 for obj-1's serial. That is the coincidence
        // the ticket is about: the release was rejected by a lookup miss rather than
        // by the credential, so the only thing standing between an authentic token
        // and somebody else's key was another component's strictness.
        //
        // `LeaseUnauthorized` says the credential refused it. `UnknownLease` says the
        // table did not recognise it, which is also what an expired lease looks like.
        auto const refused = fleet.service.Release(Insider, token, "obj-2");
        CHECK(refused.status == Wire::Status::Error);
        CHECK(refused.error == Wire::ErrorCode::LeaseUnauthorized);

        // Neither key moved: not the one the token names, and not the one the caller
        // asked about. A refusal that freed either would be worse than the bug.
        CHECK(fleet.service.Leases().IsInFlight("obj-1"));
        CHECK(fleet.service.Leases().IsInFlight("obj-2"));

        // And the token still works for what it actually covers, so the check is a
        // rule about the claim rather than a token that has been spent by trying.
        CHECK(fleet.service.Release(Insider, token, "obj-1").status == Wire::Status::Ok);
        CHECK_FALSE(fleet.service.Leases().IsInFlight("obj-1"));
    }
}

TEST_CASE("A scheduler with no cluster key says so, once", "[distributed][scheduler][lease]")
{
    // Unsigned grants stay a working configuration -- a single machine with no
    // `--cluster-key-file` is what most people run, and refusing to schedule would
    // break every one of those installs. What is not acceptable is doing it quietly:
    // a fleet that is green and is not doing the thing it claims is the failure this
    // repository keeps rediscovering.
    Leading fleet;
    REQUIRE(fleet.service.Register(Insider, OneSlot("gcc-14", "peer-1:7100")).status == Wire::Status::Ok);
    fleet.logger.Clear();

    auto const granted = fleet.service.Lease(Insider, Ask("gcc-14", "obj-1"));
    REQUIRE(granted.status == Wire::Status::Ok);

    // Unsigned means the lease table's own handle, unchanged -- so an old launcher
    // and an old worker go on working exactly as they did. Spelled as the literal
    // `LeaseTable` mints, on purpose: if that format moves, the thing this case is
    // about has moved with it.
    CHECK(TokenOf(granted) == "l1");

    auto const records = fleet.logger.Snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records.front().level == LogLevel::Warn);
    CHECK(records.front().message.contains("UNSIGNED"));
    CHECK(records.front().message.contains("--cluster-key-file"));

    // Once for the life of the process, not once per grant: the fact is about the
    // configuration and does not change, and a line per lease would bury it under
    // itself on the first parallel build.
    fleet.logger.Clear();
    // Released first: the worker offers one slot, so a second lease taken while the
    // first is outstanding would be refused for capacity and never reach the mint.
    REQUIRE(fleet.service.Release(Insider, TokenOf(granted), "obj-1").status == Wire::Status::Ok);
    auto const second = fleet.service.Lease(Insider, Ask("gcc-14", "obj-2"));
    REQUIRE(second.status == Wire::Status::Ok);
    CHECK(fleet.logger.Snapshot().empty());
}

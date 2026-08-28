// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

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

/// A service that already leads, which is the precondition of every verb.
struct Leading
{
    Leading()
    {
        service.SetRole(SchedulerRole::Leader, {});
    }

    ManualClock clock;
    AtomicMetricsSink metrics;
    SchedulerService service { clock, metrics };
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

TEST_CASE("Only the leader hands out capacity", "[distributed][scheduler]")
{
    // A follower's registry is a stale copy of somebody else's, so admitting a
    // worker here would put it in a fleet nothing schedules onto -- and it would
    // heartbeat into that void forever. The refusal carries the leader's address so
    // the caller can redirect instead.
    ManualClock clock;
    AtomicMetricsSink metrics;
    SchedulerService service { clock, metrics };

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
        service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000");
        auto const reply = service.Lease(Insider, Ask("gcc-14", "abc"));
        CHECK(reply.status == Wire::Status::Error);
        CHECK(reply.error == Wire::ErrorCode::NotLeader);
        CHECK(reply.message == "10.0.0.1:7000");
    }

    SECTION("leadership is the gate on every verb, not only on Lease")
    {
        // The rule is on the gate rather than on each handler, so a verb added
        // without thinking about it is refused rather than served. Asserted per
        // verb because "I added a handler that skips the gate" is exactly the
        // regression this arrangement exists to make impossible.
        service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000");
        CHECK(service.Register(Insider, OneSlot("gcc-14", "10.0.0.2:7100")).error == Wire::ErrorCode::NotLeader);
        CHECK(service.Heartbeat(Insider, "whoever", NodeLoad {}).error == Wire::ErrorCode::NotLeader);
        CHECK(service.Lease(Insider, Ask("gcc-14", "abc")).error == Wire::ErrorCode::NotLeader);
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
    SchedulerService service { clock, metrics };
    service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000");

    CHECK(service.Lease(Outsider, Ask("gcc-14", "abc")).error == Wire::ErrorCode::NotLeader);
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

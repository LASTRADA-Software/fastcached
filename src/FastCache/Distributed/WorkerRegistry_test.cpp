// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/NodeLoadTestUtils.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using namespace FastCache::Distributed::Testing;
using FastCache::Testing::Unwrap;

namespace
{

/// A registry over a clock the test drives, so every expiry assertion is exact
/// rather than a race against wall time.
struct Fixture
{
    ManualClock clock;
    WorkerRegistry registry { clock, std::chrono::milliseconds { 1000 } };
};

/// A registration with no codec preferences, which is what most of these cases
/// care about. Named so the field list is spelled once rather than at every call.
[[nodiscard]] WorkerRegistration Announce(std::string_view fingerprint, std::string_view endpoint, std::uint32_t slots)
{
    return { .fingerprint = fingerprint, .endpoint = endpoint, .slots = slots, .codecs = {} };
}

constexpr std::string_view Gcc13 = "gcc-13-abcdef";
constexpr std::string_view Gcc14 = "gcc-14-123456";

} // namespace

TEST_CASE("A registered worker is picked for its own fingerprint", "[distributed][registry]")
{
    Fixture fix;
    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->id == id);
    CHECK(picked->endpoint == "10.0.0.1:6676");
}

TEST_CASE("A job is never dispatched across fingerprints", "[distributed][registry]")
{
    // The single most important property here. An over-strict match costs a local
    // compile; an over-loose one produces a silently wrong object that is then
    // stored under a key other machines fetch. There is no symmetry between those
    // two errors, which is why no configuration can loosen this.
    Fixture fix;
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    auto const picked = fix.registry.Pick(Gcc14);
    REQUIRE_FALSE(picked.has_value());
    CHECK(picked.error() == PickError::NoWorker);
}

TEST_CASE("A near-miss fingerprint is still a miss", "[distributed][registry]")
{
    // Byte-identical means byte-identical: a prefix, a suffix, and a case
    // difference are all different toolchains as far as this is concerned.
    Fixture fix;
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    for (auto const& near:
         { std::string { Gcc13 } + "x", std::string { "x" } + std::string { Gcc13 }, std::string { "GCC-13-ABCDEF" } })
    {
        INFO("fingerprint " << near);
        CHECK_FALSE(fix.registry.Pick(near).has_value());
    }
}

TEST_CASE("An empty fleet and a busy fleet are different answers", "[distributed][registry]")
{
    // Both end in a local compile at the client, but they are different operator
    // problems -- one is a configuration mistake, the other is capacity -- and the
    // client reports the scheduler's own words.
    Fixture fix;
    CHECK(fix.registry.Pick(Gcc13).error() == PickError::NoWorker);

    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 1));
    fix.registry.JobStarted(id);
    CHECK(fix.registry.Pick(Gcc13).error() == PickError::NoCapacity);
}

TEST_CASE("The least-loaded matching worker wins", "[distributed][registry]")
{
    // Not round-robin: compile times vary by an order of magnitude within one
    // build, so distributing arrivals rather than load queues a long translation
    // unit behind another while a worker idles.
    Fixture fix;
    auto const busy = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));
    auto const idle = fix.registry.Register(Announce(Gcc13, "10.0.0.2:6676", 4));

    fix.registry.JobStarted(busy);
    fix.registry.JobStarted(busy);

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->id == idle);
}

TEST_CASE("A worker that stops heartbeating stops being dispatched to", "[distributed][registry]")
{
    // A worker that dies mid-job would otherwise hold its slots forever and the
    // fleet would shrink to nothing with no diagnostic.
    Fixture fix;
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));
    REQUIRE(fix.registry.Pick(Gcc13).has_value());

    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE_FALSE(picked.has_value());
    CHECK(picked.error() == PickError::NoWorker);
    CHECK(fix.registry.LiveWorkers().empty());
}

TEST_CASE("A heartbeat keeps a worker alive and corrects its load", "[distributed][registry]")
{
    Fixture fix;
    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    fix.clock.Advance(std::chrono::milliseconds { 900 });
    // The worker's own count is authoritative: the registry's drifts whenever a
    // client dies between leasing and compiling, and only the worker knows what it
    // is actually running.
    CHECK(fix.registry.Heartbeat(id, Busy(3)));
    fix.clock.Advance(std::chrono::milliseconds { 900 });

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->inFlight == 3);
}

TEST_CASE("A heartbeat from an unknown worker is refused", "[distributed][registry]")
{
    // So the worker learns to register again rather than heartbeating into a void.
    Fixture fix;
    CHECK_FALSE(fix.registry.Heartbeat("w999", NodeLoad {}));
}

TEST_CASE("A worker that restarts keeps one identity, not two", "[distributed][registry]")
{
    // Re-registration is keyed on (fingerprint, endpoint). Issuing a second id
    // would leave the first pointing at a dead port until it expired, and half the
    // leases for that toolchain would go there in the meantime.
    Fixture fix;
    auto const first = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));
    fix.registry.JobStarted(first);

    auto const second = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 8));
    CHECK(second == first);
    CHECK(fix.registry.LiveWorkers().size() == 1);

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->slots == 8);
    // A restarted worker is running nothing; carrying the old count forward would
    // make it look busy until its first heartbeat.
    CHECK(picked->inFlight == 0);
}

TEST_CASE("The same toolchain on two hosts is two workers", "[distributed][registry]")
{
    Fixture fix;
    auto const a = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 1));
    auto const b = fix.registry.Register(Announce(Gcc13, "10.0.0.2:6676", 1));
    CHECK(a != b);
    CHECK(fix.registry.LiveWorkers().size() == 2);
}

TEST_CASE("Finishing a job at zero outstanding does not wrap the counter", "[distributed][registry]")
{
    // `inFlight` is unsigned. A decrement at zero would report four billion jobs
    // outstanding, taking the worker out of rotation permanently and silently --
    // and reaching zero here is not even a bug, since a heartbeat can correct the
    // count downwards between a job starting and finishing.
    Fixture fix;
    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 1));

    fix.registry.JobFinished(id);
    fix.registry.JobFinished(id);

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->inFlight == 0);
}

TEST_CASE("A removed worker is gone immediately", "[distributed][registry]")
{
    Fixture fix;
    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));
    fix.registry.Remove(id);
    CHECK_FALSE(fix.registry.Pick(Gcc13).has_value());
}

TEST_CASE("A clock that moves backwards does not expire the fleet", "[distributed][registry]")
{
    // Not paranoia about the steady clock: a ManualClock in a test can be set
    // backwards, and treating a negative age as enormous would expire everything.
    Fixture fix;
    fix.clock.Advance(std::chrono::milliseconds { 5000 });
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    fix.clock.Advance(std::chrono::milliseconds { -2000 });
    CHECK(fix.registry.Pick(Gcc13).has_value());
}

TEST_CASE("A big machine with more running jobs still wins on headroom", "[distributed][registry]")
{
    // The correction. `Pick` compared `inFlight` in ABSOLUTE terms, which treats
    // every worker as an identical box: a 64-slot server running 8 jobs looked
    // busier than a 4-slot laptop running 2, when the server had 56 slots free and
    // the laptop had 2. Across a fleet of mixed machines -- the ordinary case, not
    // an exotic one -- that sends work to the smallest machines first and leaves the
    // big ones idle.
    Fixture fix;
    auto const server = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 64));
    auto const laptop = fix.registry.Register(Announce(Gcc13, "10.0.0.2:6676", 4));

    // The server is carrying four times the laptop's load and is still the better
    // place for the next job.
    for (auto job = 0; job < 8; ++job)
        fix.registry.JobStarted(server);
    for (auto job = 0; job < 2; ++job)
        fix.registry.JobStarted(laptop);

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->id == server);
}

TEST_CASE("Equal headroom is broken by which machine has more of itself left", "[distributed][registry]")
{
    // Where the headroom ties, the proportionally emptier machine takes it: 4 free
    // of 8 has more of itself left than 4 free of 64, and is the better place to put
    // a job that will occupy a core for seconds.
    Fixture fix;
    auto const big = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 64));
    auto const small = fix.registry.Register(Announce(Gcc13, "10.0.0.2:6676", 8));

    for (auto job = 0; job < 60; ++job)
        fix.registry.JobStarted(big);
    for (auto job = 0; job < 4; ++job)
        fix.registry.JobStarted(small);

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->id == small);
}

TEST_CASE("A full machine is never picked however large it is", "[distributed][registry]")
{
    // Headroom is a preference; the slot cap is not. A worker at its limit is out of
    // the running entirely, or the fleet would be fuller and slower than it believes
    // at the same moment.
    Fixture fix;
    auto const big = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 64));
    auto const small = fix.registry.Register(Announce(Gcc13, "10.0.0.2:6676", 2));

    for (auto job = 0; job < 64; ++job)
        fix.registry.JobStarted(big);

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->id == small);
}

TEST_CASE("A worker whose machine is busy elsewhere is passed over", "[distributed][registry]")
{
    // The end-to-end shape of the live-load rule, through the registry rather than
    // against the policy function: this is the path that would silently keep sending
    // work to a saturated developer machine if the heartbeat's load were recorded
    // and then never consulted.
    Fixture fix;
    auto const desk = fix.registry.Register(
        WorkerRegistration { .fingerprint = Gcc13,
                             .endpoint = "10.0.0.1:6676",
                             .slots = 0,
                             .codecs = {},
                             .capacity = NodeCapacity { .logicalCores = 8, .nodeClass = NodeClass::Workstation } });
    auto const server = fix.registry.Register(
        WorkerRegistration { .fingerprint = Gcc13,
                             .endpoint = "10.0.0.2:6676",
                             .slots = 0,
                             .codecs = {},
                             .capacity = NodeCapacity { .logicalCores = 4, .nodeClass = NodeClass::Dedicated } });

    // Six slots against four: with nothing else known, the bigger machine wins.
    auto const idle = fix.registry.Pick(Gcc13);
    REQUIRE(idle.has_value());
    CHECK(idle->id == desk);

    // Its owner then starts using it. Eight busy cores of eight, none of them ours,
    // so it withdraws entirely and the small dedicated node takes the work.
    REQUIRE(fix.registry.Heartbeat(desk, WithCpu(0, 1000)));
    auto const shifted = fix.registry.Pick(Gcc13);
    REQUIRE(shifted.has_value());
    CHECK(shifted->id == server);

    // And it comes back when they stop, rather than being written off.
    REQUIRE(fix.registry.Heartbeat(desk, WithCpu(0, 0)));
    auto const recovered = fix.registry.Pick(Gcc13);
    REQUIRE(recovered.has_value());
    CHECK(recovered->id == desk);
}

TEST_CASE("A fleet that is unavailable is a different refusal from one that is small", "[distributed][registry]")
{
    // Three refusals rather than two, for the reason there were two rather than one:
    // they are different operator problems with different fixes. Told "no capacity",
    // an operator buys machines; the truth in the first section below is that the
    // machines they already own have filled their scratch disks, and 200 MB fixes it.
    Fixture fix;
    auto const id = fix.registry.Register(
        WorkerRegistration { .fingerprint = Gcc13,
                             .endpoint = "10.0.0.1:6676",
                             .slots = 4,
                             .codecs = {},
                             .capacity = NodeCapacity { .logicalCores = 8, .nodeClass = NodeClass::Dedicated } });

    SECTION("slots free on paper, withdrawn in practice")
    {
        REQUIRE(fix.registry.Heartbeat(id, WithScratch(0, 0)));

        auto const picked = fix.registry.Pick(Gcc13);
        REQUIRE_FALSE(picked.has_value());
        CHECK(picked.error() == PickError::Withdrawn);
    }

    SECTION("genuinely full of this fleet's own work")
    {
        // No live-load report at all, so nothing is withdrawn; the four slots are
        // simply taken. This is the case that really does mean "buy more machines".
        REQUIRE(fix.registry.Heartbeat(id, Busy(4)));

        auto const picked = fix.registry.Pick(Gcc13);
        REQUIRE_FALSE(picked.has_value());
        CHECK(picked.error() == PickError::NoCapacity);
    }

    SECTION("both at once reports the actionable one")
    {
        // A second worker full of our own work, beside the first with a full disk.
        // `Withdrawn` wins: "some of your machines are unavailable" is fixable today,
        // where "the fleet is small" is a purchase -- and reporting the purchase
        // would hide the full disk behind a number that looks like growth.
        auto const busy = fix.registry.Register(Announce(Gcc13, "10.0.0.2:6676", 2));
        REQUIRE(fix.registry.Heartbeat(busy, Busy(2)));
        REQUIRE(fix.registry.Heartbeat(id, WithScratch(0, 0)));

        auto const picked = fix.registry.Pick(Gcc13);
        REQUIRE_FALSE(picked.has_value());
        CHECK(picked.error() == PickError::Withdrawn);
    }
}

TEST_CASE("One machine serving two toolchains is one node's cache", "[distributed][registry][cache]")
{
    // The registry keys on (fingerprint, endpoint), so a node started with two
    // `--toolchain` flags is two entries against one machine -- deliberately, and
    // documented at the node. Both heartbeat the SAME cache figures, because a cache
    // is per node and not per toolchain. So anything summing a cache field across
    // `LiveWorkers()` counts one node's objects and bytes twice, which on a fleet
    // page reads as a cache holding double what it holds.
    Fixture fixture;

    NodeCacheCapacity budget {};
    budget.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] = 256ULL << 20;
    auto announce = Announce("gcc-14", "10.0.0.2:7100", 4);
    announce.capacity.cache = budget;

    auto const gcc = fixture.registry.Register(announce);
    announce.fingerprint = "clang-20";
    auto const clang = fixture.registry.Register(announce);
    CHECK(gcc != clang);
    CHECK(fixture.registry.LiveWorkers().size() == 2);

    NodeLoad load {};
    load.cache.tiers[static_cast<std::size_t>(StorageTier::Memory)] =
        CacheTierUsage { .itemCount = 900, .bytesUsed = 100ULL << 20, .evictions = 3 };
    load.cache.hits = 4000;
    load.cache.misses = 100;
    CHECK(fixture.registry.Heartbeat(gcc, load));
    CHECK(fixture.registry.Heartbeat(clang, load));

    auto const caches = fixture.registry.NodeCaches();
    REQUIRE(caches.size() == 1);
    CHECK(caches[0].endpoint == "10.0.0.2:7100");
    auto const& memory = caches[0].load.tiers[static_cast<std::size_t>(StorageTier::Memory)];
    REQUIRE(memory.has_value());
    // 900 and not 1800, which is the whole point.
    CHECK(Unwrap(memory).itemCount == 900);
    CHECK(Unwrap(memory).bytesUsed == 100ULL << 20);
    CHECK(caches[0].load.hits == 4000);
    CHECK(caches[0].capacity.tierBytesLimit == budget.tierBytesLimit);
}

TEST_CASE("Two machines are two node caches", "[distributed][registry][cache]")
{
    // The other direction, so the dedup above cannot pass by collapsing everything:
    // different endpoints are different nodes, whatever toolchains they serve.
    Fixture fixture;
    (void) fixture.registry.Register(Announce("gcc-14", "10.0.0.2:7100", 4));
    (void) fixture.registry.Register(Announce("gcc-14", "10.0.0.3:7100", 4));

    auto const caches = fixture.registry.NodeCaches();
    REQUIRE(caches.size() == 2);
    // Ordered by endpoint, so a snapshot is reproducible -- the property
    // `LiveWorkers()` sorts for, applied to the key this view groups on.
    CHECK(caches[0].endpoint == "10.0.0.2:7100");
    CHECK(caches[1].endpoint == "10.0.0.3:7100");
}

TEST_CASE("A node that stopped heartbeating stops reporting a cache", "[distributed][registry][cache]")
{
    // A stale figure is worse than none: an operator reading a fleet page cannot
    // tell "this node holds 900 objects" from "this node held 900 objects when it
    // was last heard from, an hour ago".
    Fixture fixture;
    (void) fixture.registry.Register(Announce("gcc-14", "10.0.0.2:7100", 4));
    CHECK(fixture.registry.NodeCaches().size() == 1);

    fixture.clock.Advance(std::chrono::milliseconds { 2000 });
    CHECK(fixture.registry.NodeCaches().empty());
}

TEST_CASE("A node with no cache reports no tiers rather than empty ones", "[distributed][registry][cache]")
{
    // Absent all the way through. A node running with `--cache-memory 0` and no
    // `--cache-dir` has no cache at all, and a leader drawing zeroes for it would
    // show a member whose cache is doing nothing -- a different claim, and one an
    // operator would act on.
    Fixture fixture;
    (void) fixture.registry.Register(Announce("gcc-14", "10.0.0.2:7100", 4));

    auto const caches = fixture.registry.NodeCaches();
    REQUIRE(caches.size() == 1);
    for (auto const& tier: caches[0].capacity.tierBytesLimit)
        CHECK_FALSE(tier.has_value());
    for (auto const& tier: caches[0].load.tiers)
        CHECK_FALSE(tier.has_value());
    CHECK_FALSE(caches[0].load.hits.has_value());
}

TEST_CASE("A sibling that just re-registered does not blank the node's cache", "[distributed][registry][cache]")
{
    // The entries of one node do NOT always agree, which is why picking between
    // them arbitrarily is wrong. `Register` clears a worker's load -- a
    // re-registering worker has restarted, so what it last reported is a reading
    // from before that -- so a node whose gcc entry has just re-registered holds
    // nothing there while its clang entry still holds last round's figures.
    //
    // Choosing the empty one reports that node as having no cache until the next
    // heartbeat, and "no cache" is a claim an operator acts on. Worse, which entry
    // got chosen depended on an unordered_map's iteration order, so it would have
    // been intermittent.
    Fixture fixture;
    auto announce = Announce("gcc-14", "10.0.0.2:7100", 4);
    auto const gcc = fixture.registry.Register(announce);
    announce.fingerprint = "clang-20";
    auto const clang = fixture.registry.Register(announce);

    NodeLoad load {};
    load.cache.tiers[static_cast<std::size_t>(StorageTier::Memory)] = CacheTierUsage { .itemCount = 900 };
    load.cache.hits = 4000;
    CHECK(fixture.registry.Heartbeat(gcc, load));
    CHECK(fixture.registry.Heartbeat(clang, load));

    // One entry re-registers, and its load is reset. It is also now the most
    // recently seen, so "newest wins" alone would pick exactly the wrong one.
    fixture.clock.Advance(std::chrono::milliseconds { 10 });
    announce.fingerprint = "gcc-14";
    CHECK(fixture.registry.Register(announce) == gcc);

    auto const caches = fixture.registry.NodeCaches();
    REQUIRE(caches.size() == 1);
    auto const& memory = caches[0].load.tiers[static_cast<std::size_t>(StorageTier::Memory)];
    REQUIRE(memory.has_value());
    CHECK(Unwrap(memory).itemCount == 900);
    CHECK(caches[0].load.hits == 4000);
}

TEST_CASE("How long ago a worker was heard from is measured on the injected clock",
          "[distributed][registry][heartbeat-age]")
{
    // The age has to come from the clock the registry was given, not from
    // `steady_clock::now()`. Handed a raw `TimePoint`, a consumer would reach for
    // the latter -- right in production and wrong under every one of these cases,
    // silently, because the two clocks agree about nothing.
    Fixture fix;
    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    auto const fresh = fix.registry.LiveWorkerReports();
    REQUIRE(fresh.size() == 1);
    CHECK(fresh[0].info.id == id);
    CHECK(fresh[0].heartbeatAge == std::chrono::milliseconds { 0 });

    fix.clock.Advance(std::chrono::milliseconds { 400 });
    auto const aged = fix.registry.LiveWorkerReports();
    REQUIRE(aged.size() == 1);
    CHECK(aged[0].heartbeatAge == std::chrono::milliseconds { 400 });

    // A heartbeat is what resets it -- that is what the number is reporting.
    CHECK(fix.registry.Heartbeat(id, Busy(1)));
    CHECK(fix.registry.LiveWorkerReports()[0].heartbeatAge == std::chrono::milliseconds { 0 });
}

TEST_CASE("A worker past its timeout is absent rather than very old", "[distributed][registry][heartbeat-age]")
{
    // The distinction a dashboard would otherwise get wrong in the dangerous
    // direction: an expired worker must not appear at all, because a row showing a
    // large age reads as a machine that is merely slow to report.
    Fixture fix;
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    CHECK(fix.registry.LiveWorkerReports().empty());
    CHECK(fix.registry.NodeReports().empty());
}

TEST_CASE("A clock set backwards reports no age rather than an enormous one", "[distributed][registry][heartbeat-age]")
{
    // A `ManualClock` can legitimately go backwards in a test, and an unsigned
    // duration would then read as several hundred million years.
    Fixture fix;
    fix.clock.Advance(std::chrono::milliseconds { 500 });
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.1:6676", 4));

    fix.clock.Advance(std::chrono::milliseconds { -200 });
    auto const reports = fix.registry.LiveWorkerReports();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].heartbeatAge == std::chrono::milliseconds { 0 });
}

TEST_CASE("One machine serving two toolchains is one node, and its cores are not doubled",
          "[distributed][registry][node-report]")
{
    // The double count a fleet page exists to avoid. This registry keys on
    // (fingerprint, endpoint), so a node with two `--toolchain` flags is two
    // entries carrying ONE machine's cores -- and summing them across
    // `LiveWorkers()` reports a fleet twice the size of the one an operator owns.
    //
    // Deliberately different slot counts and job counts per entry, so a max/sum
    // transposition fails rather than passing on equal numbers.
    Fixture fix;
    auto gcc = Announce(Gcc13, "10.0.0.2:7100", 6);
    gcc.capacity = NodeCapacity { .logicalCores = 32, .totalMemoryBytes = 64ULL << 30 };
    auto const gccId = fix.registry.Register(gcc);

    auto clang = Announce(Gcc14, "10.0.0.2:7100", 4);
    clang.capacity = gcc.capacity;
    auto const clangId = fix.registry.Register(clang);

    CHECK(fix.registry.LiveWorkers().size() == 2);
    fix.registry.JobStarted(gccId);
    fix.registry.JobStarted(gccId);
    fix.registry.JobStarted(clangId);

    auto const nodes = fix.registry.NodeReports();
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].endpoint == "10.0.0.2:7100");
    // 32, not 64: the entries describe one machine.
    CHECK(nodes[0].capacity.logicalCores == 32);
    // The maximum, because both were derived from the same cores.
    CHECK(nodes[0].registeredSlots == 6);
    // The sum, because those genuinely are three different jobs.
    CHECK(nodes[0].fleetJobsInFlight == 3);
    // Both toolchains named, sorted so the column order cannot move between reads.
    REQUIRE(nodes[0].fingerprints.size() == 2);
    CHECK(nodes[0].fingerprints[0] == Gcc13);
    CHECK(nodes[0].fingerprints[1] == Gcc14);
}

TEST_CASE("Two machines are two node reports", "[distributed][registry][node-report]")
{
    // The other direction, so the grouping above cannot pass by collapsing
    // everything: different endpoints are different machines.
    Fixture fix;
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.2:7100", 4));
    (void) fix.registry.Register(Announce(Gcc13, "10.0.0.3:7100", 4));

    auto const nodes = fix.registry.NodeReports();
    REQUIRE(nodes.size() == 2);
    // Ordered by endpoint, so a snapshot is reproducible.
    CHECK(nodes[0].endpoint == "10.0.0.2:7100");
    CHECK(nodes[1].endpoint == "10.0.0.3:7100");
}

TEST_CASE("A node cache report carries how stale its figures are", "[distributed][registry][cache]")
{
    // "This cache is empty" and "this node stopped answering and these are its last
    // figures" look identical without an age, and lead to opposite conclusions.
    Fixture fix;
    auto const id = fix.registry.Register(Announce(Gcc13, "10.0.0.2:7100", 4));
    NodeLoad load {};
    load.cache.hits = 10;
    CHECK(fix.registry.Heartbeat(id, load));

    fix.clock.Advance(std::chrono::milliseconds { 250 });
    auto const caches = fix.registry.NodeCaches();
    REQUIRE(caches.size() == 1);
    CHECK(caches[0].heartbeatAge == std::chrono::milliseconds { 250 });
    CHECK(caches[0].load.hits == 10);
}

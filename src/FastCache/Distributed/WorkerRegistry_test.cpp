// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace FastCache;
using namespace FastCache::Distributed;

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
    CHECK(fix.registry.Heartbeat(id, 3));
    fix.clock.Advance(std::chrono::milliseconds { 900 });

    auto const picked = fix.registry.Pick(Gcc13);
    REQUIRE(picked.has_value());
    CHECK(picked->inFlight == 3);
}

TEST_CASE("A heartbeat from an unknown worker is refused", "[distributed][registry]")
{
    // So the worker learns to register again rather than heartbeating into a void.
    Fixture fix;
    CHECK_FALSE(fix.registry.Heartbeat("w999", 0));
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

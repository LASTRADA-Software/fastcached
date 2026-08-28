// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/LeaseTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <ranges>
#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::Unwrap;

namespace
{

/// A lease table over a clock the test drives. Expiry is the whole behaviour
/// here, so a real clock would make every case both slow and flaky.
struct Fixture
{
    ManualClock clock;
    LeaseTable leases { clock, std::chrono::milliseconds { 1000 } };
};

} // namespace

TEST_CASE("A lease is granted and can be found by its token", "[distributed][lease]")
{
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());
    CHECK(Unwrap(lease).key == "objkey-1");
    CHECK(Unwrap(lease).workerId == "w1");

    auto const found = fix.leases.Find(Unwrap(lease).token);
    REQUIRE(found.has_value());
    CHECK(Unwrap(found).token == Unwrap(lease).token);
}

TEST_CASE("The same key is not leased twice at once", "[distributed][lease]")
{
    // Duplicate-work suppression, which is the thing neither distcc nor
    // sccache-dist can do because neither is also the cache. When sixty parallel
    // clients miss the same key after a header change -- the ordinary shape of a
    // miss on a shared cache -- only the first should be dispatched.
    Fixture fix;
    REQUIRE(fix.leases.Acquire("objkey-1", "w1").has_value());
    CHECK_FALSE(fix.leases.Acquire("objkey-1", "w2").has_value());
}

TEST_CASE("Different keys lease independently", "[distributed][lease]")
{
    Fixture fix;
    CHECK(fix.leases.Acquire("objkey-1", "w1").has_value());
    CHECK(fix.leases.Acquire("objkey-2", "w1").has_value());
    CHECK(fix.leases.LiveCount() == 2);
}

TEST_CASE("Finding does not consume the lease", "[distributed][lease]")
{
    // A worker validates the token before it starts, and the job is only resolved
    // when it finishes. Consuming at validation would release the key while the
    // compile was still running, letting a second client dispatch the same work.
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());

    CHECK(fix.leases.Find(Unwrap(lease).token).has_value());
    CHECK(fix.leases.Find(Unwrap(lease).token).has_value());
    CHECK_FALSE(fix.leases.Acquire("objkey-1", "w2").has_value());
}

TEST_CASE("Releasing a lease frees its key for the next client", "[distributed][lease]")
{
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());

    auto const released = fix.leases.Release(Unwrap(lease).token, "objkey-1");
    REQUIRE(released.has_value());
    CHECK(Unwrap(released).key == "objkey-1");
    CHECK(fix.leases.LiveCount() == 0);
    CHECK(fix.leases.Acquire("objkey-1", "w2").has_value());
}

TEST_CASE("Releasing an unknown token is a no-op, not a crash", "[distributed][lease]")
{
    Fixture fix;
    CHECK_FALSE(fix.leases.Release("nope", "objkey-1").has_value());
}

TEST_CASE("An abandoned lease expires and the key becomes leasable again", "[distributed][lease]")
{
    // A client can die between taking a lease and sending the job -- Ctrl-C on a
    // build is the common case, not a rare one. Without expiry that key would be
    // marked in-flight forever, so one interrupted build would permanently
    // un-distribute a translation unit.
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());

    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    CHECK_FALSE(fix.leases.Find(Unwrap(lease).token).has_value());
    CHECK(fix.leases.LiveCount() == 0);

    auto const second = fix.leases.Acquire("objkey-1", "w2");
    REQUIRE(second.has_value());
    CHECK(Unwrap(second).token != Unwrap(lease).token);
}

TEST_CASE("Releasing an expired lease does not evict the client that replaced it", "[distributed][lease]")
{
    // The subtle one. Once a lease expires, its key may already have been re-leased
    // to somebody else. A late Release from the ORIGINAL holder must not remove
    // that mapping -- doing so would free a live lease and let a third client
    // dispatch work the second is currently running.
    Fixture fix;
    auto const first = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(first.has_value());

    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    auto const second = fix.leases.Acquire("objkey-1", "w2");
    REQUIRE(second.has_value());

    (void) fix.leases.Release(Unwrap(first).token, "objkey-1"); // the abandoned client finally reports
    CHECK(fix.leases.Find(Unwrap(second).token).has_value());
    CHECK_FALSE(fix.leases.Acquire("objkey-1", "w3").has_value());
}

TEST_CASE("A token that names another key resolves nothing, and erases nothing", "[distributed][lease]")
{
    // The token is a small integer this table minted, and `_nextToken` starts again
    // at one in a table that has just been constructed -- so a client reporting a
    // job it began before the scheduler restarted arrives holding a number this
    // instance has since issued to somebody else. Resolving on the number alone
    // would free a key that is being built and decrement a worker that is busy.
    //
    // Nothing is erased either: the entry belongs to whoever legitimately holds that
    // number, and they must still be able to resolve it themselves.
    Fixture fix;
    auto const mine = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(mine.has_value());

    CHECK_FALSE(fix.leases.Release(Unwrap(mine).token, "some-other-key").has_value());

    CHECK(fix.leases.IsInFlight("objkey-1"));
    CHECK(fix.leases.Release(Unwrap(mine).token, "objkey-1").has_value());
}

TEST_CASE("An expired lease reports as gone rather than as freed", "[distributed][lease]")
{
    // The case the key index cannot show: nothing re-leased this key, so the entry
    // is still sitting in the token map when its holder finally reports. Presence is
    // not liveness -- the key stopped being suppressed when the lifetime ran out --
    // and answering as though this call had freed something would hide the one
    // condition worth reporting: a job that outlived its lease, which is a timeout
    // shorter than the fleet's slowest translation unit.
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());

    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    CHECK_FALSE(fix.leases.Release(Unwrap(lease).token, "objkey-1").has_value());

    // And the entry went with it rather than being left for a later `Acquire` to
    // sweep -- observable as the key still leasing cleanly, under a NEW token.
    auto const next = fix.leases.Acquire("objkey-1", "w2");
    REQUIRE(next.has_value());
    CHECK(Unwrap(next).token != Unwrap(lease).token);
    CHECK(fix.leases.LiveCount() == 1);
}

TEST_CASE("Dropping a worker releases every lease held against it", "[distributed][lease]")
{
    // Without this, a worker dying mid-job leaves its keys marked in-flight until
    // each lease expires, and every client that misses on one is refused in the
    // meantime -- losing a machine would quietly stop distributing part of a build.
    Fixture fix;
    REQUIRE(fix.leases.Acquire("objkey-1", "w1").has_value());
    REQUIRE(fix.leases.Acquire("objkey-2", "w1").has_value());
    REQUIRE(fix.leases.Acquire("objkey-3", "w2").has_value());

    CHECK(fix.leases.ReleaseWorker("w1") == 2);
    CHECK(fix.leases.LiveCount() == 1);
    CHECK(fix.leases.Acquire("objkey-1", "w2").has_value());
    CHECK_FALSE(fix.leases.Acquire("objkey-3", "w1").has_value());
}

TEST_CASE("Dropping a worker with no leases is harmless", "[distributed][lease]")
{
    Fixture fix;
    CHECK(fix.leases.ReleaseWorker("w-nobody") == 0);
}

TEST_CASE("Re-leasing an expired key does not leak the old token", "[distributed][lease]")
{
    // Nothing else ever visits an expired token, so if Acquire did not erase both
    // directions the token map would grow without bound over a long-lived daemon.
    Fixture fix;
    auto const first = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(first.has_value());

    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    REQUIRE(fix.leases.Acquire("objkey-1", "w2").has_value());

    // The old token is gone entirely, not merely expired.
    CHECK_FALSE(fix.leases.Release(Unwrap(first).token, "objkey-1").has_value());
}

TEST_CASE("The live leases can be walked, oldest first", "[distributed][lease]")
{
    // The grain `LiveCount()` cannot answer at. A fleet that has stopped making
    // progress shows a number; what an operator needs is which keys are held and by
    // whom -- and since a lease is resolved by the client that took it, one still
    // outstanding after minutes is a client that died mid-build whose worker is
    // still heartbeating.
    Fixture fix;
    REQUIRE(fix.leases.Acquire("oldest", "w1").has_value());
    fix.clock.Advance(std::chrono::milliseconds { 300 });
    REQUIRE(fix.leases.Acquire("middle", "w2").has_value());
    fix.clock.Advance(std::chrono::milliseconds { 100 });
    REQUIRE(fix.leases.Acquire("newest", "w1").has_value());

    auto const held = fix.leases.LiveLeases(10);
    REQUIRE(held.size() == 3);

    // Ordered by descending age, not by insertion or by hash: `_byToken` is an
    // unordered_map, so without the sort this assertion would pass or fail by
    // accident.
    CHECK(held[0].key == "oldest");
    CHECK(held[1].key == "middle");
    CHECK(held[2].key == "newest");

    // The holder travels with it -- the actionable half -- and the age is measured
    // against the injected clock rather than a real one.
    CHECK(held[0].workerId == "w1");
    CHECK(held[1].workerId == "w2");
    CHECK(held[0].age == std::chrono::milliseconds { 400 });
    CHECK(held[2].age == std::chrono::milliseconds { 0 });
}

TEST_CASE("The lease listing is bounded, and keeps the oldest", "[distributed][lease]")
{
    // A fleet at full tilt holds thousands, so any listing is bounded -- and WHICH
    // end it keeps is the whole point: the newest fifty of three thousand would
    // answer nothing, while the oldest are the ones that have stopped moving. The
    // total stays available through `LiveCount()`, so the truncation is visible
    // rather than silent.
    Fixture fix;
    for (auto const index: std::views::iota(0, 5))
    {
        REQUIRE(fix.leases.Acquire(std::format("key-{}", index), "w1").has_value());
        fix.clock.Advance(std::chrono::milliseconds { 10 });
    }

    auto const held = fix.leases.LiveLeases(2);
    REQUIRE(held.size() == 2);
    CHECK(held[0].key == "key-0");
    CHECK(held[1].key == "key-1");
    CHECK(fix.leases.LiveCount() == 5);

    CHECK(fix.leases.LiveLeases(0).empty());
}

TEST_CASE("An expired lease is not listed, even while it is still in the table", "[distributed][lease]")
{
    // Presence is not liveness -- the split `IsInFlight` and `Release` already make.
    // Nothing re-leased this key, so the entry is still sitting in the token map, and
    // listing it would put a lease on the page that stopped suppressing anything.
    Fixture fix;
    REQUIRE(fix.leases.Acquire("abandoned", "w1").has_value());
    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    REQUIRE(fix.leases.Acquire("live-one", "w1").has_value());

    auto const held = fix.leases.LiveLeases(10);
    REQUIRE(held.size() == 1);
    CHECK(held[0].key == "live-one");
}

TEST_CASE("A resolved lease leaves the listing at once", "[distributed][lease]")
{
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());
    REQUIRE(fix.leases.LiveLeases(10).size() == 1);

    REQUIRE(fix.leases.Release(Unwrap(lease).token, "objkey-1").has_value());
    CHECK(fix.leases.LiveLeases(10).empty());
}

TEST_CASE("A clock that moves backwards does not expire live leases", "[distributed][lease]")
{
    Fixture fix;
    fix.clock.Advance(std::chrono::milliseconds { 5000 });
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());

    fix.clock.Advance(std::chrono::milliseconds { -2000 });
    CHECK(fix.leases.Find(Unwrap(lease).token).has_value());
}

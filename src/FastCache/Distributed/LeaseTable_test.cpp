// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/LeaseTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
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

    auto const released = fix.leases.Release(Unwrap(lease).token);
    REQUIRE(released.has_value());
    CHECK(Unwrap(released).key == "objkey-1");
    CHECK(fix.leases.LiveCount() == 0);
    CHECK(fix.leases.Acquire("objkey-1", "w2").has_value());
}

TEST_CASE("Releasing an unknown token is a no-op, not a crash", "[distributed][lease]")
{
    Fixture fix;
    CHECK_FALSE(fix.leases.Release("nope").has_value());
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

    (void) fix.leases.Release(Unwrap(first).token); // the abandoned client finally reports
    CHECK(fix.leases.Find(Unwrap(second).token).has_value());
    CHECK_FALSE(fix.leases.Acquire("objkey-1", "w3").has_value());
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
    CHECK_FALSE(fix.leases.Release(Unwrap(first).token).has_value());
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

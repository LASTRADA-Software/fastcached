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
    CHECK(fix.leases.LiveLeases(0).total == 2);
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
    CHECK(released->key == "objkey-1");
    CHECK(fix.leases.LiveLeases(0).total == 0);
    CHECK(fix.leases.Acquire("objkey-1", "w2").has_value());
}

TEST_CASE("Releasing an unknown token is a no-op, not a crash", "[distributed][lease]")
{
    Fixture fix;
    CHECK_FALSE(fix.leases.Release("nope", "objkey-1").has_value());
}

TEST_CASE("The three ways a release resolves nothing are told apart", "[distributed][lease]")
{
    // Asserted against EACH OTHER in one case rather than one section per arm, which
    // is the whole point: all three used to be one `nullopt`, so every per-arm
    // assertion available at the time passed on a table that could not tell them
    // apart -- and the scheduler above, having nothing to distinguish, duly counted
    // none of them and left a fleet unable to observe the one condition this file
    // already documented as worth reporting (#1074).
    //
    // Three refusals in one fixture, in an order that keeps each arrangement honest:
    // the key mismatch is taken while its lease is still LIVE, so it is answering
    // about the key rather than about expiry -- `Release` checks the key first, so a
    // mismatched release of an already-expired token would pass this case while
    // proving nothing about which check fired.
    Fixture fix;

    auto const mine = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(mine.has_value());
    auto const other = fix.leases.Acquire("objkey-2", "w1");
    REQUIRE(other.has_value());

    // Never issued: no entry under that token at all. What a second release of one
    // token also looks like, the first having erased the entry.
    auto const unknown = fix.leases.Release("no-such-token", "objkey-1");

    // Issued, live, and naming somebody else's key -- what a token minted by a
    // previous instance looks like once this one has reissued its number.
    auto const mismatched = fix.leases.Release(Unwrap(mine).token, "objkey-2");

    // Resolved nothing WITHOUT erasing, or the refusal would have freed the live
    // lease whose number it collided with, letting a third client dispatch work
    // somebody is already doing. Asked here rather than at the end of the case,
    // because the clock advance below expires this lease too and the question stops
    // meaning anything the moment it does.
    CHECK_FALSE(fix.leases.Acquire("objkey-1", "w2").has_value());

    // Issued, this caller's own key, and past its lifetime: the job outlived it.
    fix.clock.Advance(std::chrono::milliseconds { 1001 });
    auto const expired = fix.leases.Release(Unwrap(other).token, "objkey-2");

    REQUIRE_FALSE(unknown.has_value());
    REQUIRE_FALSE(mismatched.has_value());
    REQUIRE_FALSE(expired.has_value());

    CHECK(unknown.error() == LeaseTable::ReleaseRefusal::UnknownToken);
    CHECK(mismatched.error() == LeaseTable::ReleaseRefusal::KeyMismatch);
    CHECK(expired.error() == LeaseTable::ReleaseRefusal::Expired);

    // The discrimination itself. Each assertion above names one enumerator and would
    // go on passing if two of the three collapsed into it; only comparing them
    // rejects a table that answers any pair identically.
    CHECK(unknown.error() != mismatched.error());
    CHECK(mismatched.error() != expired.error());
    CHECK(unknown.error() != expired.error());
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
    CHECK(fix.leases.LiveLeases(0).total == 0);

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
    CHECK(fix.leases.LiveLeases(0).total == 1);
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
    CHECK(fix.leases.LiveLeases(0).total == 1);
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
    // The grain a count cannot answer at. A fleet that has stopped making
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

    auto const held = fix.leases.LiveLeases(10).oldest;
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
    // total travels beside the listing, sampled under the same lock, so the
    // truncation is visible rather than silent.
    Fixture fix;
    for (auto const index: std::views::iota(0, 5))
    {
        REQUIRE(fix.leases.Acquire(std::format("key-{}", index), "w1").has_value());
        fix.clock.Advance(std::chrono::milliseconds { 10 });
    }

    auto const listing = fix.leases.LiveLeases(2);
    REQUIRE(listing.oldest.size() == 2);
    CHECK(listing.oldest[0].key == "key-0");
    CHECK(listing.oldest[1].key == "key-1");
    // The total comes back from the SAME call, under the same lock: asked
    // separately the two can disagree, and the truncation notice a reader relies on
    // is then wrong in exactly the situation it exists for.
    CHECK(listing.total == 5);

    CHECK(fix.leases.LiveLeases(0).oldest.empty());
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

    auto const held = fix.leases.LiveLeases(10).oldest;
    REQUIRE(held.size() == 1);
    CHECK(held[0].key == "live-one");
}

TEST_CASE("A resolved lease leaves the listing at once", "[distributed][lease]")
{
    Fixture fix;
    auto const lease = fix.leases.Acquire("objkey-1", "w1");
    REQUIRE(lease.has_value());
    REQUIRE(fix.leases.LiveLeases(10).oldest.size() == 1);

    REQUIRE(fix.leases.Release(Unwrap(lease).token, "objkey-1").has_value());
    CHECK(fix.leases.LiveLeases(10).oldest.empty());
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

TEST_CASE("A lease keeps the bound it was granted under when a later one is shorter", "[distributed][lease]")
{
    // **#522's second decision, made structural rather than asserted in prose.** The
    // lifetime is a replicated setting a cluster may change while work is in flight. A
    // table that judged live entries against the CURRENT value would move the bound of a
    // grant already minted -- and the client and the worker are both holding a token
    // whose expiry came from the old one, so the three ends would disagree about when
    // the job is over. That is the disagreement this ticket exists to make impossible.
    Fixture fix;

    using namespace std::chrono_literals;
    auto const generous = fix.leases.Acquire("objkey-long", "w1", 10'000ms);
    REQUIRE(generous.has_value());
    CHECK(Unwrap(generous).lifetime == 10'000ms);

    // The cluster shortens the setting. Every lease taken from here on is short.
    auto const brief = fix.leases.Acquire("objkey-short", "w1", 200ms);
    REQUIRE(brief.has_value());

    // Past the NEW bound and well inside the OLD one.
    fix.clock.Advance(1'000ms);

    // The one that matters: the in-flight generous lease is still live, judged against
    // what it was granted under. Read through `Find`, which is the call a worker's
    // authorization goes through, rather than through a getter no production path uses.
    CHECK(fix.leases.Find(Unwrap(generous).token).has_value());
    CHECK(fix.leases.IsInFlight("objkey-long"));

    // The control, and it is what makes the assertion above mean something: a table that
    // simply never expired anything would pass every line up to here. The short lease
    // taken under the NEW value is gone at the same instant, from the same table, under
    // the same clock -- so what is being observed is the per-entry bound and not a
    // liveness check that stopped working.
    CHECK_FALSE(fix.leases.Find(Unwrap(brief).token).has_value());
    CHECK_FALSE(fix.leases.IsInFlight("objkey-short"));
}

TEST_CASE("A lease granted under a longer bound is not expired early by a shorter default", "[distributed][lease]")
{
    // The mirror, because the failure has two directions and only one of them is
    // reachable from the case above. A table holding `_leaseTimeout` and consulting it
    // per entry expires a GENEROUS in-flight lease the moment the setting drops; one
    // that ignores the granted value altogether keeps a BRIEF lease alive far too long,
    // suppressing a key nobody is building. Both are one-line implementations away.
    using namespace std::chrono_literals;
    Fixture fix; // constructed with a 1000ms default

    auto const brief = fix.leases.Acquire("objkey-brief", "w1", 100ms);
    REQUIRE(brief.has_value());

    fix.clock.Advance(500ms); // past the grant, inside the table's own default

    CHECK_FALSE(fix.leases.Find(Unwrap(brief).token).has_value());

    // And the key is free again, which is the operator-visible half: a lease that
    // outlives its grant suppresses duplicate work for a job nobody is doing.
    auto const regranted = fix.leases.Acquire("objkey-brief", "w2", 100ms);
    CHECK(regranted.has_value());
}

TEST_CASE("A caller that names no lifetime gets the table's own, which is the one-machine install", "[distributed][lease]")
{
    // The deployment with no replicated state to read: one node, no consensus, nothing
    // to agree with. It must keep working with no configuration at all, so the
    // convenience overload is asserted rather than assumed -- and asserted through the
    // OBSERVABLE bound rather than through `Timeout()`, which would pass against an
    // overload that recorded the default and judged against something else.
    using namespace std::chrono_literals;
    Fixture fix; // 1000ms

    auto const lease = fix.leases.Acquire("objkey-default", "w1");
    REQUIRE(lease.has_value());
    CHECK(Unwrap(lease).lifetime == fix.leases.Timeout());

    fix.clock.Advance(900ms);
    CHECK(fix.leases.Find(Unwrap(lease).token).has_value());
    fix.clock.Advance(200ms);
    CHECK_FALSE(fix.leases.Find(Unwrap(lease).token).has_value());
}

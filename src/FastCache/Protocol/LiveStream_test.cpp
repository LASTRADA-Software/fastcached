// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

TEST_CASE("Events are the differences between two probes, keyed and in table order", "[livestats]")
{
    LiveEventProbe before;
    before.members = { { .key = "n1", .detail = "n1" }, { .key = "n2", .detail = "n2" } };
    before.leadership = LiveFact { .key = "3|", .detail = "" };
    before.survey = LiveFact { .key = "1", .detail = "1 of 4" };

    auto after = before;
    CHECK(DiffLiveEvents(before, after).empty());

    // The words changed and the key did not: not an event, or a survey would be one per toolchain.
    after.survey = LiveFact { .key = "1", .detail = "3 of 4" };
    CHECK(DiffLiveEvents(before, after).empty());

    after.members = { { .key = "n2", .detail = "n2" }, { .key = "n3", .detail = "n3" } };
    after.leadership = LiveFact { .key = "1|n3:6674", .detail = "n3:6674" };
    after.enrollment = LiveFact { .key = "2", .detail = "0 pending" };

    auto const events = DiffLiveEvents(before, after);
    REQUIRE(events.size() == 4);
    CHECK(events[0].kind == Wire::LiveEventKind::MemberJoined);
    CHECK(events[0].detail == "n3");
    CHECK(events[1].kind == Wire::LiveEventKind::MemberLeft);
    CHECK(events[1].detail == "n1");
    CHECK(events[2].kind == Wire::LiveEventKind::LeadershipChanged);
    CHECK(events[2].detail == "n3:6674");
    // Appearing is a change: a window that did not exist and now does.
    CHECK(events[3].kind == Wire::LiveEventKind::EnrollmentChanged);
}

TEST_CASE("A gap is whole cadences missed, never wake-up jitter", "[livestats]")
{
    LiveCursor cursor { .nextDue = 10, .ticksPerCadence = 2 };

    CHECK_FALSE(DecideLiveStep(cursor, 9, false).snapshot);

    auto const onTime = DecideLiveStep(cursor, 10, false);
    CHECK(onTime.snapshot);
    CHECK_FALSE(onTime.gap.has_value());
    CHECK(cursor.nextDue == 12);

    // One tick late is less than a cadence: jitter, and no hole in the panel.
    auto const jitter = DecideLiveStep(cursor, 13, false);
    CHECK(jitter.snapshot);
    CHECK_FALSE(jitter.gap.has_value());
    CHECK(cursor.nextDue == 15);

    auto const parked = DecideLiveStep(cursor, 21, false);
    CHECK(parked.snapshot);
    REQUIRE(parked.gap.has_value());
    CHECK(Unwrap(parked.gap).dropped == 3);
    CHECK(Unwrap(parked.gap).firstTick == 15);
    CHECK(Unwrap(parked.gap).lastTick == 19);

    // An event owes a snapshot whatever the cadence says, and restarts the cadence from there.
    auto const forced = DecideLiveStep(cursor, 22, true);
    CHECK(forced.snapshot);
    CHECK_FALSE(forced.gap.has_value());
    CHECK(cursor.nextDue == 24);
}

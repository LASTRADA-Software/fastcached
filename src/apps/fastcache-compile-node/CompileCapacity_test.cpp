// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string_view>
#include <thread>

using namespace FastCache;
using namespace FastCache::Node;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The words the drained report is recognised by.
constexpr std::string_view DrainedReport = "cordoned and drained";

/// A worker with @p slots slots and a budget nothing in these cases reaches.
/// @param slots The slot cap.
/// @param logger Where the cordon's reports land.
/// @return The accounting.
[[nodiscard]] CompileCapacity WorkerWith(std::size_t slots, ILogger& logger)
{
    return CompileCapacity { slots, WorkerMaxRequestBytes, std::chrono::seconds { 5 }, logger };
}

/// The words the abandonment report is recognised by.
constexpr std::string_view AbandonReport = "giving up after";

/// The words the still-waiting report is recognised by.
///
/// The report line's TAIL, not its opening `waiting for`: the abandonment line above reads
/// "... ending now rather than waiting for the supervisor to kill this process", so the
/// obvious needle is a substring of BOTH reports and separates neither. A needle that the
/// opposite outcome also produces is not a needle.
constexpr std::string_view WaitingReport = "compile(s) to finish before stopping";

/// How many records in @p logger carry @p phrase.
/// @param logger The capture.
/// @param phrase What to look for.
/// @return The count.
[[nodiscard]] std::size_t Reports(CapturingLogger const& logger, std::string_view phrase)
{
    auto const records = logger.Snapshot();
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [phrase](CapturingLogger::Record const& record) { return record.message.contains(phrase); }));
}

/// How many drained reports @p logger has captured.
/// @param logger The capture.
/// @return The count.
[[nodiscard]] std::size_t DrainedReports(CapturingLogger const& logger)
{
    return Reports(logger, DrainedReport);
}

/// An abandonment that RETURNS and counts, so a case can drive `Drain` past its bound and
/// see what it did instead of watching the test binary end.
///
/// **More permissive than the seam it stands for**, exactly as `ExpiryReaper_test`'s is:
/// production does not come back from here, because a compile still running holds a
/// pointer into the capacity and returning frees it underneath the compile. A case pays
/// for the difference by holding the in-flight count with `TryTakeSlot` and NO running
/// job -- so nothing is borrowing the object when `Drain` returns, and the permissiveness
/// costs nothing (#297).
class RecordingAbandonment final: public IDrainAbandonment
{
  public:
    void Abandon() noexcept override
    {
        calls.fetch_add(1, std::memory_order_acq_rel);
    }

    std::atomic<int> calls { 0 }; ///< How many times `Drain` abandoned a compile.
};

} // namespace

TEST_CASE("A cordon refuses a new compile and leaves the running ones their slots", "[node][capacity][cordon]")
{
    CapturingLogger logger;
    auto capacity = WorkerWith(4, logger);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);

    auto const answered = capacity.Cordon(true);
    CHECK(answered.state == Wire::WireCordonState::Draining);
    CHECK(answered.inFlight == 2);

    // Refused as CORDONED with two slots free: a refusal decided by the slot count would
    // say `Taken` here, and one folded into the full case would say `Full`.
    CHECK(capacity.TryTakeSlot() == SlotAdmission::Cordoned);
    CHECK(capacity.InFlight() == 2);
    CHECK(capacity.CordonState() == Wire::WireCordonState::Draining);

    capacity.ReleaseSlot();
    capacity.ReleaseSlot();
    CHECK(capacity.CordonState() == Wire::WireCordonState::Drained);
}

TEST_CASE("The drained report fires once, and only at the last release", "[node][capacity][cordon]")
{
    CapturingLogger logger;
    auto capacity = WorkerWith(4, logger);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);

    (void) capacity.Cordon(true);
    CHECK(DrainedReports(logger) == 0);

    // One of two finishing says nothing an operator waiting to reboot can act on.
    capacity.ReleaseSlot();
    CHECK(DrainedReports(logger) == 0);

    capacity.ReleaseSlot();
    CHECK(DrainedReports(logger) == 1);

    // Asking again for the state it already has changes nothing and reports nothing, so a
    // script that cordons twice does not log a second drained line.
    CHECK(capacity.Cordon(true).state == Wire::WireCordonState::Drained);
    CHECK(DrainedReports(logger) == 1);
}

TEST_CASE("A cordon on an idle worker reports it drained at once", "[node][capacity][cordon]")
{
    CapturingLogger logger;
    auto capacity = WorkerWith(2, logger);

    auto const answered = capacity.Cordon(true);

    CHECK(answered.state == Wire::WireCordonState::Drained);
    CHECK(answered.inFlight == 0);
    CHECK(DrainedReports(logger) == 1);
}

TEST_CASE("A release on a worker nobody cordoned reports nothing", "[node][capacity][cordon]")
{
    // The control for the two cases above: a drained line from every release to zero would
    // pass both of them.
    CapturingLogger logger;
    auto capacity = WorkerWith(2, logger);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
    capacity.ReleaseSlot();

    CHECK(DrainedReports(logger) == 0);
    CHECK(capacity.CordonState() == Wire::WireCordonState::Serving);
}

TEST_CASE("Lifting a cordon takes compiles again", "[node][capacity][cordon]")
{
    CapturingLogger logger;
    auto capacity = WorkerWith(1, logger);
    (void) capacity.Cordon(true);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Cordoned);

    CHECK(capacity.Cordon(false).state == Wire::WireCordonState::Serving);
    CHECK_FALSE(capacity.IsCordoned());
    CHECK(capacity.TryTakeSlot() == SlotAdmission::Taken);
    // And the one slot is now spent, which is the full answer rather than the cordoned one.
    CHECK(capacity.TryTakeSlot() == SlotAdmission::Full);
}

TEST_CASE("A stopping worker says it is stopping, cordoned or not", "[node][capacity][cordon]")
{
    // A stop ends by itself and a cordon does not, and a client does the same thing with
    // both -- but the counters differ, and a cordoned node being stopped is being stopped.
    CapturingLogger logger;
    auto capacity = WorkerWith(2, logger);
    (void) capacity.Cordon(true);
    capacity.BeginShutdown();

    CHECK(capacity.TryTakeSlot() == SlotAdmission::Stopping);
}

TEST_CASE("A restart un-cordons: the cordon is the process's, and nothing carries it over", "[node][capacity][cordon]")
{
    // Two workers in sequence stand in for one node before and after a restart. The first
    // is cordoned and gone; the second must take compiles, because a cordon that survived
    // a restart would be a machine that silently never came back to the fleet.
    CapturingLogger logger;
    {
        auto before = WorkerWith(2, logger);
        (void) before.Cordon(true);
        REQUIRE(before.IsCordoned());
    }

    auto after = WorkerWith(2, logger);
    CHECK_FALSE(after.IsCordoned());
    CHECK(after.CordonState() == Wire::WireCordonState::Serving);
    CHECK(after.TryTakeSlot() == SlotAdmission::Taken);
}

TEST_CASE("A cordon wakes the heartbeat's wait at once, and a stop wins over it", "[node][capacity][cordon]")
{
    // The scheduler hears a cordon from a heartbeat and from nothing else, so a wait that
    // noticed it only when the interval ran out would leave this worker leased for that
    // long.
    //
    // **The verdict is what the wait DECIDED, never how long it took.** A wall-clock bound
    // reads a slow host as a regression and lets a fast one hide it; `WaitForHeartbeat`
    // answers `Elapsed` for a cordon that moved without waking it, so a wait nobody woke
    // fails on the reason. The elapsed time is kept only as a guard against a wait that
    // outlives its own interval, and says so when it fires.
    constexpr auto LongInterval = std::chrono::seconds { 30 };
    constexpr auto HangGuard = LongInterval + std::chrono::seconds { 30 };
    CapturingLogger logger;
    auto capacity = WorkerWith(1, logger);
    std::stop_source stop;

    SECTION("a cordon from another thread ends a long wait as CordonChanged")
    {
        auto const started = std::chrono::steady_clock::now();
        auto cordoner = std::jthread { [&capacity] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
            (void) capacity.Cordon(true);
        } };
        auto const wake = capacity.WaitForHeartbeat(stop.get_token(), false, LongInterval);
        auto const waited = std::chrono::steady_clock::now() - started;
        cordoner.join();

        CHECK(wake == HeartbeatWake::CordonChanged);
        INFO("HANG GUARD, not the verdict (the wake reason is): the wait took "
             << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count() << " ms of a " << LongInterval.count()
             << " s interval");
        CHECK(waited < HangGuard);
    }

    SECTION("lifting a cordon wakes it too")
    {
        (void) capacity.Cordon(true);
        auto lifter = std::jthread { [&capacity] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
            (void) capacity.Cordon(false);
        } };
        auto const started = std::chrono::steady_clock::now();
        auto const wake = capacity.WaitForHeartbeat(stop.get_token(), true, LongInterval);
        auto const waited = std::chrono::steady_clock::now() - started;
        lifter.join();

        CHECK(wake == HeartbeatWake::CordonChanged);
        INFO("HANG GUARD, not the verdict (the wake reason is): the wait took "
             << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count() << " ms of a " << LongInterval.count()
             << " s interval");
        CHECK(waited < HangGuard);
    }

    SECTION("a cordon that already differs from the announced one is not waited for")
    {
        (void) capacity.Cordon(true);
        CHECK(capacity.WaitForHeartbeat(stop.get_token(), false, LongInterval) == HeartbeatWake::CordonChanged);
    }

    SECTION("control: nothing moving lets the interval elapse")
    {
        CHECK(capacity.WaitForHeartbeat(stop.get_token(), false, std::chrono::milliseconds { 20 })
              == HeartbeatWake::Elapsed);
    }

    SECTION("a stop wins, even over a cordon that moved as well")
    {
        (void) capacity.Cordon(true);
        stop.request_stop();
        CHECK(capacity.WaitForHeartbeat(stop.get_token(), false, LongInterval) == HeartbeatWake::Stopped);
    }
}

TEST_CASE("A drain whose bound runs out abandons what is running, and says what it abandoned", "[node][capacity][drain]")
{
    // **The arm this repository recorded as untestable.** `DrainAction::Abandon` used to
    // call `std::_Exit` inline, so a case that reached it did not fail -- it took the whole
    // test binary with it, along with every case that would have run after it in that
    // process. The reason written down for splitting `NextDrainAction` out was "a side
    // effect no test can survive is one no test will check", which was a true statement
    // about that LINE and was recorded as a property of the ARM (#297).
    //
    // It is the line. `IDrainAbandonment` already existed for `ExpiryReaper`, and
    // `BoundedDrain.hpp` already named THIS drain as the shape it modelled -- while this
    // drain was the one class that could not take it.
    //
    // **The bound here is internal and this is what it costs.** `Drain` wakes on
    // `DrainReportInterval` (2 s) and `NextDrainAction` abandons once the elapsed time
    // reaches `drainTimeout`, whose unit is whole seconds -- so the abandoning section
    // costs one report interval, ~2 s, and cannot be made cheaper without changing either
    // constant. The elapsed time is CHECKED against a ceiling and reported, so a drain that
    // comes back late fails with a number. What that cannot catch is a drain that does not
    // come back at all -- a future edit dropping the `return` after `Abandon()` would loop
    // -- and that presents as a ctest timeout rather than a red. Stated rather than
    // defended against: running the drain off-thread to bound the join would hand this case
    // a use-after-free hazard that the on-thread call does not have, which is the worse
    // trade for a failure mode the next run names anyway.
    constexpr auto Bound = std::chrono::seconds { 1 };
    constexpr auto Ceiling = std::chrono::seconds { 30 };
    CapturingLogger logger;
    RecordingAbandonment abandonment;
    CompileCapacity capacity { 4, WorkerMaxRequestBytes, Bound, logger, abandonment };

    SECTION("a compile that never gives its slot back is abandoned when the bound is spent")
    {
        REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
        capacity.BeginShutdown();

        auto const started = std::chrono::steady_clock::now();
        capacity.Drain();
        auto const waited = std::chrono::steady_clock::now() - started;

        // Exactly one: the `return` after `Abandon()` is what stops the loop coming round
        // to abandon the same compile again, and a count of one is what asserts it.
        CHECK(abandonment.calls.load() == 1);

        // The operator-facing half. An abandonment that ends the process silently is the
        // `SIGKILL` this whole bound exists to replace, so the line saying WHAT was
        // abandoned is part of the behaviour, not decoration.
        CHECK(Reports(logger, AbandonReport) == 1);
        auto const records = logger.Snapshot();
        CHECK(std::ranges::any_of(records, [](CapturingLogger::Record const& record) {
            return record.message.contains(AbandonReport) && record.message.contains("1 compile(s) still running");
        }));

        INFO("CEILING, not the verdict (the abandonment count is): the drain took "
             << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count() << " ms on a " << Bound.count()
             << " s bound");
        CHECK(waited < Ceiling);
    }

    SECTION("control: a compile that gives its slot back drains without abandoning anything")
    {
        // **The discrimination.** The section above passes under a `Drain` that abandons
        // unconditionally, and under one that abandons on every wake -- so on its own it
        // says only that the arm is reachable, not that it is reached for the right
        // reason. This is the same object, the same bound and the same seam, differing
        // only in whether the slot comes back.
        REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
        capacity.BeginShutdown();
        auto releaser = std::jthread { [&capacity] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
            capacity.ReleaseSlot();
        } };

        capacity.Drain();
        releaser.join();

        CHECK(abandonment.calls.load() == 0);
        CHECK(Reports(logger, AbandonReport) == 0);
    }

    SECTION("control: a worker with nothing running drains at once and abandons nothing")
    {
        capacity.BeginShutdown();
        capacity.Drain();

        CHECK(abandonment.calls.load() == 0);
        CHECK(Reports(logger, AbandonReport) == 0);
    }
}

TEST_CASE("A drain still inside its bound says what it is waiting for and abandons nothing", "[node][capacity][drain]")
{
    // **The third arm, and the one the other case cannot see.** Its three sections reach
    // `Finished` and `Abandon` and never `Report`, so a `Drain` that called `Abandon()` on
    // the reporting arm as well -- abandoning a compile while still inside its bound --
    // passes every one of them. Found by neutering, not by reading: the neuter written to
    // check the controls discriminate turned out to be invisible to all four sections.
    //
    // `NextDrainAction`'s own unit tests cannot cover it either. They are exhaustive over
    // the DECISION, and this is a defect in CARRYING IT OUT -- which is the whole reason
    // the arm needed a seam rather than a proxy.
    //
    // **Costs one report interval by construction.** `Report` fires on `DrainReportInterval`
    // (2 s), so a section that observes one cannot be quicker than that; the bound is long
    // enough that the release, not the ceiling, is what ends the drain.
    constexpr auto LongBound = std::chrono::seconds { 30 };
    constexpr auto HoldPast = std::chrono::milliseconds { 2200 };
    CapturingLogger logger;
    RecordingAbandonment abandonment;
    CompileCapacity capacity { 4, WorkerMaxRequestBytes, LongBound, logger, abandonment };

    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
    capacity.BeginShutdown();
    auto releaser = std::jthread { [&capacity, HoldPast] {
        std::this_thread::sleep_for(HoldPast);
        capacity.ReleaseSlot();
    } };

    capacity.Drain();
    releaser.join();

    // A stop that says nothing for its whole bound is indistinguishable from one that has
    // hung, which is the reading the cadence exists to prevent.
    CHECK(Reports(logger, WaitingReport) >= 1);
    CHECK(abandonment.calls.load() == 0);
    CHECK(Reports(logger, AbandonReport) == 0);
}

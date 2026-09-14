// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

/// How many drained reports @p logger has captured.
/// @param logger The capture.
/// @return The count.
[[nodiscard]] std::size_t DrainedReports(CapturingLogger const& logger)
{
    auto const records = logger.Snapshot();
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [](CapturingLogger::Record const& record) { return record.message.contains(DrainedReport); }));
}

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

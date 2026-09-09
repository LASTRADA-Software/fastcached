// SPDX-License-Identifier: Apache-2.0
//
// The wall-clock borrow guard, asserted as a property of the TYPES rather than
// demonstrated by a probe.
//
// Every one of these is a `static_assert` pair, and the pairing is what stops the
// negative half passing vacuously: `!is_constructible_v<T, SystemWallClock>` is also
// true of a type constructible from nothing at all, so each refusal is stated beside
// the acceptance it must not have taken with it.
//
// The FORWARDER rows are the ones worth reading. A deleted `T(IWallClock const&&)`
// overload on each storing type rejects a direct temporary and ACCEPTS one that
// arrived through a forwarding constructor -- measured on gcc and clang -- because
// inside a forwarder the parameter is a named lvalue and binds to the ordinary
// overload. #1028 was exactly that: the temporary went to `FleetSampler`, which
// stores no clock at all. `WallClockRef` is carried BY VALUE, so forwarding copies a
// borrow rather than re-binding a reference and the refusal survives the hop.
//
// Shown red by planting: deleting `WallClockRef(IWallClock const&&) = delete;` fires
// every negative row here and leaves every positive row green.

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string_view>
#include <type_traits>

using namespace FastCache;

namespace
{

// The borrow itself.
static_assert(std::is_constructible_v<WallClockRef, SystemWallClock&>,
              "a named clock must still be borrowable, or the guard refuses every honest caller");
static_assert(!std::is_constructible_v<WallClockRef, SystemWallClock>,
              "a temporary must be refused: the borrow outlives the full expression");
static_assert(std::is_constructible_v<WallClockRef, ManualWallClock&>, "any IWallClock, not just the system one");
static_assert(!std::is_constructible_v<WallClockRef, ManualWallClock>, "including the one tests reach for");

// The retainers, which hold the borrow.
static_assert(std::is_constructible_v<Distributed::FleetHistory, SystemWallClock&>, "lvalue into a retainer");
static_assert(!std::is_constructible_v<Distributed::FleetHistory, SystemWallClock>, "rvalue into a retainer");
static_assert(std::is_constructible_v<Distributed::FleetNodeHistories, SystemWallClock&>, "lvalue into a retainer");
static_assert(!std::is_constructible_v<Distributed::FleetNodeHistories, SystemWallClock>, "rvalue into a retainer");

// A retainer reached through several other arguments -- the shape a real call has.
static_assert(std::is_constructible_v<Distributed::SchedulerService,
                                      ManualClock&,
                                      SystemWallClock&,
                                      AtomicMetricsSink&,
                                      NullLogger&,
                                      std::span<std::byte const>,
                                      std::string_view>,
              "lvalue into the scheduler");
static_assert(!std::is_constructible_v<Distributed::SchedulerService,
                                       ManualClock&,
                                       SystemWallClock,
                                       AtomicMetricsSink&,
                                       NullLogger&,
                                       std::span<std::byte const>,
                                       std::string_view>,
              "rvalue into the scheduler");

static_assert(std::is_constructible_v<CacheEngine, InMemoryLruStorage&, ManualClock&, SystemWallClock&, IMetricsSink*>,
              "lvalue into the engine");
static_assert(!std::is_constructible_v<CacheEngine, InMemoryLruStorage&, ManualClock&, SystemWallClock, IMetricsSink*>,
              "rvalue into the engine");

} // namespace

TEST_CASE("A borrowed wall clock answers as the clock it borrows", "[core][clock][borrow]")
{
    // The guard must not have cost the borrow its job. `Now()` is asserted through the
    // ref against the same clock read directly, on a MANUAL clock so the two readings
    // are the same instant by construction rather than by being close together.
    ManualWallClock clock { std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } } };
    WallClockRef const borrowed { clock };

    CHECK(borrowed.Now() == clock.Now());
    CHECK(&borrowed.Get() == &clock);

    clock.Advance(std::chrono::seconds { 42 });
    CHECK(borrowed.Now() == clock.Now());
}

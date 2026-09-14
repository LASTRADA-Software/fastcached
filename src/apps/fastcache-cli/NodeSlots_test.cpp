// SPDX-License-Identifier: Apache-2.0
//
// A node's slots as the panel works them out from two readings. What these cases hold is that the
// panel names the limit the scheduler's own arithmetic names, and that it says nothing -- rather
// than `registered` -- while it cannot know.
#include "NodeSlots.hpp"

#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <utility>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::Unwrap;

namespace
{

constexpr std::uint64_t GiB = 1ULL << 30;

/// A 16-core node registered with 16 slots, 6 of them running, with room everywhere unless a case
/// takes it away.
[[nodiscard]] HostCapacity SixteenCores()
{
    return HostCapacity { .logicalCores = 16,
                          .configuredSlots = 16,
                          .totalMemoryBytes = 64 * GiB,
                          .diskCapacityBytes = 1000 * GiB,
                          .diskFreeBytes = 500 * GiB,
                          .busySlots = 6 };
}

/// A reading of @p host doing @p load.
[[nodiscard]] StatsReading ReadingOf(std::optional<HostCapacity> host, std::optional<HostLoadReading> load)
{
    return StatsReading { .counters = {},
                          .snapshot = MetricsSnapshot {
                              .storage = std::nullopt, .host = host, .hostLoad = load, .uptime = Uptime {} } };
}

/// The CPU counters of a machine that has been busy @p busy ticks of @p total.
[[nodiscard]] HostLoadReading Ticks(std::uint64_t busy, std::uint64_t total)
{
    return HostLoadReading { .cpu = CpuTicks { .busy = busy, .total = total }, .availableMemoryBytes = 32 * GiB };
}

} // namespace

TEST_CASE("A reading with no host has no slots", "[cli][node-slots]")
{
    // The daemon: no host block, so no slots line at all rather than a line of zeroes.
    CHECK_FALSE(NodeSlotsOf(nullptr, ReadingOf(std::nullopt, std::nullopt)).has_value());
    // The positive control: the same reading with a host has slots.
    CHECK(NodeSlotsOf(nullptr, ReadingOf(SixteenCores(), std::nullopt)).has_value());
}

TEST_CASE("Somebody else using the machine is named as the limit, as the scheduler names it", "[cli][node-slots]")
{
    // The mockup's own line: 6 in flight / 12 available / 16 registered, limited-by external-cpu.
    // 625 permille of 16 cores is 10 busy, 6 of them this fleet's, so 4 belong to somebody else.
    auto const previous = ReadingOf(SixteenCores(), Ticks(1000, 10000));
    auto const now = ReadingOf(SixteenCores(), Ticks(1000 + 625, 10000 + 1000));

    auto const slots = NodeSlotsOf(&previous, now);
    REQUIRE(slots.has_value());
    CHECK(Unwrap(slots).inFlight == 6);
    CHECK(Unwrap(slots).registered == 16);
    CHECK(Unwrap(slots).cpuBusyPermille == std::optional<std::uint32_t> { 625 });
    REQUIRE(Unwrap(slots).ceilings.has_value());
    auto const ceilings = Unwrap(Unwrap(slots).ceilings);
    CHECK(ceilings.available == 12);
    CHECK(ceilings.binding == Distributed::SlotLimit::ExternalCpu);

    // And it IS the scheduler's answer for the same inputs, not a lookalike.
    auto const scheduler = Distributed::SlotCeilingsFor(Distributed::NodeCapacity { .logicalCores = 16 },
                                                        16,
                                                        Distributed::NodeLoad { .inFlight = 6,
                                                                                .cpuBusyPermille = 625,
                                                                                .availableMemoryBytes = 32 * GiB,
                                                                                .freeScratchBytes = 500 * GiB,
                                                                                .cache = {} });
    CHECK(ceilings.available == scheduler.available);
    CHECK(ceilings.binding == scheduler.binding);
}

TEST_CASE("A machine short of memory is limited by memory, and scratch by scratch", "[cli][node-slots]")
{
    // An idle CPU, so the only thing that can bind is the resource each section takes away.
    auto const idle = [](std::uint64_t memory, std::uint64_t scratch) {
        auto host = SixteenCores();
        host.diskFreeBytes = scratch;
        auto load = Ticks(0, 0);
        load.availableMemoryBytes = memory;
        return std::pair { host, load };
    };

    SECTION("memory")
    {
        auto [host, load] = idle(2 * GiB, 500 * GiB);
        auto const previous = ReadingOf(host, load);
        load.cpu = CpuTicks { .busy = 0, .total = 1000 };
        auto const slots = NodeSlotsOf(&previous, ReadingOf(host, load));
        REQUIRE(slots.has_value());
        REQUIRE(Unwrap(slots).ceilings.has_value());
        // Two more jobs' worth of memory on top of the six running.
        CHECK(Unwrap(Unwrap(slots).ceilings).available == 8);
        CHECK(Unwrap(Unwrap(slots).ceilings).binding == Distributed::SlotLimit::Memory);
        CHECK(Unwrap(slots).availableMemoryBytes == std::optional { 2 * GiB });
    }

    SECTION("scratch")
    {
        auto [host, load] = idle(64 * GiB, Distributed::ScratchBudgetPerJobBytes * 3);
        auto const previous = ReadingOf(host, load);
        load.cpu = CpuTicks { .busy = 0, .total = 1000 };
        auto const slots = NodeSlotsOf(&previous, ReadingOf(host, load));
        REQUIRE(slots.has_value());
        REQUIRE(Unwrap(slots).ceilings.has_value());
        CHECK(Unwrap(Unwrap(slots).ceilings).available == 9);
        CHECK(Unwrap(Unwrap(slots).ceilings).binding == Distributed::SlotLimit::Scratch);
    }
}

TEST_CASE("A node's first reading names no limit, because its CPU cannot be read from one", "[cli][node-slots]")
{
    // The platform reports CPU, so the scheduler has a CPU ceiling and the panel, with one reading,
    // does not. Saying `registered` here would tell an operator nothing holds the machine back.
    auto const now = ReadingOf(SixteenCores(), Ticks(1625, 11000));
    auto const slots = NodeSlotsOf(nullptr, now);
    REQUIRE(slots.has_value());
    CHECK(Unwrap(slots).inFlight == 6);
    CHECK_FALSE(Unwrap(slots).ceilings.has_value());
    CHECK_FALSE(Unwrap(slots).cpuBusyPermille.has_value());
    // What one reading CAN say, it says.
    CHECK(Unwrap(slots).availableMemoryBytes == std::optional { 32 * GiB });

    // The same after a gap: the previous reading carried no load, and a rate is never taken across one.
    auto const gap = ReadingOf(SixteenCores(), std::nullopt);
    CHECK_FALSE(Unwrap(NodeSlotsOf(&gap, now)).ceilings.has_value());

    // And a pair the CPU arithmetic declines -- counters gone backwards, as after a suspended VM.
    auto const later = ReadingOf(SixteenCores(), Ticks(5000, 50000));
    CHECK_FALSE(Unwrap(NodeSlotsOf(&later, now)).ceilings.has_value());

    // The control: an ordinary adjacent pair does name one.
    auto const earlier = ReadingOf(SixteenCores(), Ticks(1000, 10000));
    CHECK(Unwrap(NodeSlotsOf(&earlier, now)).ceilings.has_value());
}

TEST_CASE("A platform that will not report its CPU still gets the scheduler's answer", "[cli][node-slots]")
{
    // Absent CPU is what the scheduler schedules around, from the same platform: the ceilings are
    // computed on the other figures, and nothing waits for a second reading that cannot help.
    auto const load = HostLoadReading { .cpu = std::nullopt, .availableMemoryBytes = 32 * GiB };
    auto const slots = NodeSlotsOf(nullptr, ReadingOf(SixteenCores(), load));
    REQUIRE(slots.has_value());
    REQUIRE(Unwrap(slots).ceilings.has_value());
    CHECK(Unwrap(Unwrap(slots).ceilings).available == 16);
    CHECK(Unwrap(Unwrap(slots).ceilings).binding == Distributed::SlotLimit::Registered);
    CHECK_FALSE(Unwrap(Unwrap(slots).ceilings).byExternalCpu.has_value());
}

TEST_CASE("A reading that carries no load names no limit", "[cli][node-slots]")
{
    // Not the platform declining: the reading has no load block at all, so the ceilings are made of
    // nothing the panel holds, while the scheduler holds a heartbeat's worth.
    auto const previous = ReadingOf(SixteenCores(), std::nullopt);
    auto const slots = NodeSlotsOf(&previous, ReadingOf(SixteenCores(), std::nullopt));
    REQUIRE(slots.has_value());
    CHECK(Unwrap(slots).registered == 16);
    CHECK_FALSE(Unwrap(slots).ceilings.has_value());
    CHECK_FALSE(Unwrap(slots).availableMemoryBytes.has_value());
}

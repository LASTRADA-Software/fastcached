// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/NodeLoadTestUtils.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace FastCache::Distributed;
using namespace FastCache::Distributed::Testing;

TEST_CASE("A machine nobody classified is treated as somebody's desk", "[distributed][nodepolicy]")
{
    // The safety property, and the reason `Workstation` is the zero value. Getting
    // this backwards saturates a developer's machine with other people's builds --
    // a failure they experience as "my editor stutters" and never connect to a
    // build fleet, because nothing anywhere reports it.
    constexpr NodeCapacity unclassified { .logicalCores = 8 };

    CHECK(unclassified.nodeClass == NodeClass::Workstation);
    CHECK(OfferableSlots(unclassified, 0) == 6); // 8 cores, 2 held back
}

TEST_CASE("A dedicated machine is driven to its limit", "[distributed][nodepolicy]")
{
    // Nobody is sitting at it, so holding cores back would be pure waste -- the
    // whole reason the class exists rather than one reserve for everybody.
    constexpr NodeCapacity server { .logicalCores = 64, .nodeClass = NodeClass::Dedicated };

    CHECK(OfferableSlots(server, 0) == 64);
}

TEST_CASE("A small workstation still offers one slot", "[distributed][nodepolicy]")
{
    // Saturating rather than wrapping. A two-core workstation reserving two cores
    // must offer one, not 4294967295 -- and a node that offered ZERO would register,
    // heartbeat, never be picked, and look exactly like a fleet that is permanently
    // busy. That is the failure the zero-slot registration refusal exists to
    // prevent, arriving by arithmetic instead of by configuration.
    constexpr NodeCapacity dualCore { .logicalCores = 2 };
    CHECK(OfferableSlots(dualCore, 0) == 1);

    constexpr NodeCapacity singleCore { .logicalCores = 1 };
    CHECK(OfferableSlots(singleCore, 0) == 1);
}

TEST_CASE("A machine that reported no cores is treated as having one", "[distributed][nodepolicy]")
{
    // Refusing to schedule onto it would punish a worker for a fact it merely failed
    // to collect. One slot is the answer that is never wrong by much.
    constexpr NodeCapacity silent { .logicalCores = 0, .nodeClass = NodeClass::Dedicated };
    CHECK(OfferableSlots(silent, 0) == 1);
}

TEST_CASE("An explicit slot count is the answer, not a hint", "[distributed][nodepolicy]")
{
    // It is neither capped at the core count nor reduced by the class reserve, and
    // both halves are decisions. `--slots` is set by the person whose machine this
    // is: capping it would silently refuse the deliberate oversubscription an
    // I/O-heavy build wants, and subtracting the reserve on top would make
    // `--slots 4` on a workstation quietly offer two, which is not what the flag
    // says. The "fuller than the fleet believes" hazard a cap would guard against
    // cannot arise, because the worker enforces this same number locally -- one
    // `OfferableSlots` call answers both.
    constexpr NodeCapacity workstation { .logicalCores = 16 };
    constexpr NodeCapacity server { .logicalCores = 16, .nodeClass = NodeClass::Dedicated };

    CHECK(OfferableSlots(server, 4) == 4);
    CHECK(OfferableSlots(workstation, 4) == 4);
    CHECK(OfferableSlots(server, 64) == 64);

    // Including past what the memory would otherwise allow: an operator who names a
    // number has overridden the heuristic, not asked it to arbitrate.
    constexpr NodeCapacity cramped { .logicalCores = 64, .totalMemoryBytes = 4ULL << 30 };
    CHECK(OfferableSlots(cramped, 32) == 32);
}

TEST_CASE("A machine with more cores than memory is sized by its memory", "[distributed][nodepolicy]")
{
    // One job per core is the obvious rule and is wrong where the ratio is skewed: a
    // 64-thread box with 8 GiB asked for 64 concurrent C++ compiles swaps, or the
    // OOM killer takes them -- and those come back as refusals the client retries
    // locally, so distribution appears to work while making the build slower than
    // not distributing at all.
    constexpr NodeCapacity cramped { .logicalCores = 64, .totalMemoryBytes = 8ULL << 30, .nodeClass = NodeClass::Dedicated };
    CHECK(OfferableSlots(cramped, 0) == 8);

    // The reserve comes off AFTER the clamp, which is what the operator's promise
    // means: two threads stay out of the fleet's hands whichever ceiling bound
    // first. Subtracting before would let the memory clamp satisfy the promise on
    // paper while the machine still ran a compile on every core.
    constexpr NodeCapacity crampedDesk { .logicalCores = 64, .totalMemoryBytes = 8ULL << 30 };
    CHECK(OfferableSlots(crampedDesk, 0) == 6);
}

TEST_CASE("Memory only ever lowers the count, and silence lowers nothing", "[distributed][nodepolicy]")
{
    // An ordinary build host has at least a gigabyte per thread, so the heuristic
    // must not bind there -- a clamp that fires on healthy machines is one nobody
    // can trust when it fires on a sick one.
    constexpr NodeCapacity ample { .logicalCores = 16, .totalMemoryBytes = 64ULL << 30, .nodeClass = NodeClass::Dedicated };
    CHECK(OfferableSlots(ample, 0) == 16);

    // Absent is not zero. A machine that could not read its own memory is scheduled
    // on its other properties; reading "0 bytes" literally would clamp every such
    // node to one slot for a fact it merely failed to collect.
    CHECK_FALSE(MemorySlotCeiling(0).has_value());
    constexpr NodeCapacity silent { .logicalCores = 16, .totalMemoryBytes = 0, .nodeClass = NodeClass::Dedicated };
    CHECK(OfferableSlots(silent, 0) == 16);

    // And a machine with less than one budget's worth still offers one rather than
    // none, for the reason every other floor here exists.
    CHECK(MemorySlotCeiling(256ULL << 20) == 1U);
}

TEST_CASE("A node class byte this build does not know is refused", "[distributed][nodepolicy]")
{
    // Not clamped, because the two wrong answers are wrong in opposite directions
    // and neither is visible from either end: read as `Workstation` it offers a
    // dedicated build server two cores fewer than it has, read as `Dedicated` it
    // saturates somebody's desktop.
    CHECK(NodeClassFromRaw(0) == NodeClass::Workstation);
    CHECK(NodeClassFromRaw(1) == NodeClass::Dedicated);
    CHECK_FALSE(NodeClassFromRaw(2).has_value());
    CHECK_FALSE(NodeClassFromRaw(255).has_value());
}

TEST_CASE("An explicit reserve overrides the class default", "[distributed][nodepolicy]")
{
    // The default is what most people run, so it has to be right -- but somebody who
    // knows their machine must be able to say so. `reserveIsExplicit` is what tells
    // "leave 0 free" apart from "did not say", which a bare 0 cannot.
    constexpr NodeCapacity generous { .logicalCores = 16, .reservedCores = 8, .reserveIsExplicit = true };
    CHECK(OfferableSlots(generous, 0) == 8);

    constexpr NodeCapacity none { .logicalCores = 16, .reservedCores = 0, .reserveIsExplicit = true };
    CHECK(OfferableSlots(none, 0) == 16);

    // And a workstation that did NOT say still gets the class default rather than
    // the zero its field happens to hold.
    constexpr NodeCapacity silent { .logicalCores = 16 };
    CHECK(OfferableSlots(silent, 0) == 14);
}

TEST_CASE("A class is named, and an unknown name is refused", "[distributed][nodepolicy]")
{
    CHECK(NodeClassByName("workstation") == NodeClass::Workstation);
    CHECK(NodeClassByName("dedicated") == NodeClass::Dedicated);

    // Refused rather than defaulted: an operator who typed `--node-class=server`
    // meant something, and silently giving them a workstation would hold cores back
    // on a machine they said was dedicated -- with a config file that looks correct.
    CHECK_FALSE(NodeClassByName("server").has_value());
    CHECK_FALSE(NodeClassByName("").has_value());
}

TEST_CASE("A machine somebody else is using withdraws that capacity", "[distributed][nodepolicy]")
{
    // The point of sampling host CPU at all. A workstation's static reserve keeps
    // two cores free permanently; this is what happens when its owner starts using
    // six more of them.
    constexpr NodeCapacity desk { .logicalCores = 16 };
    constexpr auto registered = OfferableSlots(desk, 0); // 14
    static_assert(registered == 14);

    // Half the machine busy, nothing of it ours: eight of the sixteen cores are
    // somebody else's, so eight come off the fourteen it registered with.
    constexpr auto busy = WithCpu(0, 500);
    CHECK(AvailableSlots(desk, registered, busy) == 6);

    // An idle machine keeps everything it registered with. Zero is a measurement,
    // not an absence, and must not be treated as one.
    constexpr auto idle = WithCpu(0, 0);
    CHECK(AvailableSlots(desk, registered, idle) == registered);

    // And a machine that would not say keeps everything too: absent is not zero in
    // the other direction either, and punishing a node for a fact it merely failed
    // to collect would take whole platforms out of a fleet.
    constexpr auto silent = Busy(0);
    CHECK(AvailableSlots(desk, registered, silent) == registered);
}

TEST_CASE("A machine does not withdraw because of the fleet's own jobs", "[distributed][nodepolicy]")
{
    // The feedback loop this subtraction exists to break. Without it: giving a
    // machine work raises its CPU, which withdraws the capacity that let it take the
    // work, which frees the CPU -- a fleet that oscillates for reasons no operator
    // can see from either end.
    constexpr NodeCapacity server { .logicalCores = 16, .nodeClass = NodeClass::Dedicated };

    // Eight of sixteen cores busy and eight of our jobs running: all of it is ours,
    // so nothing is withdrawn and the remaining slots stay available.
    constexpr auto ours = WithCpu(8, 500);
    CHECK(AvailableSlots(server, 16, ours) == 16);

    // Twelve busy with eight of ours: four cores belong to somebody else.
    constexpr auto shared = WithCpu(8, 750);
    CHECK(AvailableSlots(server, 16, shared) == 12);
}

TEST_CASE("A saturated machine withdraws entirely, and may reach zero", "[distributed][nodepolicy]")
{
    // Unlike `OfferableSlots`, which has a floor. The two answer different
    // questions: a registered worker must be worth picking EVENTUALLY, which is why
    // that one never returns zero; a worker whose machine is fully committed right
    // now must be worth picking NEVER, until that changes -- and a floor here would
    // keep handing it jobs it has nowhere to run.
    constexpr NodeCapacity desk { .logicalCores = 8 };
    constexpr auto hammered = WithCpu(0, 1000);

    CHECK(AvailableSlots(desk, OfferableSlots(desk, 0), hammered) == 0);
}

TEST_CASE("A worker whose scratch disk has filled stops being offered work", "[distributed][nodepolicy]")
{
    // One of the most common failures on a long-lived build host and one of the
    // least visible from the other end: such a worker accepts every job and refuses
    // every one, so the client compiles locally while the fleet looks healthy.
    constexpr NodeCapacity server { .logicalCores = 16, .nodeClass = NodeClass::Dedicated };

    constexpr auto full = WithScratch(0, 0);
    CHECK(AvailableSlots(server, 16, full) == 0);

    // Room for a few, not for sixteen.
    constexpr auto tight = WithScratch(0, 512ULL << 20);
    CHECK(AvailableSlots(server, 16, tight) == 4);

    // Plenty of room changes nothing.
    constexpr auto roomy = WithScratch(0, 500ULL << 30);
    CHECK(AvailableSlots(server, 16, roomy) == 16);
}

TEST_CASE("A resource ceiling counts the running jobs' own share once", "[distributed][nodepolicy]")
{
    // A machine reports what is left AFTER its current jobs have taken theirs, so a
    // ceiling of `available / budget` alone would count their share twice and shrink
    // a busy machine to nothing the moment it started working -- which for a
    // scheduler reads as a fleet that collapses under exactly the load it exists to
    // carry.
    constexpr NodeCapacity server { .logicalCores = 16, .nodeClass = NodeClass::Dedicated };

    // Four jobs running and room for four more: the ceiling is eight, not four.
    constexpr auto working = WithScratch(4, 512ULL << 20);
    CHECK(AvailableSlots(server, 16, working) == 8);

    // Memory behaves identically, which is the reason the arithmetic is one helper.
    constexpr auto memoryBound = WithMemory(4, 4ULL << 30);
    CHECK(AvailableSlots(server, 16, memoryBound) == 8);
}

TEST_CASE("The lowest of the live ceilings wins", "[distributed][nodepolicy]")
{
    // Each limit describes a different thing running out, so they compose by
    // minimum rather than by any kind of blend: a machine with plenty of CPU and no
    // disk cannot compile, and reporting the average of the two would say it half
    // can.
    constexpr NodeCapacity server { .logicalCores = 32, .nodeClass = NodeClass::Dedicated };
    // Spelled out rather than built, because this is the one case that is about
    // all three limits at once.
    constexpr NodeLoad mixed { .inFlight = 0,
                               .cpuBusyPermille = 250,              // 8 cores elsewhere -> 24
                               .availableMemoryBytes = 20ULL << 30, // -> 20
                               .freeScratchBytes = 1ULL << 30 };    // -> 8

    CHECK(AvailableSlots(server, 32, mixed) == 8);
}

TEST_CASE("Naming the limit that withdrew a machine's slots", "[distributed][nodepolicy][slotlimit]")
{
    // The number `AvailableSlots` returns is a `min` of four, and which one it came
    // from is the only thing an operator can act on: a node offering 2 of 16 sends
    // them shopping for hardware when somebody is merely *using* the machine.
    //
    // Distinct numbers per case, deliberately: a table of near-identical rows makes
    // "this ceiling reports its neighbour's value" the likely slip, and equal
    // values would let it through.
    constexpr NodeCapacity machine { .logicalCores = 32,
                                     .totalMemoryBytes = 64ULL << 30,
                                     .nodeClass = NodeClass::Dedicated };

    SECTION("nothing withdraws anything")
    {
        constexpr auto ceilings = SlotCeilingsFor(machine, 16, Busy(3));
        STATIC_REQUIRE(ceilings.binding == SlotLimit::Registered);
        STATIC_REQUIRE(ceilings.available == 16);
        STATIC_REQUIRE(ceilings.registered == 16);
        // Absent is not zero: a machine that reported nothing has no ceilings,
        // which is a different claim from ceilings of zero.
        STATIC_REQUIRE(!ceilings.byExternalCpu.has_value());
        STATIC_REQUIRE(!ceilings.byMemory.has_value());
        STATIC_REQUIRE(!ceilings.byScratch.has_value());
    }

    SECTION("somebody else is using the machine")
    {
        // 500 permille of 32 cores is 16 busy, less the 2 this fleet runs = 14
        // external, so 16 registered - 14 = 2.
        constexpr auto ceilings = SlotCeilingsFor(machine, 16, WithCpu(2, 500));
        STATIC_REQUIRE(ceilings.binding == SlotLimit::ExternalCpu);
        STATIC_REQUIRE(ceilings.available == 2);
        STATIC_REQUIRE(ceilings.byExternalCpu == 2U);
        STATIC_REQUIRE(!ceilings.byMemory.has_value());
    }

    SECTION("memory is nearly gone")
    {
        // 3 GiB left plus the 1 job running = 4.
        constexpr auto ceilings = SlotCeilingsFor(machine, 16, WithMemory(1, 3ULL << 30));
        STATIC_REQUIRE(ceilings.binding == SlotLimit::Memory);
        STATIC_REQUIRE(ceilings.available == 4);
        STATIC_REQUIRE(ceilings.byMemory == 4U);
        STATIC_REQUIRE(!ceilings.byExternalCpu.has_value());
    }

    SECTION("the scratch filesystem is nearly full")
    {
        // 640 MiB at 128 MiB a job is 5, plus the 1 running = 6.
        constexpr auto ceilings = SlotCeilingsFor(machine, 16, WithScratch(1, 640ULL << 20));
        STATIC_REQUIRE(ceilings.binding == SlotLimit::Scratch);
        STATIC_REQUIRE(ceilings.available == 6);
        STATIC_REQUIRE(ceilings.byScratch == 6U);
    }

    SECTION("the lowest of several ceilings binds, and each is still reported")
    {
        constexpr NodeLoad crowded { .inFlight = 1,
                                     .cpuBusyPermille = 250,          // 8 busy - 1 ours = 7 external -> 9
                                     .availableMemoryBytes = 7ULL << 30,  // 7 + 1 = 8
                                     .freeScratchBytes = 384ULL << 20 };  // 3 + 1 = 4
        constexpr auto ceilings = SlotCeilingsFor(machine, 16, crowded);
        STATIC_REQUIRE(ceilings.byExternalCpu == 9U);
        STATIC_REQUIRE(ceilings.byMemory == 8U);
        STATIC_REQUIRE(ceilings.byScratch == 4U);
        STATIC_REQUIRE(ceilings.available == 4);
        STATIC_REQUIRE(ceilings.binding == SlotLimit::Scratch);
    }

    SECTION("a tie names the earlier limit, so the answer is reproducible")
    {
        // Memory and scratch both land on 4. Which one is named must not depend on
        // how the comparison happened to be written.
        constexpr NodeLoad tied { .inFlight = 1,
                                  .cpuBusyPermille = std::nullopt,
                                  .availableMemoryBytes = 3ULL << 30,
                                  .freeScratchBytes = 384ULL << 20 };
        constexpr auto ceilings = SlotCeilingsFor(machine, 16, tied);
        STATIC_REQUIRE(ceilings.byMemory == 4U);
        STATIC_REQUIRE(ceilings.byScratch == 4U);
        STATIC_REQUIRE(ceilings.binding == SlotLimit::Memory);
    }
}

TEST_CASE("The unfolded ceilings agree with the number the scheduler uses", "[distributed][nodepolicy][slotlimit]")
{
    // The property that says the refactor changed nothing: `AvailableSlots` IS the
    // minimum `SlotCeilingsFor` arrives at, over every shape of report.
    constexpr NodeCapacity machine { .logicalCores = 16,
                                     .totalMemoryBytes = 32ULL << 30,
                                     .nodeClass = NodeClass::Dedicated };

    constexpr auto agrees = [](NodeCapacity const& capacity, std::uint32_t slots, NodeLoad const& load) {
        return AvailableSlots(capacity, slots, load) == SlotCeilingsFor(capacity, slots, load).available;
    };

    STATIC_REQUIRE(agrees(machine, 8, Busy(0)));
    STATIC_REQUIRE(agrees(machine, 8, Busy(8)));
    STATIC_REQUIRE(agrees(machine, 8, WithCpu(0, 1000)));
    STATIC_REQUIRE(agrees(machine, 8, WithCpu(4, 250)));
    STATIC_REQUIRE(agrees(machine, 8, WithMemory(0, 0)));
    STATIC_REQUIRE(agrees(machine, 8, WithScratch(2, 1ULL << 30)));
    STATIC_REQUIRE(agrees(NodeCapacity {}, 1, WithCpu(0, 1000)));
}

TEST_CASE("Every slot limit has a row naming it and what to do about it", "[distributed][nodepolicy][slotlimit]")
{
    // The table drives the report, so a limit added to the enum without a row is a
    // build failure rather than a dashboard cell naming whichever row sorted first.
    // Asserted over the table rather than against a list written out beside it.
    for (auto const& row: SlotLimitTable)
    {
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.remedy.empty());
        CHECK(TraitsFor(row.limit).name == row.name);
    }
    CHECK(TraitsFor(SlotLimit::Scratch).name == "scratch");
}

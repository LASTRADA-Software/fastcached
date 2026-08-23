// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/NodePolicy.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace FastCache::Distributed;

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

TEST_CASE("An advertised slot count caps but never raises", "[distributed][nodepolicy]")
{
    // A worker may ask for fewer than its cores -- it knows something the scheduler
    // does not, like a memory-hungry toolchain. It may not ask for more: the machine
    // would be fuller and slower than the fleet believes, at the same moment, which
    // is precisely what the slot cap exists to prevent.
    constexpr NodeCapacity server { .logicalCores = 16, .nodeClass = NodeClass::Dedicated };

    CHECK(OfferableSlots(server, 4) == 4);
    CHECK(OfferableSlots(server, 64) == 16);
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

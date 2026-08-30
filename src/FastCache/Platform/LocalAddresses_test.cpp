// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{
/// The interval every case below measures against, named once.
constexpr auto Interval = 10s;
} // namespace

TEST_CASE("Loopback is this machine without the platform being asked at all", "[platform][locality]")
{
    // Fast by construction before it is fast by cache. The node's cache surface
    // binds loopback by default, so this is the answer essentially every real caller
    // gets -- and it must not reach the address set, because a cache only the
    // unusual case touches is a far weaker thing to have to get right.
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    // One call, at construction. Everything below is free.
    REQUIRE(machine.Calls() == 1);

    CHECK(locality.IsThisMachine("127.0.0.1"));
    CHECK(locality.IsThisMachine("127.0.0.53"));
    CHECK(locality.IsThisMachine("::1"));
    CHECK(machine.Calls() == 1);
}

TEST_CASE("The address set is refreshed on an interval, never because a caller missed", "[platform][locality]")
{
    // The rule with teeth. A refresh triggered by an unrecognised address hands a
    // remote peer a free amplifier: it forces the expensive probe once per request
    // simply by asking, and on Windows that probe is milliseconds. Nothing about the
    // ANSWERS distinguishes the two designs -- both refuse every stranger correctly
    // -- so the probe count is the only thing that can tell them apart, which is why
    // `ScriptedHostAddresses` counts.
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };
    REQUIRE(machine.Calls() == 1);

    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 50))
        CHECK_FALSE(locality.IsThisMachine("10.9.9.9"));
    CHECK(machine.Calls() == 1);

    // The interval, and not one call before it.
    clock.Advance(Interval - 1ms);
    CHECK_FALSE(locality.IsThisMachine("10.9.9.9"));
    CHECK(machine.Calls() == 1);

    clock.Advance(1ms);
    CHECK_FALSE(locality.IsThisMachine("10.9.9.9"));
    CHECK(machine.Calls() == 2);
}

TEST_CASE("An address this machine gains is refused until the next refresh, and then admitted", "[platform][locality]")
{
    // The first of the two failure directions the header names, and the safe one:
    // staleness fails CLOSED and self-heals with no operator action. What it costs
    // is one local client on a freshly-assigned address falling back to a local
    // compile -- a miss and a retry, never a wrong answer served confidently.
    Testing::ScriptedHostAddresses machine { { "10.0.0.7" } };
    ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    machine.Publish({ "10.0.0.7", "10.0.0.8" });
    CHECK_FALSE(locality.IsThisMachine("10.0.0.8"));

    clock.Advance(Interval);
    CHECK(locality.IsThisMachine("10.0.0.8"));
}

TEST_CASE("An address this machine loses stays admitted for at most one interval", "[platform][locality]")
{
    // The other direction, stated rather than hidden, because a rule whose only
    // documented failure mode is the harmless one is a rule nobody has priced.
    // Exploiting this needs another machine to be handed that exact address inside
    // the window -- a DHCP reassignment racing the refresh -- and the machine that
    // gains it is on the segment the address came from.
    Testing::ScriptedHostAddresses machine { { "10.0.0.7", "10.0.0.8" } };
    ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    machine.Publish({ "10.0.0.7" });
    CHECK(locality.IsThisMachine("10.0.0.8"));

    clock.Advance(Interval);
    CHECK_FALSE(locality.IsThisMachine("10.0.0.8"));
}

TEST_CASE("A dual-stack caller is folded against the interface list, both spellings", "[platform][locality]")
{
    // A surface bound to `::` reports an IPv4 peer as `::ffff:10.0.0.7`, and which
    // spelling arrives is a property of how the LISTENER was bound rather than of
    // anything about the peer. A raw compare would therefore refuse this machine's
    // own clients on exactly the bind the locality rule exists for -- the failure
    // #180 already paid for once on the member list.
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    CHECK(locality.IsThisMachine("10.0.0.7"));
    CHECK(locality.IsThisMachine("::ffff:10.0.0.7"));

    // And the other way round: an interface reported in the mapped spelling still
    // answers for a peer that arrives unmapped.
    Testing::ScriptedHostAddresses const mapped { { "::ffff:10.0.0.7" } };
    CachedLocalityOracle const foldsBack { mapped, clock, Interval };
    CHECK(foldsBack.IsThisMachine("10.0.0.7"));
}

TEST_CASE("A machine that would not say its addresses is loopback and nothing else", "[platform][locality]")
{
    // The direction an unanswerable probe has to fail in. A platform that returns
    // nothing must not become a wildcard, and a node whose own address it cannot
    // learn still has to serve the loopback clients that are the reason it runs.
    Testing::ScriptedHostAddresses const silent;
    ManualClock clock;
    CachedLocalityOracle const locality { silent, clock, Interval };

    CHECK(locality.IsThisMachine("127.0.0.1"));
    CHECK_FALSE(locality.IsThisMachine("10.0.0.7"));
}

TEST_CASE("A peer with no name matches nothing, an empty interface entry included", "[platform][locality]")
{
    // `FormatPeerAddress` answers empty for a peer whose family is unknown or whose
    // `getpeername` failed, and two unanswerable questions are not a match. The
    // guard is `SameHost`'s rather than this file's, and it is asserted here because
    // this is the surface where being wrong hands a stranger the tier.
    Testing::ScriptedHostAddresses const machine { { "", "10.0.0.7" } };
    ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    CHECK_FALSE(locality.IsThisMachine(""));
    CHECK_FALSE(locality.IsThisMachine("::ffff:"));
}

TEST_CASE("The real machine reports addresses, and loopback is among them", "[platform][locality]")
{
    // The one case that touches the kernel, and it is here because everything above
    // proves the *rules* against a scripted machine and none of it proves the probe
    // runs at all. A `QueryLocalAddresses` that always answered empty would leave
    // every case above green and every node refusing its own operator on a widened
    // bind.
    //
    // Loopback specifically, rather than a count: it is the one address every host
    // running this suite has, since the suite binds and dials `127.0.0.1` throughout.
    auto const addresses = QueryLocalAddresses();
    REQUIRE_FALSE(addresses.empty());

    auto const loopback = [](std::string const& address) {
        return IsLoopbackHost(address);
    };
    CHECK(std::ranges::any_of(addresses, loopback));

    // And nothing it reports is a spelling a peer could never arrive in: no port, no
    // brackets, no `%scope` suffix, because `FormatPeerAddress` produces none of
    // those and a set the peers cannot match is a set that looks populated and is
    // not.
    auto const unmatchable = [](std::string const& address) {
        return address.empty() || address.contains('%') || address.contains('[');
    };
    CHECK(std::ranges::none_of(addresses, unmatchable));
}

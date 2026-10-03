// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/Unwrap.hpp>

#if !defined(_WIN32)
    #include <net/if.h>
#endif

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
    core::platform::ManualClock clock;
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
    core::platform::ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };
    REQUIRE(machine.Calls() == 1);

    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 50))
        CHECK_FALSE(locality.IsThisMachine("10.9.9.9"));
    CHECK(machine.Calls() == 1);

    // The interval, and not one call before it.
    clock.advance(Interval - 1ms);
    CHECK_FALSE(locality.IsThisMachine("10.9.9.9"));
    CHECK(machine.Calls() == 1);

    clock.advance(1ms);
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
    core::platform::ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    machine.Publish({ "10.0.0.7", "10.0.0.8" });
    CHECK_FALSE(locality.IsThisMachine("10.0.0.8"));

    clock.advance(Interval);
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
    core::platform::ManualClock clock;
    CachedLocalityOracle const locality { machine, clock, Interval };

    machine.Publish({ "10.0.0.7" });
    CHECK(locality.IsThisMachine("10.0.0.8"));

    clock.advance(Interval);
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
    core::platform::ManualClock clock;
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
    core::platform::ManualClock clock;
    CachedLocalityOracle const locality { silent, clock, Interval };

    CHECK(locality.IsThisMachine("127.0.0.1"));
    CHECK_FALSE(locality.IsThisMachine("10.0.0.7"));
}

TEST_CASE("A peer with no name matches nothing, an empty interface entry included", "[platform][locality]")
{
    // `core::net::formatPeerAddress` answers empty for a peer whose family is unknown or whose
    // `getpeername` failed, and two unanswerable questions are not a match. The
    // guard is `SameHost`'s rather than this file's, and it is asserted here because
    // this is the surface where being wrong hands a stranger the tier.
    Testing::ScriptedHostAddresses const machine { { "", "10.0.0.7" } };
    core::platform::ManualClock clock;
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
    // brackets, no `%scope` suffix, because `core::net::formatPeerAddress` produces none of
    // those and a set the peers cannot match is a set that looks populated and is
    // not.
    auto const unmatchable = [](std::string const& address) {
        return address.empty() || address.contains('%') || address.contains('[');
    };
    CHECK(std::ranges::none_of(addresses, unmatchable));
}

namespace
{
/// An interface address, up and not loopback unless a case says otherwise.
/// @param octets The address.
/// @param prefix Its on-link prefix.
/// @param up Whether its link is up.
/// @param loopback Whether it is a loopback interface.
/// @return The address.
[[nodiscard]] Ipv4InterfaceAddress At(std::array<std::uint8_t, 4> octets,
                                      std::uint8_t prefix,
                                      bool up = true,
                                      bool loopback = false,
                                      bool broadcastLink = true)
{
    return Ipv4InterfaceAddress { .interfaceName = "eth",
                                  .address = octets,
                                  .prefixLength = prefix,
                                  .up = up,
                                  .loopback = loopback,
                                  .broadcastLink = broadcastLink };
}
} // namespace

TEST_CASE("An interface address is beaconed from only when it is up, routable and on a subnet with a broadcast",
          "[platform][interfaces]")
{
    // One row per exclusion, and each row's address differs from the eligible one by the ONE
    // property it excludes on. The last row pins the order: a down loopback is `Down`, the answer an
    // operator can act on first.
    struct Row
    {
        std::string_view what;
        Ipv4InterfaceAddress address;
        BroadcastEligibility expected;
    };
    auto const rows = std::array {
        Row { .what = "a LAN address", .address = At({ 192, 168, 86, 24 }, 24), .expected = BroadcastEligibility::Eligible },
        Row { .what = "a /30", .address = At({ 10, 0, 0, 5 }, 30), .expected = BroadcastEligibility::Eligible },
        Row { .what = "down", .address = At({ 192, 168, 86, 24 }, 24, false), .expected = BroadcastEligibility::Down },
        Row { .what = "a loopback interface",
              .address = At({ 127, 0, 0, 1 }, 8, true, true),
              .expected = BroadcastEligibility::Loopback },
        Row { .what = "127/8 on an interface not flagged loopback",
              .address = At({ 127, 0, 0, 2 }, 8),
              .expected = BroadcastEligibility::Loopback },
        Row { .what = "link-local", .address = At({ 169, 254, 3, 4 }, 16), .expected = BroadcastEligibility::LinkLocal },
        Row { .what = "a /31", .address = At({ 10, 0, 0, 4 }, 31), .expected = BroadcastEligibility::NoBroadcast },
        Row { .what = "a /32", .address = At({ 172, 31, 255, 2 }, 32), .expected = BroadcastEligibility::NoBroadcast },
        Row { .what = "a /0", .address = At({ 10, 0, 0, 1 }, 0), .expected = BroadcastEligibility::NoBroadcast },
        Row { .what = "0.0.0.0", .address = At({ 0, 0, 0, 0 }, 8), .expected = BroadcastEligibility::Unassigned },
        // A tunnel has a subnet on paper and no broadcast domain: a WireGuard `wg0` at 10.0.0.2/24
        // is IFF_POINTOPOINT, or at least not IFF_BROADCAST, and its prefix alone would call it
        // eligible. The link decides, whatever the prefix.
        Row { .what = "a point-to-point /24 (WireGuard wg0)",
              .address = At({ 10, 0, 0, 2 }, 24, true, false, false),
              .expected = BroadcastEligibility::NoBroadcast },
        Row { .what = "a down loopback",
              .address = At({ 127, 0, 0, 1 }, 8, false, true),
              .expected = BroadcastEligibility::Down },
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        CHECK(EligibilityOf(row.address) == row.expected);
    }

    // Every exclusion says why, since the sentence naming it is the only place an operator meets it.
    for (auto const& row: BroadcastEligibilityRows)
    {
        INFO(row.word);
        CHECK_FALSE(row.word.empty());
        CHECK(row.why.empty() == (row.answer == BroadcastEligibility::Eligible));
    }
}

TEST_CASE("A directed broadcast sets every host bit of the subnet and keeps the network bits", "[platform][interfaces]")
{
    struct Row
    {
        Ipv4InterfaceAddress address;
        std::string_view broadcast;
    };
    auto const rows = std::array {
        Row { .address = At({ 192, 168, 86, 24 }, 24), .broadcast = "192.168.86.255" },
        Row { .address = At({ 192, 168, 86, 24 }, 23), .broadcast = "192.168.87.255" },
        Row { .address = At({ 10, 20, 0, 5 }, 16), .broadcast = "10.20.255.255" },
        Row { .address = At({ 10, 0, 0, 5 }, 30), .broadcast = "10.0.0.7" },
        Row { .address = At({ 10, 1, 2, 3 }, 8), .broadcast = "10.255.255.255" },
        Row { .address = At({ 172, 31, 255, 2 }, 20), .broadcast = "172.31.255.255" },
    };
    for (auto const& row: rows)
    {
        INFO(FormatIpv4(row.address.address) << "/" << static_cast<int>(row.address.prefixLength));
        CHECK(FormatIpv4(DirectedBroadcastOf(row.address)) == row.broadcast);
    }
}

TEST_CASE("The real machine's interface walk reports its loopback, up and flagged", "[platform][interfaces]")
{
    // The positive control for the platform walk: every machine this suite runs on has a loopback
    // interface, so a walk that reported none -- or reported it unflagged, or down -- is a walk
    // that read the adapter list wrong, whatever it says about the rest.
    auto const source = MakeSystemInterfaceAddresses();
    auto const walked = source->Ipv4Addresses();
    REQUIRE(walked.has_value());
    auto const& interfaces = Testing::Unwrap(walked);
    REQUIRE_FALSE(interfaces.empty());
    CHECK(std::ranges::any_of(interfaces, [](Ipv4InterfaceAddress const& entry) {
        return entry.loopback && entry.up && entry.address[0] == 127 && entry.prefixLength == 8;
    }));
}

#if !defined(_WIN32)
TEST_CASE("A link is a broadcast medium when it broadcasts and is not point-to-point", "[platform][interfaces]")
{
    // The flags as the POSIX walk reads them. A tunnel -- WireGuard's wg0, a tun, a ppp -- is
    // IFF_POINTOPOINT, often with no IFF_BROADCAST at all, and its netmask alone would call it
    // eligible. IFF_LOOPBACK is not this function's question (`EligibilityOf` asks it first), so
    // it changes nothing either way; nor does link state.
    struct Row
    {
        std::string_view what;
        unsigned int flags;
        bool broadcastLink;
    };
    constexpr auto Up = static_cast<unsigned int>(IFF_UP | IFF_RUNNING);
    auto const rows = std::array {
        Row { .what = "broadcast only", .flags = IFF_BROADCAST, .broadcastLink = true },
        Row { .what = "point-to-point only", .flags = IFF_POINTOPOINT, .broadcastLink = false },
        Row { .what = "both", .flags = IFF_BROADCAST | IFF_POINTOPOINT, .broadcastLink = false },
        Row { .what = "neither", .flags = 0U, .broadcastLink = false },
        Row { .what = "an up Ethernet", .flags = Up | IFF_BROADCAST, .broadcastLink = true },
        Row { .what = "an up wg0", .flags = Up | IFF_POINTOPOINT, .broadcastLink = false },
        Row { .what = "loopback alone, as Linux's lo", .flags = Up | IFF_LOOPBACK, .broadcastLink = false },
        Row {
            .what = "loopback that says it broadcasts", .flags = Up | IFF_LOOPBACK | IFF_BROADCAST, .broadcastLink = true },
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        CHECK(BroadcastLinkFrom(row.flags) == row.broadcastLink);
    }
}
#endif

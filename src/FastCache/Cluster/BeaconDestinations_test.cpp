// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/BeaconDestinations.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <core/platform/Clock.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using namespace std::chrono_literals;

namespace
{
/// The beacon port every destination below carries.
constexpr std::uint16_t Port = 6681;

/// The refresh interval every case measures against.
constexpr auto Interval = 30s;

/// An interface address, up and not loopback unless a case says otherwise.
/// @param name The interface's name.
/// @param octets The address.
/// @param prefix Its on-link prefix.
/// @param up Whether its link is up.
/// @param loopback Whether it is a loopback interface.
/// @return The address.
[[nodiscard]] Ipv4InterfaceAddress Interface(std::string name,
                                             std::array<std::uint8_t, 4> octets,
                                             std::uint8_t prefix,
                                             bool up = true,
                                             bool loopback = false,
                                             bool broadcastLink = true)
{
    return Ipv4InterfaceAddress { .interfaceName = std::move(name),
                                  .address = octets,
                                  .prefixLength = prefix,
                                  .up = up,
                                  .loopback = loopback,
                                  .broadcastLink = broadcastLink };
}

/// The LAN the reviewer's host was on, and the VPN-type adapter its limited broadcast left by.
[[nodiscard]] Ipv4InterfaceAddress Lan()
{
    return Interface("Ethernet", { 192, 168, 86, 24 }, 24);
}

/// @copydoc Lan
[[nodiscard]] Ipv4InterfaceAddress Vpn()
{
    return Interface("vEthernet (VPN)", { 172, 31, 255, 2 }, 24);
}

/// @param destinations Where beacons go.
/// @return Their hosts, in order, each carrying `Port` or the case fails.
[[nodiscard]] std::vector<std::string> HostsOf(std::vector<core::net::DatagramAddress> const& destinations)
{
    auto hosts = std::vector<std::string> {};
    for (auto const& destination: destinations)
    {
        CHECK(destination.port == Port);
        hosts.push_back(destination.host);
    }
    return hosts;
}

/// @param logger Where lines were captured.
/// @param level Which level to count.
/// @param needle What a line must contain.
/// @return How many lines of @p level contain @p needle.
[[nodiscard]] std::size_t Lines(CapturingLogger const& logger, LogLevel level, std::string_view needle)
{
    auto const records = logger.Snapshot();
    return static_cast<std::size_t>(std::ranges::count_if(records, [level, needle](CapturingLogger::Record const& record) {
        return record.level == level && record.message.contains(needle);
    }));
}
} // namespace

TEST_CASE("Every eligible interface is beaconed at its own directed broadcast, once per subnet",
          "[cluster][discovery][beacon]")
{
    // The whole point of the default: the LAN hears this node whichever interface the stack would
    // have guessed for the limited broadcast. Two interfaces, three, and two addresses on one
    // subnet -- which one directed broadcast reaches both of.
    core::platform::ManualClock clock;
    CapturingLogger logger;

    auto const two = Testing::ScriptedInterfaceAddresses { { Lan(), Vpn() } };
    CHECK(HostsOf(DirectedBroadcastDestinations { two, clock, Port, logger, Interval }.Destinations())
          == std::vector<std::string> { "192.168.86.255", "172.31.255.255" });

    auto const three = Testing::ScriptedInterfaceAddresses { { Lan(), Vpn(), Interface("Wi-Fi", { 10, 20, 0, 5 }, 16) } };
    CHECK(HostsOf(DirectedBroadcastDestinations { three, clock, Port, logger, Interval }.Destinations())
          == std::vector<std::string> { "192.168.86.255", "172.31.255.255", "10.20.255.255" });

    auto const sharing =
        Testing::ScriptedInterfaceAddresses { { Lan(), Interface("Ethernet 2", { 192, 168, 86, 40 }, 24) } };
    CHECK(HostsOf(DirectedBroadcastDestinations { sharing, clock, Port, logger, Interval }.Destinations())
          == std::vector<std::string> { "192.168.86.255" });

    // And where it beacons is said, once per set.
    CHECK(Lines(logger, LogLevel::Info, "beaconing to 192.168.86.255:6681, 172.31.255.255:6681 --") == 1);
}

TEST_CASE("Loopback, down, link-local and point-to-point interfaces are passed over", "[cluster][discovery][beacon]")
{
    // Each excluded interface beside the LAN leaves exactly the LAN's broadcast; each ALONE leaves
    // nothing -- so what is asserted is the exclusion, not the LAN happening to come first.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto const excluded = std::array {
        Interface("Loopback", { 127, 0, 0, 1 }, 8, true, true),
        Interface("Ethernet 3", { 192, 168, 50, 7 }, 24, false),
        Interface("Unidentified", { 169, 254, 12, 1 }, 16),
        Interface("Tunnel", { 10, 99, 0, 2 }, 32),
        // A /24 whose LINK has no broadcast: WireGuard's wg0 is IFF_POINTOPOINT without
        // IFF_BROADCAST, so its prefix alone would have made it a destination.
        Interface("wg0", { 10, 0, 0, 2 }, 24, true, false, false),
    };
    for (auto const& entry: excluded)
    {
        INFO(entry.interfaceName);
        auto const beside = Testing::ScriptedInterfaceAddresses { { entry, Lan() } };
        CHECK(HostsOf(DirectedBroadcastDestinations { beside, clock, Port, logger, Interval }.Destinations())
              == std::vector<std::string> { "192.168.86.255" });
        auto const alone = Testing::ScriptedInterfaceAddresses { { entry } };
        CHECK(DirectedBroadcastDestinations { alone, clock, Port, logger, Interval }.Destinations().empty());
    }
}

TEST_CASE("The interfaces are asked again on an interval, never per beacon", "[cluster][discovery][beacon]")
{
    // A VPN coming up mid-run is picked up at the next refresh, and not before: the probe count is
    // what separates that from asking the platform at every beacon, which answers the same.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto machine = Testing::ScriptedInterfaceAddresses { { Lan() } };
    auto const destinations = DirectedBroadcastDestinations { machine, clock, Port, logger, Interval };
    REQUIRE(machine.Calls() == 1);

    for ([[maybe_unused]] auto const beacon: std::views::iota(0, 20))
        CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    CHECK(machine.Calls() == 1);

    machine.Publish({ Lan(), Vpn() });
    clock.advance(Interval - 1s);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    CHECK(machine.Calls() == 1);

    clock.advance(1s);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255", "172.31.255.255" });
    CHECK(machine.Calls() == 2);
}

TEST_CASE("No eligible interface is said by name with the remedy, once, and its return is said too",
          "[cluster][discovery][beacon]")
{
    // Never a silent no-op: a node that beacons nowhere says so, naming each interface it passed over
    // and why, and how to beacon somewhere anyway. Said once while nothing changes -- the refresh
    // repeats every interval -- and the recovery is said when a link comes up.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto machine = Testing::ScriptedInterfaceAddresses { { Interface("Loopback", { 127, 0, 0, 1 }, 8, true, true),
                                                           Interface("Unidentified", { 169, 254, 12, 1 }, 16) } };
    auto const destinations = DirectedBroadcastDestinations { machine, clock, Port, logger, Interval };

    CHECK(destinations.Destinations().empty());
    CHECK(Lines(logger, LogLevel::Warn, "no interface to beacon on") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "Loopback 127.0.0.1/8 (loopback") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "Unidentified 169.254.12.1/16 (link-local") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "--discovery=<address>:6681") == 1);
    // And the remedy for a machine no beacon can leave at all, which naming an address does not
    // help: the fleet is reached by a seed or by DNS, which need no broadcast link.
    CHECK(Lines(logger, LogLevel::Warn, "--fleet-seed=<host[:port]>") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "DNS SRV record, _fastcache._tcp.<domain>") == 1);

    clock.advance(Interval);
    CHECK(destinations.Destinations().empty());
    clock.advance(Interval);
    CHECK(destinations.Destinations().empty());
    CHECK(Lines(logger, LogLevel::Warn, "no interface to beacon on") == 1);

    machine.Publish({ Lan() });
    clock.advance(Interval);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    CHECK(Lines(logger, LogLevel::Info, "beaconing to 192.168.86.255:6681 --") == 1);

    // And a machine that reported no IPv4 interface at all says that, rather than an empty list.
    CapturingLogger bare;
    auto const nothing = Testing::ScriptedInterfaceAddresses {};
    CHECK(DirectedBroadcastDestinations { nothing, clock, Port, bare, Interval }.Destinations().empty());
    CHECK(Lines(bare, LogLevel::Warn, "reported no IPv4 interface at all") == 1);
}

TEST_CASE("A failed interface walk is said with its error and keeps the destinations it had", "[cluster][discovery][beacon]")
{
    // A walk that failed is not a machine with no interfaces: "reported no IPv4 interface at all" is
    // a fact the platform never stated, and the error it did state was lost. The destinations stay
    // what they were -- a stale set degrades safely -- and the error is said once, by name. The walk
    // that succeeds afterwards says where beacons go, although nothing changed.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto const failure = std::make_error_code(std::errc::not_enough_memory);
    auto machine = Testing::ScriptedInterfaceAddresses { { Lan() } };
    auto const destinations = DirectedBroadcastDestinations { machine, clock, Port, logger, Interval };
    REQUIRE(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });

    machine.FailWith(failure);
    clock.advance(Interval);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    clock.advance(Interval);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    CHECK(Lines(logger, LogLevel::Warn, "could not list this machine's interfaces (" + failure.message() + ")") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "beacons go to 192.168.86.255:6681 until they can be") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "no interface to beacon on") == 0);
    CHECK(Lines(logger, LogLevel::Warn, "reported no IPv4 interface at all") == 0);

    // The end is said once no walk has failed for a whole interval: not at the first success.
    machine.Publish({ Lan() });
    clock.advance(Interval);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    CHECK(Lines(logger, LogLevel::Info, "beaconing to 192.168.86.255:6681 --") == 1);
    clock.advance(Interval);
    CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    CHECK(Lines(logger, LogLevel::Info, "beaconing to 192.168.86.255:6681 --") == 2);

    // Failing from the first walk: nowhere to beacon, and still the error rather than an empty machine.
    CapturingLogger early;
    auto failing = Testing::ScriptedInterfaceAddresses {};
    failing.FailWith(failure);
    CHECK(DirectedBroadcastDestinations { failing, clock, Port, early, Interval }.Destinations().empty());
    CHECK(Lines(early, LogLevel::Warn, "could not list this machine's interfaces") == 1);
    CHECK(Lines(early, LogLevel::Warn, "so beacons go nowhere until they can be") == 1);
    CHECK(Lines(early, LogLevel::Warn, "reported no IPv4 interface at all") == 0);
}

TEST_CASE("A walk that fails every other time is one line, and its end waits for a quiet interval",
          "[cluster][discovery][beacon]")
{
    // The walk's version of the flapping link: said once per error and ended at the first success,
    // a walk that fails every other refresh was a Warn and an Info a minute, 2880 lines a day. The
    // same error is one error, and nothing ends until a whole interval has passed without one.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto const failure = std::make_error_code(std::errc::not_enough_memory);
    auto machine = Testing::ScriptedInterfaceAddresses { { Lan() } };
    auto const destinations = DirectedBroadcastDestinations { machine, clock, Port, logger, Interval };
    constexpr auto Refreshes = 40;

    for (auto const refresh: std::views::iota(0, Refreshes))
    {
        if (refresh % 2 == 0)
            machine.FailWith(failure);
        else
            machine.Publish({ Lan() });
        clock.advance(Interval);
        CHECK(HostsOf(destinations.Destinations()) == std::vector<std::string> { "192.168.86.255" });
    }
    CHECK(Lines(logger, LogLevel::Warn, "could not list this machine's interfaces") == 1);
    CHECK(Lines(logger, LogLevel::Info, "beaconing to 192.168.86.255:6681 --") == 1); // the first sample only
    CHECK(machine.Calls() == static_cast<std::size_t>(Refreshes + 1));

    // It stops failing: one interval with no failure, and the end is said once.
    machine.Publish({ Lan() });
    clock.advance(Interval);
    (void) destinations.Destinations();
    clock.advance(Interval);
    (void) destinations.Destinations();
    CHECK(Lines(logger, LogLevel::Info, "beaconing to 192.168.86.255:6681 --") == 2);
    CHECK(Lines(logger, LogLevel::Warn, "could not list this machine's interfaces") == 1);
}

TEST_CASE("Walk errors that keep changing are said at most once an interval, with how many a line stands for",
          "[cluster][discovery][beacon]")
{
    // The throttle the flap above never reaches once a repeated error is one error: a walk failing a
    // DIFFERENT way at every refresh. One line an interval, the later ones counting the errors.
    core::platform::ManualClock clock;
    CapturingLogger logger;
    auto machine = Testing::ScriptedInterfaceAddresses { { Lan() } };
    auto const destinations = DirectedBroadcastDestinations { machine, clock, Port, logger, Interval };
    static_assert(DirectedBroadcastDestinations::WalkFailureReportInterval == 2 * Interval,
                  "the counts below assume a line at most every second refresh");

    for (auto const refresh: std::views::iota(0, 5))
    {
        machine.FailWith(refresh % 2 == 0 ? std::make_error_code(std::errc::not_enough_memory)
                                          : std::make_error_code(std::errc::io_error));
        clock.advance(Interval);
        (void) destinations.Destinations();
    }
    // 30s says the first; 60s is counted; 90s is due for both; 120s counted; 150s due for both.
    CHECK(Lines(logger, LogLevel::Warn, "could not list this machine's interfaces") == 3);
    CHECK(Lines(logger, LogLevel::Warn, "; 1 new error(s) since the last such line") == 1);
    CHECK(Lines(logger, LogLevel::Warn, "; 2 new error(s) since the last such line") == 2);
}

TEST_CASE("A fixed destination is exactly the address it was given", "[cluster][discovery][beacon]")
{
    auto const fixed = FixedBeaconDestination { core::net::DatagramAddress { .host = "255.255.255.255", .port = Port } };
    auto const destinations = fixed.Destinations();
    REQUIRE(destinations.size() == 1);
    CHECK(destinations.front().host == "255.255.255.255");
    CHECK(destinations.front().port == Port);
}

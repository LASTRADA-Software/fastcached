// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/InMemoryDatagram.hpp>
#include <FastCache/Net/UdpSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{
/// The bytes of @p text, as a datagram payload.
[[nodiscard]] std::span<std::byte const> Bytes(std::string_view text)
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// @p datagram's payload as text.
[[nodiscard]] std::string Text(ReceivedDatagram const& datagram)
{
    return { reinterpret_cast<char const*>(datagram.payload.data()), datagram.payload.size() };
}

/// A node's address on the test segment.
///
/// Every node here answers on one port, because what these cases are about is
/// which inbox a datagram reaches -- so naming the port at each of two dozen call
/// sites would be noise around the half that matters. Named `AtHost` and not
/// `At` because `DiscoveryService_test`'s `AtEndpoint` takes whole `host:port`
/// text: both land in one test binary, and two same-named helpers whose
/// contracts differ by an invisible `:7000` is how a case comes to address
/// nowhere at all.
/// @param host The node's address.
/// @return Its bus address.
[[nodiscard]] DatagramAddress AtHost(std::string_view host)
{
    return DatagramAddress { .host = std::string { host }, .port = 7000 };
}
} // namespace

TEST_CASE("DatagramBus delivers a unicast to exactly one inbox", "[net][datagram]")
{
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));
    auto bob = bus.Open(AtHost("10.0.0.2"));
    auto carol = bus.Open(AtHost("10.0.0.3"));

    REQUIRE(alice->Send(Bytes("hello"), AtHost("10.0.0.2")).has_value());

    auto const received = bob->Receive(10ms);
    REQUIRE(received.has_value());
    CHECK(Text(*received) == "hello");
    CHECK(received->from == AtHost("10.0.0.1"));

    // Nobody else sees it.
    CHECK_FALSE(carol->Receive(10ms).has_value());
}

TEST_CASE("DatagramBus delivers a broadcast to everyone, sender included", "[net][datagram]")
{
    // Sender included, because that is what a real broadcast does -- and it is
    // exactly the case PeerDirectory has to ignore. A double that quietly spared
    // the sender would hide the bug where a lone node records its own beacon and
    // proposes a membership change to admit itself.
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));
    auto bob = bus.Open(AtHost("10.0.0.2"));

    REQUIRE(alice->Send(Bytes("beacon"), DatagramBus::BroadcastAddress()).has_value());

    auto const atBob = bob->Receive(10ms);
    REQUIRE(atBob.has_value());
    CHECK(Text(*atBob) == "beacon");

    auto const atAlice = alice->Receive(10ms);
    REQUIRE(atAlice.has_value());
    CHECK(Text(*atAlice) == "beacon");
    CHECK(atAlice->from == AtHost("10.0.0.1"));
}

TEST_CASE("DatagramBus loses what it is told to lose", "[net][datagram]")
{
    // Scripted rather than random: a test that dropped datagrams by chance would
    // fail occasionally for reasons nobody could reproduce, and what discovery
    // has to survive is a specific peer going quiet, not a global rate.
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));
    auto bob = bus.Open(AtHost("10.0.0.2"));
    auto carol = bus.Open(AtHost("10.0.0.3"));

    bus.DropNext(AtHost("10.0.0.2"), 2);

    for (auto const* const text: { "one", "two", "three" })
        REQUIRE(alice->Send(Bytes(text), DatagramBus::BroadcastAddress()).has_value());

    // Bob lost the first two; the third gets through.
    auto const atBob = bob->Receive(10ms);
    REQUIRE(atBob.has_value());
    CHECK(Text(*atBob) == "three");

    // Carol was never starved, so she has all three -- a drop is per destination,
    // which is the partition shape a global drop rate cannot express.
    //
    // Dereferenced directly rather than through `Unwrap`: that helper exists for
    // `std::optional`, which clang-tidy cannot see a REQUIRE guard through.
    // `std::expected` is not covered by that check, and routing one through it
    // does not compile.
    for (auto const* const expected: { "one", "two", "three" })
    {
        auto const next = carol->Receive(10ms);
        REQUIRE(next.has_value());
        CHECK(Text(*next) == expected);
    }
}

TEST_CASE("A datagram to nobody is discarded, not reported", "[net][datagram]")
{
    // What UDP does. Reporting it would hand the layer above a delivery signal
    // the real network cannot provide, and discovery would come to rely on it.
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));

    CHECK(alice->Send(Bytes("into the void"), AtHost("10.0.0.9")).has_value());
    CHECK(bus.SendCount() == 1);
}

TEST_CASE("A closed datagram socket stops its receive loop", "[net][datagram]")
{
    // The property the whole `Receive(timeout)` shape exists for: POSIX does not
    // unblock a parked receive when another thread closes the socket, so a loop
    // that could not observe a shutdown would hang a `systemctl stop` -- which
    // this repository has already paid for once on accept().
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));

    alice->Close();

    auto const result = alice->Receive(10ms);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DatagramWait::Closed);
}

TEST_CASE("An idle datagram socket times out rather than blocking", "[net][datagram]")
{
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));

    auto const result = alice->Receive(5ms);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DatagramWait::TimedOut);
}

TEST_CASE("A real UDP socket round-trips a datagram", "[net][datagram][smoke]")
{
    // The double above is where discovery's logic is tested; this is the one case
    // that proves the real implementation behind the same seam actually sends and
    // receives -- an interface with only a fake behind it is an interface nobody
    // has checked.
    //
    // Loopback and kernel-chosen ports, so it needs no fixture and cannot collide
    // with anything else on the machine.
    auto receiver = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off);
    REQUIRE(receiver != nullptr);

    auto const bound = receiver->BoundAddress();
    REQUIRE_FALSE(bound.host.empty());
    REQUIRE(bound.port != 0);

    auto sender = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off);
    REQUIRE(sender != nullptr);

    REQUIRE(sender->Send(Bytes("over the wire"), bound).has_value());

    auto const received = receiver->Receive(2s);
    REQUIRE(received.has_value());
    CHECK(Text(*received) == "over the wire");
    CHECK(received->from == sender->BoundAddress());
}

TEST_CASE("A real UDP socket reports an address it cannot use", "[net][datagram][smoke]")
{
    auto sender = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off);
    REQUIRE(sender != nullptr);

    // `.invalid` is reserved by RFC 6761 and guaranteed not to resolve, so this
    // is the resolution failure rather than a DNS round trip. It is what remains
    // of this case now that a malformed `host:port` cannot reach this layer at
    // all: the halves arrive apart, so the only address a caller can still get
    // wrong is one that names nothing.
    auto const result = sender->Send(Bytes("x"), DatagramAddress { .host = "example.invalid", .port = 7000 });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == NetErrorCode::AddressNotAvail);

    // An address with no host at all is refused before resolution, and that is
    // not the same check: `getaddrinfo("", ...)` is EAI_NONAME on glibc but
    // SUCCEEDS on Winsock, resolving to the local host. Left to the resolver, a
    // datagram aimed at a sender this process could not render would be refused
    // on one platform and quietly sent to loopback on the other. It is what
    // `BoundAddress` reports for a socket it cannot name.
    auto const nowhere = sender->Send(Bytes("x"), DatagramAddress { .host = "", .port = 7000 });
    REQUIRE_FALSE(nowhere.has_value());
    CHECK(nowhere.error().code == NetErrorCode::AddressNotAvail);
}

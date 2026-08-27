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

#include <tests/DatagramPayload.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::DatagramBytes;
using FastCache::Testing::DatagramText;

namespace
{
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

    REQUIRE(alice->Send(DatagramBytes("hello"), AtHost("10.0.0.2")).has_value());

    auto const received = bob->Receive(10ms);
    REQUIRE(received.has_value());
    CHECK(DatagramText(*received) == "hello");
    CHECK(received->from == AtHost("10.0.0.1"));

    // Nobody else sees it.
    CHECK_FALSE(carol->Receive(10ms).has_value());
}

TEST_CASE("Two sockets sharing one address both hear a broadcast; one hears a unicast", "[net][datagram]")
{
    // The asymmetry the whole shared-port defect rests on, and the reason this
    // double models a shared address at all. A UDP port is shareable -- every
    // node on a segment binds the beacon port, co-hosted nodes on one machine
    // included -- and sharing it buys HEARING the broadcast and nothing else.
    DatagramBus bus;
    auto first = bus.Open(AtHost("10.0.0.1"));
    auto second = bus.Open(AtHost("10.0.0.1"));
    auto sender = bus.Open(AtHost("10.0.0.9"));

    REQUIRE(sender->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddress()).has_value());

    for (auto* socket: { first.get(), second.get() })
    {
        auto const heard = socket->Receive(10ms);
        REQUIRE(heard.has_value());
        CHECK(DatagramText(*heard) == "beacon");
    }

    REQUIRE(sender->Send(DatagramBytes("challenge"), AtHost("10.0.0.1")).has_value());

    // The first to have attached, deterministically: a real kernel's answer
    // differs between platforms -- Windows 11 hands it to the first-bound socket
    // and Linux to the last -- so what is asserted is that exactly ONE of them
    // gets it, which is what every platform agrees on and what the layer above
    // has to survive.
    auto const atFirst = first->Receive(10ms);
    REQUIRE(atFirst.has_value());
    CHECK(DatagramText(*atFirst) == "challenge");
    CHECK_FALSE(second->Receive(10ms).has_value());
}

TEST_CASE("A broadcast to a port passes over a socket on another one", "[net][datagram]")
{
    // What `255.255.255.255:P` does. It matters as soon as one node holds sockets
    // on two ports, which is the shape `AnswerFromOwnAddress` gives it: a beacon
    // must reach the socket listening for beacons and not the one waiting for the
    // answer to one, or the node challenges every peer twice per beacon and
    // rejects the first proof back as a bad key.
    DatagramBus bus;
    auto listener = bus.Open(DatagramAddress { .host = "10.0.0.1", .port = 6681 });
    auto own = bus.Open(DatagramAddress { .host = "10.0.0.1", .port = 40001 });

    REQUIRE(own->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddressOn(6681)).has_value());

    auto const atListener = listener->Receive(10ms);
    REQUIRE(atListener.has_value());
    CHECK(DatagramText(*atListener) == "beacon");
    CHECK_FALSE(own->Receive(10ms).has_value());
}

TEST_CASE("Closing one of two sockets on one address leaves the other serving", "[net][datagram]")
{
    // Sharing an address is not sharing a socket. A double that gave one address
    // one inbox would report the survivor closed -- and, worse, would have handed
    // both nodes the same queue all along.
    DatagramBus bus;
    auto first = bus.Open(AtHost("10.0.0.1"));
    auto second = bus.Open(AtHost("10.0.0.1"));
    auto sender = bus.Open(AtHost("10.0.0.9"));

    first->Close();

    auto const closed = first->Receive(10ms);
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error() == DatagramWait::Closed);

    REQUIRE(sender->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddress()).has_value());

    auto const heard = second->Receive(10ms);
    REQUIRE(heard.has_value());
    CHECK(DatagramText(*heard) == "beacon");
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

    REQUIRE(alice->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddress()).has_value());

    auto const atBob = bob->Receive(10ms);
    REQUIRE(atBob.has_value());
    CHECK(DatagramText(*atBob) == "beacon");

    auto const atAlice = alice->Receive(10ms);
    REQUIRE(atAlice.has_value());
    CHECK(DatagramText(*atAlice) == "beacon");
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

    // Asserted, not discarded: starving an address nobody holds is a no-op, and
    // the case below would then be green because the datagram never arrived
    // rather than because it was dropped.
    REQUIRE(bus.DropNext(AtHost("10.0.0.2"), 2) == 1);

    for (auto const* const text: { "one", "two", "three" })
        REQUIRE(alice->Send(DatagramBytes(text), DatagramBus::BroadcastAddress()).has_value());

    // Bob lost the first two; the third gets through.
    auto const atBob = bob->Receive(10ms);
    REQUIRE(atBob.has_value());
    CHECK(DatagramText(*atBob) == "three");

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
        CHECK(DatagramText(*next) == expected);
    }
}

TEST_CASE("A datagram to nobody is discarded, not reported", "[net][datagram]")
{
    // What UDP does. Reporting it would hand the layer above a delivery signal
    // the real network cannot provide, and discovery would come to rely on it.
    DatagramBus bus;
    auto alice = bus.Open(AtHost("10.0.0.1"));

    CHECK(alice->Send(DatagramBytes("into the void"), AtHost("10.0.0.9")).has_value());
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

    REQUIRE(sender->Send(DatagramBytes("over the wire"), bound).has_value());

    auto const received = receiver->Receive(2s);
    REQUIRE(received.has_value());
    CHECK(DatagramText(*received) == "over the wire");
    CHECK(received->from == sender->BoundAddress());
}

TEST_CASE("Two real UDP sockets on one port: only one is handed a unicast", "[net][datagram][smoke]")
{
    // The kernel behaviour the shared-port fix is premised on, asserted against a
    // real stack rather than trusted. `PortSharing::Shared` lets both of these
    // bind -- which is what a segment needs, since every node listens where the
    // others shout -- and then exactly one of them is handed a unicast. Which one
    // is not portable: measured, Windows 11 picks the first-bound and Ubuntu
    // 24.04 the last, so the assertion is the count.
    //
    // It is also what proves `PortSharing::Shared` means the same thing on all
    // three platforms. SO_REUSEADDR alone would fail the second bind on macOS,
    // and this is the case that would say so.
    //
    // Unicast only. The broadcast half is what `SO_REUSEADDR` is *for* and is not
    // in doubt; asserting it here would mean putting a datagram on the segment
    // from a unit test, which a CI runner may refuse and a colleague's LAN should
    // not have to see.
    //
    // The port is kernel-chosen and then re-bound, rather than a constant: a
    // fixed one collides with whatever else is on the machine, and this suite
    // runs in parallel.
    auto first = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Shared);
    REQUIRE(first != nullptr);
    auto const shared = first->BoundAddress();
    REQUIRE(shared.port != 0);

    auto second = OpenUdpSocket("127.0.0.1", shared.port, BroadcastMode::Off, PortSharing::Shared);
    REQUIRE(second != nullptr);
    CHECK(second->BoundAddress().port == shared.port);

    auto sender = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off);
    REQUIRE(sender != nullptr);
    REQUIRE(sender->Send(DatagramBytes("challenge"), shared).has_value());

    // A short wait on the second of the two, because the interesting outcome --
    // the one that breaks discovery -- is that it never arrives, and that answer
    // is only ever a timeout.
    auto const atFirst = first->Receive(2s);
    auto const atSecond = second->Receive(200ms);

    CHECK(static_cast<int>(atFirst.has_value()) + static_cast<int>(atSecond.has_value()) == 1);
    if (atFirst.has_value())
        CHECK(DatagramText(*atFirst) == "challenge");
    else
        CHECK(DatagramText(*atSecond) == "challenge");
}

TEST_CASE("An exclusive UDP socket keeps its address to itself", "[net][datagram][smoke]")
{
    // The other half of the contract, and the reason `PortSharing` is a parameter
    // rather than something every UDP socket gets. Sharing is what a beacon port
    // needs; a socket whose whole job is that the answer addressed to it arrives
    // at IT must not share, and nothing would notice if it quietly did -- the
    // datagrams would simply go to the wrong process now and then.
    auto held = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Exclusive);
    REQUIRE(held != nullptr);
    auto const bound = held->BoundAddress();
    REQUIRE(bound.port != 0);

    CHECK(OpenUdpSocket("127.0.0.1", bound.port, BroadcastMode::Off, PortSharing::Exclusive) == nullptr);
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
    auto const result = sender->Send(DatagramBytes("x"), DatagramAddress { .host = "example.invalid", .port = 7000 });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == NetErrorCode::AddressNotAvail);

    // An address with no host at all is refused before resolution, and that is
    // not the same check: `getaddrinfo("", ...)` is EAI_NONAME on glibc but
    // SUCCEEDS on Winsock, resolving to the local host. Left to the resolver, a
    // datagram aimed at a sender this process could not render would be refused
    // on one platform and quietly sent to loopback on the other. It is what
    // `BoundAddress` reports for a socket it cannot name.
    auto const nowhere = sender->Send(DatagramBytes("x"), DatagramAddress { .host = "", .port = 7000 });
    REQUIRE_FALSE(nowhere.has_value());
    CHECK(nowhere.error().code == NetErrorCode::AddressNotAvail);
}

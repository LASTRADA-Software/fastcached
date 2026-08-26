// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/InMemoryDatagram.hpp>
#include <FastCache/Net/SharedPortDatagram.hpp>
#include <FastCache/Net/UdpSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
/// The port every node in these cases listens for broadcasts on.
///
/// Named rather than repeated, because every case here turns on the difference
/// between "the port everybody bound" and "the address only I hold", and a
/// literal at each call site is how those two come to be the same number.
constexpr std::uint16_t BeaconPort = 6681;

/// One node's pair: the shared beacon port, and an address of its own.
/// @param bus The segment.
/// @param host The machine this node runs on; two co-hosted nodes share it.
/// @param ownPort The port only this node holds.
/// @return The pair, as one socket.
[[nodiscard]] std::unique_ptr<IDatagramSocket> NodeSocket(DatagramBus& bus, std::string_view host, std::uint16_t ownPort)
{
    return AnswerFromOwnAddress(bus.Open(DatagramAddress { .host = std::string { host }, .port = BeaconPort }),
                                bus.Open(DatagramAddress { .host = std::string { host }, .port = ownPort }));
}
} // namespace

TEST_CASE("A shared-port socket sends from the address only it holds", "[net][datagram][sharedport]")
{
    // The whole rule, in one assertion. What a peer replies to is the sender
    // address, so a datagram that went out of the shared socket would have it
    // answered at a port a co-hosted node may hold too.
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.Open(DatagramAddress { .host = "10.0.0.2", .port = BeaconPort });

    REQUIRE(node->Send(DatagramBytes("beacon"), peer->BoundAddress()).has_value());

    auto const received = peer->Receive(10ms);
    REQUIRE(received.has_value());
    CHECK(DatagramText(*received) == "beacon");
    CHECK(received->from == DatagramAddress { .host = "10.0.0.1", .port = 40001 });
    CHECK(received->from == node->BoundAddress());
}

TEST_CASE("A shared-port socket receives on both of its halves", "[net][datagram][sharedport]")
{
    // A beacon arrives on the shared half and an answer on the private one, and
    // the caller above sees one stream. Sent before either is read, so the case
    // does not depend on which half is polled first -- that alternates.
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.Open(DatagramAddress { .host = "10.0.0.2", .port = BeaconPort });

    REQUIRE(peer->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddressOn(BeaconPort)).has_value());
    REQUIRE(peer->Send(DatagramBytes("proof"), node->BoundAddress()).has_value());

    std::vector<std::string> heard;
    for (auto attempt = 0; attempt < 2; ++attempt)
    {
        auto const received = node->Receive(10ms);
        REQUIRE(received.has_value());
        heard.push_back(DatagramText(*received));
    }

    std::ranges::sort(heard);
    CHECK(heard == std::vector<std::string> { "beacon", "proof" });
}

TEST_CASE("A broadcast reaches the shared half and not the private one", "[net][datagram][sharedport]")
{
    // Only once. A node whose private socket also heard the beacon port would
    // see every beacon twice, challenge every peer twice per beacon, and reject
    // the first proof back as an answer to a nonce it had already replaced.
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.Open(DatagramAddress { .host = "10.0.0.2", .port = BeaconPort });

    REQUIRE(peer->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddressOn(BeaconPort)).has_value());

    auto const once = node->Receive(10ms);
    REQUIRE(once.has_value());
    CHECK(DatagramText(*once) == "beacon");
    CHECK_FALSE(node->Receive(10ms).has_value());
}

TEST_CASE("Two co-hosted shared-port sockets each get their own answers", "[net][datagram][sharedport]")
{
    // The configuration the whole class exists for: one machine, two nodes, one
    // beacon port. Both hear the broadcast, and each is answered at an address
    // the other does not hold -- which is what a single shared socket could not
    // give them, because a unicast to the port they share reaches only one.
    DatagramBus bus;
    auto first = NodeSocket(bus, "10.0.0.1", 40001);
    auto second = NodeSocket(bus, "10.0.0.1", 40002);
    auto peer = bus.Open(DatagramAddress { .host = "10.0.0.2", .port = BeaconPort });

    REQUIRE(peer->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddressOn(BeaconPort)).has_value());

    for (auto* const node: { first.get(), second.get() })
    {
        auto const beacon = node->Receive(10ms);
        REQUIRE(beacon.has_value());
        CHECK(DatagramText(*beacon) == "beacon");
    }

    REQUIRE(peer->Send(DatagramBytes("for first"), first->BoundAddress()).has_value());
    REQUIRE(peer->Send(DatagramBytes("for second"), second->BoundAddress()).has_value());

    auto const atFirst = first->Receive(10ms);
    REQUIRE(atFirst.has_value());
    CHECK(DatagramText(*atFirst) == "for first");

    auto const atSecond = second->Receive(10ms);
    REQUIRE(atSecond.has_value());
    CHECK(DatagramText(*atSecond) == "for second");
}

TEST_CASE("Closing a shared-port socket stops its receive loop", "[net][datagram][sharedport]")
{
    // The property every `Receive(timeout)` in this tree exists for: nothing but
    // a poll return can tell a loop to stop. It has to hold whichever half is
    // polled first, so it is asserted twice -- and the order alternates, so two
    // calls cover both.
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);

    node->Close();

    for (auto attempt = 0; attempt < 2; ++attempt)
    {
        auto const result = node->Receive(10ms);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == DatagramWait::Closed);
    }
}

TEST_CASE("An idle shared-port socket times out rather than blocking", "[net][datagram][sharedport]")
{
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);

    auto const result = node->Receive(10ms);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DatagramWait::TimedOut);
}

TEST_CASE("A half that could not be bound yields no pair at all", "[net][datagram][sharedport]")
{
    // `OpenUdpSocket` reports a bind it could not make as a null socket, and
    // that has to survive being paired: a pair built around one would fail on
    // the first datagram, on the discovery thread, as a null dereference rather
    // than as the startup refusal the caller already knows how to report.
    DatagramBus bus;
    auto const at = [&bus](std::uint16_t port) {
        return bus.Open(DatagramAddress { .host = "10.0.0.1", .port = port });
    };

    CHECK(AnswerFromOwnAddress(nullptr, at(40001)) == nullptr);
    CHECK(AnswerFromOwnAddress(at(BeaconPort), nullptr) == nullptr);
    CHECK(AnswerFromOwnAddress(nullptr, nullptr) == nullptr);
}

TEST_CASE("A shared-port socket drains a backlog as fast as it is asked", "[net][datagram][sharedport]")
{
    // Throughput, not ordering, and the two are different properties. Halving
    // the caller's timeout across the two halves -- the obvious implementation --
    // means that whenever the half polled first is idle, each datagram queued on
    // the other costs a full half-timeout: eight a second at DiscoveryTier's
    // 250ms poll, which a large enough segment exceeds permanently, and what the
    // kernel drops then includes the proofs.
    //
    // Asserted as elapsed time rather than by reading the implementation, and
    // against a generous ceiling, because what is being ruled out is seconds.
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.Open(DatagramAddress { .host = "10.0.0.2", .port = BeaconPort });

    constexpr auto Backlog = 16;
    for (auto sent = 0; sent < Backlog; ++sent)
        REQUIRE(peer->Send(DatagramBytes("proof"), node->BoundAddress()).has_value());

    auto const startedAt = std::chrono::steady_clock::now();
    for (auto drained = 0; drained < Backlog; ++drained)
    {
        auto const received = node->Receive(250ms);
        REQUIRE(received.has_value());
        CHECK(DatagramText(*received) == "proof");
    }
    auto const elapsed = std::chrono::steady_clock::now() - startedAt;

    // Half-the-timeout would have spent 125ms on the idle half every other
    // call -- Backlog/2 * 125ms, a full second. This costs about a
    // millisecond a datagram, so the ceiling is loose enough for a busy
    // runner and still three times under what it is ruling out.
    CHECK(elapsed < 300ms);
}

TEST_CASE("A shared-port socket polls each half however small the timeout", "[net][datagram][sharedport]")
{
    // Halving a caller's timeout must not round it to nothing. Zero is not "do
    // not wait" to a real socket -- `SO_RCVTIMEO` of zero means block forever --
    // so the half is floored at a millisecond, and both halves are still read.
    DatagramBus bus;
    auto node = NodeSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.Open(DatagramAddress { .host = "10.0.0.2", .port = BeaconPort });

    REQUIRE(peer->Send(DatagramBytes("beacon"), DatagramBus::BroadcastAddressOn(BeaconPort)).has_value());
    REQUIRE(peer->Send(DatagramBytes("proof"), node->BoundAddress()).has_value());

    std::vector<std::string> heard;
    for (auto attempt = 0; attempt < 2; ++attempt)
    {
        auto const received = node->Receive(0ms);
        REQUIRE(received.has_value());
        heard.push_back(DatagramText(*received));
    }

    std::ranges::sort(heard);
    CHECK(heard == std::vector<std::string> { "beacon", "proof" });
}

TEST_CASE("Two real nodes open a pair on one shared port", "[net][datagram][sharedport][smoke]")
{
    // The real stack, because this is where the four options actually land and
    // every wrong pairing of them still starts. Transposing the two ports gives a
    // node answering where the segment shouts; making the listener exclusive
    // gives one that locks every other node on the machine out of hearing
    // beacons. Both show up here and in no in-memory case.
    //
    // Loopback and a kernel-chosen shared port, so it needs no fixture and cannot
    // collide with anything else on the machine -- this suite runs in parallel.
    auto probe = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Shared);
    REQUIRE(probe != nullptr);
    auto const sharedPort = probe->BoundAddress().port;
    REQUIRE(sharedPort != 0);
    probe.reset();

    auto first = OpenSharedPortUdpSocket("127.0.0.1", sharedPort, 0);
    REQUIRE(first != nullptr);

    // The second is the assertion: it binds the SAME shared port, which only a
    // shared listener permits, and gets an answering address of its own.
    auto second = OpenSharedPortUdpSocket("127.0.0.1", sharedPort, 0);
    REQUIRE(second != nullptr);

    // What each reports is where it is ANSWERED, which is never the port the
    // segment broadcasts to and never the other node's.
    CHECK(first->BoundAddress().port != sharedPort);
    CHECK(second->BoundAddress().port != sharedPort);
    CHECK(first->BoundAddress().port != second->BoundAddress().port);

    // And they are answerable apart -- the whole point, over real sockets.
    REQUIRE(first->Send(DatagramBytes("for second"), second->BoundAddress()).has_value());

    auto const atSecond = second->Receive(2s);
    REQUIRE(atSecond.has_value());
    CHECK(DatagramText(*atSecond) == "for second");
    CHECK(atSecond->from == first->BoundAddress());

    CHECK_FALSE(first->Receive(200ms).has_value());
}

TEST_CASE("A shared-port pair can be asked for a named answering port", "[net][datagram][sharedport][smoke]")
{
    // What `--discovery-reply-port` is for: a host firewall that opens named
    // ports only passes the beacons and drops every challenge and proof, which
    // presents as peers seen and never admitted.
    //
    // Both numbers are kernel-chosen and then re-bound, which is the only way to
    // name a port in a suite that runs in parallel. The shared probe is HELD
    // while the second is drawn, so the two cannot come back equal -- pointing
    // both halves at one port is exactly the configuration under test elsewhere,
    // and it would present here as a bind failure nobody could explain.
    auto probe = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Shared);
    REQUIRE(probe != nullptr);
    auto const sharedPort = probe->BoundAddress().port;
    REQUIRE(sharedPort != 0);

    auto namer = OpenUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Exclusive);
    REQUIRE(namer != nullptr);
    auto const ownPort = namer->BoundAddress().port;
    REQUIRE(ownPort != 0);
    REQUIRE(ownPort != sharedPort);
    namer.reset();

    auto node = OpenSharedPortUdpSocket("127.0.0.1", sharedPort, ownPort);
    REQUIRE(node != nullptr);
    CHECK(node->BoundAddress().port == ownPort);

    // And a second node cannot have that one, which is why it is a port per node
    // rather than a port per fleet. Asked for while the first still holds it, so
    // this is the refusal itself and not a race with whoever had it last.
    CHECK(OpenSharedPortUdpSocket("127.0.0.1", sharedPort, ownPort) == nullptr);
}

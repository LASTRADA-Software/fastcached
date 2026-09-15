// SPDX-License-Identifier: Apache-2.0
//
// The dialling end and the accepting end, against EACH OTHER (#1308).
//
// `RaftPeerTransport_test.cpp` scripts an acceptor and `RaftPeerServer_test.cpp` scripts a
// dialler, and both scripts are built on the real session code -- which is what makes them
// honest about the frames, and exactly what makes them unable to see the two production
// ends disagree about the ORDER of the handshake, about which end speaks first, or about a
// refusal one end signs and the other reads as a key mismatch. So this file wires the real
// transport to the real server over one in-memory link and asserts what each end COUNTED,
// because a refusal pinned at one end only is half of a diagnosis.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Cluster/PskRaftPeerCredential.hpp>
#include <FastCache/Consensus/RaftPeerRefusals.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

/// The cluster key the server holds, and a well-behaved dialler with it.
[[nodiscard]] SecureByteBuffer ClusterKey()
{
    return SecureByteBuffer(32, std::byte { 0x5A });
}

/// A key the server does not hold.
[[nodiscard]] SecureByteBuffer StrangerKey()
{
    return SecureByteBuffer(32, std::byte { 0x33 });
}

/// Records every message the server delivers.
class RecordingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        received.push_back(std::move(message));
    }

    std::vector<RaftMessage> received; ///< In arrival order.
};

/// Every dial lands on one in-memory listener, whatever address it names.
class ListenerConnector final: public IConnector
{
  public:
    /// @param listener Where every dial arrives; must outlive the connector.
    explicit ListenerConnector(InMemoryListener& listener) noexcept:
        _listener { listener }
    {
    }

    /// @copydoc IConnector::Connect
    [[nodiscard]] Task<SocketResult> Connect(std::string host, std::uint16_t port, DialOptions options) override
    {
        std::ignore = host;
        std::ignore = port;
        std::ignore = options;
        co_return std::unique_ptr<ISocket> { _listener.ConnectClient() };
    }

  private:
    InMemoryListener& _listener;
};

/// Who is on each end of the link, and what the dialler holds.
struct LinkShape
{
    std::string server { "n2" };                  ///< The id the server is.
    std::string dialler { "n1" };                 ///< The id the transport is.
    std::string target { "n2" };                  ///< The id the transport believes it dials.
    SecureByteBuffer diallerKey { ClusterKey() }; ///< What the transport proves with.
};

/// A transport and a server on one reactor, the transport's only peer being the server.
///
/// Both handshake bounds are zero, which arms no deadline: the reactor is turned by this
/// thread, and a bound is a property the two single-ended files already drive against a
/// clock.
struct Link
{
    /// @param shape Who is on each end.
    explicit Link(LinkShape shape):
        target { shape.target },
        dialler { shape.dialler },
        diallerKey { std::move(shape.diallerKey) },
        server { listener,
                 reactor,
                 sink,
                 logger,
                 serverMetrics,
                 serverKey,
                 NodeId { shape.server },
                 serverRandom,
                 PeerServerOptions { .handshakeBound = 0ms } }
    {
        [](RaftPeerServer* accepting) -> DetachedTask {
            co_await accepting->Run();
        }(&server);

        transport = std::make_unique<RaftPeerTransport>(
            NodeId { shape.dialler },
            std::vector { PeerEndpoint { .id = NodeId { shape.target }, .host = "in-memory", .port = 1 } },
            reactor,
            connector,
            logger,
            diallerMetrics,
            diallerKey,
            diallerRandom,
            PeerTransportOptions { .handshakeBound = 0ms });
        transport->Start();
        reactor.Drain();
    }

    Link(Link const&) = delete;
    Link(Link&&) = delete;
    Link& operator=(Link const&) = delete;
    Link& operator=(Link&&) = delete;

    /// Stop the transport and close the listener, each drained on this thread.
    ~Link()
    {
        transport->RequestStop();
        reactor.Drain();
        clock.Advance(50ms);
        reactor.Drain();
        transport.reset();
        listener.Close();
        reactor.Drain();
    }

    /// Hand the transport one vote from the dialler, and let it reach the server.
    /// @param term The vote's term, so two deliveries are distinguishable.
    void SendVote(std::uint64_t term)
    {
        transport->Send(NodeId { target },
                        RaftMessage { RequestVoteResponse { .term = Term { .value = term },
                                                            .decision = VoteDecision::Granted,
                                                            .voterId = NodeId { dialler } } });
        reactor.Drain();
    }

    /// @return How many peers the transport holds a proven session with.
    [[nodiscard]] std::size_t Connected() const noexcept
    {
        return transport->ConnectedPeers();
    }

    /// @param refusal An acceptor refusal.
    /// @return How many times the server counted it.
    [[nodiscard]] std::uint64_t Refused(AcceptorRefusal refusal) const
    {
        return serverMetrics.Read(RowFor(refusal).counter);
    }

    /// @param refusal A dialler refusal.
    /// @return How many times the transport counted it.
    [[nodiscard]] std::uint64_t Refused(DiallerRefusal refusal) const
    {
        return diallerMetrics.Read(RowFor(refusal).counter);
    }

    /// @return Every refusal either end counted, summed.
    [[nodiscard]] std::uint64_t AnyRefusals() const
    {
        auto total = std::uint64_t { 0 };
        for (auto const& row: AcceptorRefusals)
            total += serverMetrics.Read(row.counter);
        for (auto const& row: DiallerRefusals)
            total += diallerMetrics.Read(row.counter);
        return total;
    }

    std::string target;               ///< The id the transport dials.
    std::string dialler;              ///< The id the transport is.
    RecordingSink sink;               ///< What the server delivered.
    AtomicMetricsSink serverMetrics;  ///< What the server counted.
    AtomicMetricsSink diallerMetrics; ///< What the transport counted.
    ManualClock clock;
    TestReactor reactor { clock };
    InMemoryListener listener;
    ListenerConnector connector { listener };
    NullLogger logger;
    Cluster::PskRaftPeerCredential const serverKey { ClusterKey() };
    Cluster::PskRaftPeerCredential const diallerKey;
    SystemRandomSource serverRandom { 0x5E };
    SystemRandomSource diallerRandom { 0xD1 };
    RaftPeerServer server;
    std::unique_ptr<RaftPeerTransport> transport;
};

} // namespace

TEST_CASE("A transport and a server holding one key form a session and deliver", "[consensus][raft][handshake]")
{
    // The control every refusal below needs beside it: the same two ends, the same link,
    // and the only difference is the one each case names.
    Link link { LinkShape {} };
    link.SendVote(1);
    link.SendVote(2);

    CHECK(link.Connected() == 1);
    REQUIRE(link.sink.received.size() == 2);
    CHECK(std::get<RequestVoteResponse>(link.sink.received[0]).term.value == 1);
    CHECK(std::get<RequestVoteResponse>(link.sink.received[1]).term.value == 2);
    CHECK(link.AnyRefusals() == 0);
}

TEST_CASE("A transport with another key is refused by the server, and each end counts its own half",
          "[consensus][raft][handshake]")
{
    // The server cannot sign a verdict for a proof it could not verify, so it closes; the
    // transport reads a close after its proof as the row that names a key mismatch.
    Link link { LinkShape { .diallerKey = StrangerKey() } };
    link.SendVote(1);

    CHECK(link.sink.received.empty());
    CHECK(link.Connected() == 0);
    CHECK(link.Refused(AcceptorRefusal::Proof) == 1);
    CHECK(link.Refused(DiallerRefusal::EndedByAcceptor) == 1);
    CHECK(link.AnyRefusals() == 2);
}

TEST_CASE("A transport dialling the wrong member hears a signed refusal and counts it by name",
          "[consensus][raft][handshake]")
{
    // #1308, A1. Both ends hold the key, so the refusal is SIGNED -- and a transport that
    // counted it as the connection ending would send an operator looking for a key
    // mismatch that is not there. Each end's own row moves, and the key-mismatch row does
    // not.
    Link link { LinkShape { .target = "n9" } };
    link.SendVote(1);

    CHECK(link.sink.received.empty());
    CHECK(link.Connected() == 0);
    CHECK(link.Refused(AcceptorRefusal::WrongTarget) == 1);
    CHECK(link.Refused(DiallerRefusal::WrongTarget) == 1);
    CHECK(link.Refused(DiallerRefusal::EndedByAcceptor) == 0);
    CHECK(link.AnyRefusals() == 2);
}

TEST_CASE("A transport under the server's own id hears a signed refusal and counts it by name",
          "[consensus][raft][handshake]")
{
    // A copied --cluster-dir: a second machine proving the key under an id the server is.
    // Asked before the target, so this is OwnId whatever the transport thought it dialled.
    Link link { LinkShape { .server = "n1", .dialler = "n1", .target = "n3" } };
    link.SendVote(1);

    CHECK(link.sink.received.empty());
    CHECK(link.Connected() == 0);
    CHECK(link.Refused(AcceptorRefusal::OwnId) == 1);
    CHECK(link.Refused(DiallerRefusal::OwnId) == 1);
    CHECK(link.Refused(DiallerRefusal::EndedByAcceptor) == 0);
    CHECK(link.AnyRefusals() == 2);
}

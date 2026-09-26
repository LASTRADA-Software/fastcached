// SPDX-License-Identifier: Apache-2.0
//
// The dialling end and the accepting end, against EACH OTHER (#1308, #178).
//
// `RaftPeerTransport_test.cpp` scripts an acceptor and `RaftPeerServer_test.cpp` scripts a
// dialler, and both scripts are built on the real session code -- which is what makes them
// honest about the frames, and exactly what makes them unable to see the two production
// ends disagree about the ORDER of the handshake, about which end speaks first, or about a
// refusal one end signs and the other reads as a key mismatch. So this file wires the real
// transport to the real server over one in-memory link and asserts what each end COUNTED,
// because a refusal pinned at one end only is half of a diagnosis.
#include <FastCache/Consensus/RaftPeerRefusals.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <core/async/Task.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/ListenerConnector.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

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

/// Who is on each end of the link, and what the dialler holds.
struct LinkShape
{
    std::string server { "n2" };  ///< The id the server is.
    std::string dialler { "n1" }; ///< The id the transport proves itself as.
    std::string target { "n2" };  ///< The id the transport believes it dials.

    /// Whose private key the transport signs with; empty means its own id's.
    std::string diallerMachine {};

    /// What BOTH ends believe about everybody's keys: one cluster's replicated roster, so a
    /// revocation reaches the two ends at once, as an applied forget does.
    std::shared_ptr<Testing::SharedRoster> roster { Testing::SharedRoster::Of({ "n1", "n2", "n3" }) };
};

/// How long the transport waits before redialling, which a case advances the clock past.
constexpr auto ReconnectBackoff = 100ms;

/// A transport and a server on one reactor, the transport's only peer being the server.
///
/// Both handshake bounds are zero, which arms no deadline: the reactor is turned by this
/// thread, and a bound is a property the two single-ended files already drive against a
/// clock.
struct Link
{
    /// @param shape Who is on each end.
    explicit Link(LinkShape const& shape):
        target { shape.target },
        dialler { shape.dialler },
        roster { shape.roster },
        serverIdentity { NodeId { shape.server }, Testing::TestKeyPair(shape.server), roster },
        diallerIdentity { NodeId { shape.dialler },
                          Testing::TestKeyPair(shape.diallerMachine.empty() ? shape.dialler : shape.diallerMachine),
                          roster },
        server { listener,      reactor,        sink,         logger,
                 serverMetrics, serverIdentity, serverRandom, PeerServerOptions { .handshakeBound = 0ms } }
    {
        [](RaftPeerServer* accepting) -> core::async::DetachedTask {
            co_await accepting->Run();
        }(&server);

        transport = std::make_unique<RaftPeerTransport>(
            std::vector { PeerEndpoint { .id = NodeId { shape.target }, .host = "in-memory", .port = 1 } },
            reactor,
            connector,
            logger,
            diallerMetrics,
            diallerIdentity,
            diallerRandom,
            PeerTransportOptions { .reconnectBackoff = ReconnectBackoff, .handshakeBound = 0ms });
        transport->Start();
        reactor.drain();
    }

    Link(Link const&) = delete;
    Link(Link&&) = delete;
    Link& operator=(Link const&) = delete;
    Link& operator=(Link&&) = delete;

    /// Stop the transport and close the listener, each drained on this thread.
    ~Link()
    {
        transport->RequestStop();
        reactor.drain();
        clock.advance(50ms);
        reactor.drain();
        transport.reset();
        listener.close();
        reactor.drain();
    }

    /// Hand the transport one vote from the dialler, and let it reach the server.
    /// @param term The vote's term, so two deliveries are distinguishable.
    void SendVote(std::uint64_t term)
    {
        transport->Send(NodeId { target },
                        RaftMessage { RequestVoteResponse { .term = Term { .value = term },
                                                            .decision = VoteDecision::Granted,
                                                            .voterId = NodeId { dialler } } });
        reactor.drain();
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
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    core::net::testing::InMemoryListener listener;
    Testing::ListenerConnector connector { listener };
    NullLogger logger;
    std::shared_ptr<Testing::SharedRoster> roster;
    Testing::TestPeerIdentity const serverIdentity;
    Testing::TestPeerIdentity const diallerIdentity;
    SystemSecureRandom serverRandom;
    SystemSecureRandom diallerRandom;
    RaftPeerServer server;
    std::unique_ptr<RaftPeerTransport> transport;
};

} // namespace

TEST_CASE("A transport and a server that each prove their id form a session and deliver", "[consensus][raft][handshake]")
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

TEST_CASE("A transport signing with a key that is not its id's is refused by the server, and each end counts its own half",
          "[consensus][raft][handshake]")
{
    // n3, holding every byte it ever held, claiming n1's id. The server cannot sign a verdict
    // for a proof it could not verify, so it closes; the transport reads a close after its
    // proof as the row that names an acceptor that could not verify it.
    Link link { LinkShape { .diallerMachine = "n3" } };
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
    // #1308, A1. Both ends prove their ids, so the refusal is SIGNED -- and a transport that
    // counted it as the connection ending would send an operator looking for a key problem
    // that is not there. Each end's own row moves, and the ended-by-acceptor row does not.
    Link link { LinkShape { .target = "n3" } };
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
    // A copied --cluster-dir: a second machine holding the server's own private key. Asked
    // before the target, so this is OwnId whatever the transport thought it dialled.
    Link link { LinkShape { .server = "n1", .dialler = "n1", .target = "n3" } };
    link.SendVote(1);

    CHECK(link.sink.received.empty());
    CHECK(link.Connected() == 0);
    CHECK(link.Refused(AcceptorRefusal::OwnId) == 1);
    CHECK(link.Refused(DiallerRefusal::OwnId) == 1);
    CHECK(link.Refused(DiallerRefusal::EndedByAcceptor) == 0);
    CHECK(link.AnyRefusals() == 2);
}

TEST_CASE("A key revoked mid-session closes that session, and the redial is refused, signed",
          "[consensus][raft][handshake][revocation]")
{
    // #178's case (b) on one link. The cluster revokes a key while a session it proved is
    // open; the session ends at its next frame, and when the dialler dials again the refusal
    // is SIGNED and says what happened -- which is what an operator reading the counters needs,
    // since nothing else about either machine changed. Both directions, because a cluster has
    // both: every pair of members dials each other.

    SECTION("the revoked machine is the dialler")
    {
        Link link { LinkShape { .server = "n1", .dialler = "n3", .target = "n1" } };
        link.SendVote(1);
        REQUIRE(link.sink.received.size() == 1);
        REQUIRE(link.Connected() == 1);

        link.roster->Revoke("n3");
        link.SendVote(2);

        // The acceptor reads the frame after the revocation and ends the connection there.
        CHECK(link.sink.received.size() == 1);
        CHECK(link.Refused(AcceptorRefusal::KeyWithdrawn) == 1);

        // The acceptor closed having read everything, so the first write after it is accepted
        // and lost -- it draws the reset -- and the one after it fails and ends the session.
        link.SendVote(3);
        link.SendVote(3);
        link.clock.advance(ReconnectBackoff);
        link.reactor.drain();

        CHECK(link.sink.received.size() == 1);
        CHECK(link.Connected() == 0);
        CHECK(link.Refused(AcceptorRefusal::RevokedKey) == 1);
        CHECK(link.Refused(DiallerRefusal::OwnKeyRevoked) == 1);

        // Never reported as a proof nobody could verify: the refusal names the revocation.
        CHECK(link.Refused(DiallerRefusal::EndedByAcceptor) == 0);
        CHECK(link.Refused(AcceptorRefusal::Proof) == 0);
    }

    SECTION("the revoked machine is the acceptor")
    {
        Link link { LinkShape { .server = "n3", .dialler = "n1", .target = "n3" } };
        link.SendVote(1);
        REQUIRE(link.sink.received.size() == 1);
        REQUIRE(link.Connected() == 1);

        link.roster->Revoke("n3");
        link.SendVote(2);

        // The dialler asks before sealing, so the frame after the revocation is never written.
        CHECK(link.sink.received.size() == 1);
        CHECK(link.Refused(DiallerRefusal::KeyWithdrawn) == 1);
        CHECK(link.Connected() == 0);

        link.clock.advance(ReconnectBackoff);
        link.reactor.drain();

        // The redial: n3 still proves n3 with the key it always had, and that key is revoked.
        CHECK(link.sink.received.size() == 1);
        CHECK(link.Connected() == 0);
        CHECK(link.Refused(DiallerRefusal::AcceptorKeyRevoked) == 1);
        CHECK(link.Refused(DiallerRefusal::AcceptorProof) == 0);
    }

    SECTION("the control: the same links with nothing revoked keep delivering")
    {
        Link link { LinkShape { .server = "n1", .dialler = "n3", .target = "n1" } };
        link.SendVote(1);
        link.SendVote(2);
        CHECK(link.sink.received.size() == 2);
        CHECK(link.Connected() == 1);
        CHECK(link.AnyRefusals() == 0);
    }
}

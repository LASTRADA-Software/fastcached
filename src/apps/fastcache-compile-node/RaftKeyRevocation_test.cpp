// SPDX-License-Identifier: Apache-2.0
//
// Removing ONE machine from a cluster, end to end on the Raft peer wire (#178).
//
// Under the pre-shared key, removing a machine meant rotating the key on every other one: the
// removed machine still held it, and holding it WAS membership. Here each machine proves its
// own key, the cluster revokes one, and nothing else changes anywhere -- which is the claim
// this file checks, with every production piece that carries it: the key files as a start
// mints and reads them, the replicated `RevokeKey`, the roster each node adopts from the state,
// and the real server and transport between them.
#include "NodeKey.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftPeerRefusals.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <tests/ListenerConnector.hpp>
#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::ScratchDirectory;

namespace
{

/// How long a transport waits before redialling, which the case advances the clock past.
constexpr auto ReconnectBackoff = 100ms;

/// Every byte of @p path, in one read.
/// @param path A file.
/// @return Its contents.
[[nodiscard]] std::string BytesOf(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return std::move(bytes).str();
}

/// Records what the server delivered.
class RecordingSink final: public Consensus::IRaftMessageSink
{
  public:
    /// @copydoc Consensus::IRaftMessageSink::Deliver
    void Deliver(Consensus::RaftMessage message) override
    {
        received.push_back(std::move(message));
    }

    std::vector<Consensus::RaftMessage> received; ///< In arrival order.
};

/// Mint a key in @p stateDirectory the way a machine's first start does.
/// @param stateDirectory The machine's state directory, holding no key yet.
/// @return The key pair it minted.
[[nodiscard]] Ed25519KeyPair MintKey(std::filesystem::path const& stateDirectory)
{
    SystemSecureRandom random;
    auto resolved = ResolveNodeKey(stateDirectory, random);
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->origin == NodeKeyOrigin::Minted);
    return std::move(resolved->pair);
}

/// One machine: its state directory, and the key its first start mints there.
struct Machine
{
    /// Mint this machine's key the way its first start does.
    /// @param machineId Its id.
    /// @param scratch Where state directories go.
    Machine(std::string machineId, std::filesystem::path const& scratch):
        id { std::move(machineId) },
        stateDirectory { scratch / id },
        key { MintKey(stateDirectory) }
    {
    }

    /// @return Its public key.
    [[nodiscard]] Ed25519PublicKey PublicKey() const
    {
        return key.PublicKey();
    }

    std::string id;                       ///< Its id.
    std::filesystem::path stateDirectory; ///< Where its key file is.
    Ed25519KeyPair key;                   ///< The key its first start minted.
};

/// The cluster's member record for @p machine.
[[nodiscard]] Cluster::ClusterMember MemberOf(Machine const& machine)
{
    return Cluster::ClusterMember { .id = machine.id,
                                    .raftEndpoint = "in-memory:1",
                                    .schedulerEndpoint = {},
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = Cluster::MemberSeat::Voter,
                                    .publicKey = machine.PublicKey() };
}

/// One node's view of the cluster: its roster over its own key, and its identity over that --
/// what `ConsensusTier` holds.
struct NodeView
{
    /// @param self The machine this view is.
    /// @param members Every member, as each node's command line names them.
    NodeView(Machine const& self, std::vector<Cluster::ClusterMember> const& members):
        roster { self.key, members },
        identity { self.id, roster }
    {
    }

    Cluster::RosterKeys roster;           ///< The keys it judges peers by.
    Consensus::RaftPeerIdentity identity; ///< Who it is on the wire.
};

/// n1's server on one in-memory listener, and the reactor everything runs on.
///
/// RAII for the teardown rather than lines at the end of the case: a failing `REQUIRE` unwinds
/// past such lines, and a server still parked in `Accept` when it is destroyed is a hang where
/// a red was wanted. Declared before every `Dialler`, so it is torn down after them.
struct Network
{
    /// @param acceptor Who n1 is.
    explicit Network(Consensus::IRaftPeerIdentity const& acceptor):
        server { listener, reactor,  sink,   logger,
                 metrics,  acceptor, random, Consensus::PeerServerOptions { .handshakeBound = 0ms } }
    {
        [](Consensus::RaftPeerServer* accepting) -> DetachedTask {
            co_await accepting->Run();
        }(&server);
    }

    Network(Network const&) = delete;
    Network(Network&&) = delete;
    Network& operator=(Network const&) = delete;
    Network& operator=(Network&&) = delete;

    ~Network()
    {
        listener.Close();
        reactor.Drain();
    }

    /// @param refusal An acceptor refusal.
    /// @return How many times n1's server counted it.
    [[nodiscard]] std::uint64_t Refused(Consensus::AcceptorRefusal refusal) const
    {
        return metrics.Read(Consensus::RowFor(refusal).counter);
    }

    ManualClock clock;
    TestReactor reactor { clock };
    InMemoryListener listener;
    Testing::ListenerConnector connector { listener };
    RecordingSink sink;
    NullLogger logger;
    AtomicMetricsSink metrics;
    SystemSecureRandom random;
    Consensus::RaftPeerServer server;
};

/// A dialler's transport to n1's server, with its own counters: started on construction, and
/// stopped -- drained on this thread -- on destruction, for `Network`'s reason.
struct Dialler
{
    /// @param view Who dials.
    /// @param network Where it dials.
    Dialler(NodeView const& view, Network& network):
        net { network },
        transport { std::vector { Consensus::PeerEndpoint { .id = "n1", .host = "in-memory", .port = 1 } },
                    network.reactor,
                    network.connector,
                    logger,
                    metrics,
                    view.identity,
                    random,
                    Consensus::PeerTransportOptions { .reconnectBackoff = ReconnectBackoff, .handshakeBound = 0ms } }
    {
        transport.Start();
        net.reactor.Drain();
    }

    Dialler(Dialler const&) = delete;
    Dialler(Dialler&&) = delete;
    Dialler& operator=(Dialler const&) = delete;
    Dialler& operator=(Dialler&&) = delete;

    /// The waiting `Stop()` in the transport's destructor must not wait on senders only this
    /// thread can advance, so they are asked to stop and drained here first.
    ~Dialler()
    {
        transport.RequestStop();
        net.reactor.Drain();
        net.clock.Advance(50ms);
        net.reactor.Drain();
    }

    /// Send one vote to n1, as @p voter, and let it arrive.
    /// @param term The vote's term, so two deliveries are distinguishable.
    /// @param voter Who the vote names as its sender.
    void Vote(std::uint64_t term, std::string const& voter)
    {
        transport.Send(
            "n1",
            Consensus::RaftMessage { Consensus::RequestVoteResponse { .term = Consensus::Term { .value = term },
                                                                      .decision = Consensus::VoteDecision::Granted,
                                                                      .voterId = voter } });
        net.reactor.Drain();
    }

    /// @param refusal A dialler refusal.
    /// @return How many times this transport counted it.
    [[nodiscard]] std::uint64_t Refused(Consensus::DiallerRefusal refusal) const
    {
        return metrics.Read(Consensus::RowFor(refusal).counter);
    }

    Network& net;                           ///< Where it dials.
    NullLogger logger;                      ///< Where it reports.
    AtomicMetricsSink metrics;              ///< What it counted.
    SystemSecureRandom random;              ///< Its handshakes' randomness.
    Consensus::RaftPeerTransport transport; ///< The transport under test.
};

} // namespace

TEST_CASE("After RevokeKey(n3), n3's session closes and its redial is refused, and nothing else changes",
          "[node][consensus][handshake][revocation]")
{
    ScratchDirectory const scratch { "raft-key-revocation" };
    Machine const n1 { "n1", scratch.Path() };
    Machine const n2 { "n2", scratch.Path() };
    Machine const n3 { "n3", scratch.Path() };

    auto const n1File = n1.stateDirectory / NodeKeyFileName;
    auto const n2File = n2.stateDirectory / NodeKeyFileName;
    auto const n1Before = BytesOf(n1File);
    auto const n2Before = BytesOf(n2File);
    REQUIRE(n1Before.size() == NodeKeyFileBytes);
    REQUIRE(n2Before.size() == NodeKeyFileBytes);

    // Every node's command line names every member with its key, and the cluster's state
    // records the same -- the formed cluster this case starts from.
    auto const members = std::vector { MemberOf(n1), MemberOf(n2), MemberOf(n3) };
    Cluster::ClusterState state;
    state.members = members;
    NodeView v1 { n1, members };
    NodeView v2 { n2, members };
    NodeView v3 { n3, members };
    for (auto* const view: { &v1, &v2, &v3 })
        view->roster.Adopt(state);

    Network network { v1.identity };

    Dialler fromN3 { v3, network };
    fromN3.Vote(1, "n3");
    REQUIRE(network.sink.received.size() == 1);
    REQUIRE(fromN3.transport.ConnectedPeers() == 1);

    // The operator revokes n3's key. The entry commits and every member applies it, n3 included.
    Cluster::Apply(state,
                   Cluster::Command { .kind = Cluster::CommandKind::RevokeKey,
                                      .key = "n3",
                                      .value = {},
                                      .schedulerEndpoint = {},
                                      .publicKey = n3.PublicKey(),
                                      .role = std::nullopt });
    REQUIRE(state.IsRevoked(n3.PublicKey()));
    for (auto* const view: { &v1, &v2, &v3 })
        view->roster.Adopt(state);

    // n3's session closes at its next frame, which is not delivered.
    fromN3.Vote(2, "n3");
    CHECK(network.sink.received.size() == 1);
    CHECK(network.Refused(Consensus::AcceptorRefusal::KeyWithdrawn) == 1);

    // And its redial is refused, signed, so n3 reports its own removal. Over TCP n3 learns of the
    // close from its next write, which the reset fails; the in-memory socket accepts writes
    // nobody reads, so the case makes the redial happen the way a moved address would.
    REQUIRE(fromN3.transport.Learn(Consensus::PeerEndpoint { .id = "n1", .host = "in-memory", .port = 2 })
            == Consensus::PeerChange::Readdressed);
    fromN3.Vote(3, "n3");
    network.clock.Advance(ReconnectBackoff);
    network.reactor.Drain();
    CHECK(network.sink.received.size() == 1);
    CHECK(fromN3.transport.ConnectedPeers() == 0);
    CHECK(network.Refused(Consensus::AcceptorRefusal::RevokedKey) == 1);
    CHECK(fromN3.Refused(Consensus::DiallerRefusal::OwnKeyRevoked) == 1);

    // Nothing else changed. n2 dials n1 with the key it always had and is heard...
    Dialler fromN2 { v2, network };
    fromN2.Vote(4, "n2");
    REQUIRE(network.sink.received.size() == 2);
    CHECK(fromN2.transport.ConnectedPeers() == 1);

    // ...and n1's and n2's key files are the bytes they were before the revocation, and a start
    // of either reads its key back rather than minting one: nothing was rotated anywhere.
    CHECK(BytesOf(n1File) == n1Before);
    CHECK(BytesOf(n2File) == n2Before);
    for (auto const* const machine: { &n1, &n2 })
    {
        CAPTURE(machine->id);
        SystemSecureRandom random;
        auto const restarted = ResolveNodeKey(machine->stateDirectory, random);
        REQUIRE(restarted.has_value());
        CHECK(restarted->origin == NodeKeyOrigin::Recorded);
        CHECK(restarted->pair.PublicKey() == machine->PublicKey());
    }
}

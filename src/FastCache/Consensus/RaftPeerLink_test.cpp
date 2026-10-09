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
//
// And a learner's link, which runs both ways on one connection: the learner's transport dials
// TWO-WAY, and the ACCEPTOR writes back on it.
#include <FastCache/Consensus/RaftPeerRefusals.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftSessionReader.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <core/async/AsyncQueue.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/CountingConnector.hpp>
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
        server { listener,      reactor,        sink,         inbound,     logger,
                 serverMetrics, serverIdentity, serverRandom, acceptLoops, PeerServerOptions { .handshakeBound = 0ms } }
    {
        [](RaftPeerServer* accepting) -> core::async::DetachedTask {
            co_await accepting->Run();
        }(&server);

        transport = std::make_unique<RaftPeerTransport>(
            std::vector { PeerEndpoint { .id = NodeId { shape.target }, .host = "in-memory", .port = 1 } },
            reactor,
            connector,
            returned,
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
    RecordingSink returned;           ///< What the one-way transport read back: never anything.
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
    Testing::NoInboundLinks inbound; ///< What the server attached; every dialler here is one-way.
    core::net::AcceptLoopHealth acceptLoops;
    RaftPeerServer server;
    std::unique_ptr<RaftPeerTransport> transport;
};

/// Which threads destroyed the sockets a `ProbingListener` handed out, in order.
class DestructionLog
{
  public:
    /// Record that a socket was destroyed on the calling thread.
    void Record()
    {
        auto const guard = std::scoped_lock { _mutex };
        _threads.push_back(std::this_thread::get_id());
    }

    /// @return Every destruction's thread, in order.
    [[nodiscard]] std::vector<std::thread::id> Threads() const
    {
        auto const guard = std::scoped_lock { _mutex };
        return _threads;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::thread::id> _threads;
};

/// An accepted socket that says which thread destroyed it, and is otherwise the socket it owns.
class ProbedSocket final: public core::net::testing::SocketDecorator
{
  public:
    /// @param owned The accepted socket; owned from here on.
    /// @param log Where the destruction is recorded; must outlive this.
    ProbedSocket(std::unique_ptr<core::net::ISocket> owned, DestructionLog& log):
        SocketDecorator { *owned },
        _owned { std::move(owned) },
        _log { log }
    {
    }

    ProbedSocket(ProbedSocket const&) = delete;
    ProbedSocket(ProbedSocket&&) = delete;
    ProbedSocket& operator=(ProbedSocket const&) = delete;
    ProbedSocket& operator=(ProbedSocket&&) = delete;

    ~ProbedSocket() override
    {
        _log.Record();
    }

  private:
    std::unique_ptr<core::net::ISocket> _owned;
    DestructionLog& _log;
};

/// A listener whose every accepted socket is a `ProbedSocket` over the one `inner` accepted, and
/// which counts them.
class ProbingListener final: public core::net::IListener
{
  public:
    /// @param inner Where connections really arrive; must outlive this.
    /// @param log Where each accepted socket's destruction is recorded; must outlive them.
    ProbingListener(core::net::testing::InMemoryListener& inner, DestructionLog& log):
        _inner { inner },
        _log { log }
    {
    }

    /// @copydoc IListener::accept
    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override
    {
        auto accepted = co_await _inner.accept();
        if (!accepted.has_value())
            co_return accepted;
        _accepted.fetch_add(1, std::memory_order_relaxed);
        co_return std::unique_ptr<core::net::ISocket> { std::make_unique<ProbedSocket>(*std::move(accepted), _log) };
    }

    /// @copydoc IListener::boundPort
    [[nodiscard]] std::uint16_t boundPort() const noexcept override
    {
        return _inner.boundPort();
    }

    /// @return How many connections were accepted.
    [[nodiscard]] std::size_t Accepted() const noexcept
    {
        return _accepted.load(std::memory_order_relaxed);
    }

  protected:
    /// @copydoc IListener::doClose
    void doClose() noexcept override
    {
        _inner.close();
    }

  private:
    core::net::testing::InMemoryListener& _inner;
    DestructionLog& _log;
    std::atomic<std::size_t> _accepted { 0 };
};

/// Counts what a learner is delivered, for a case that floods it.
class CountingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        std::ignore = message;
        ++delivered;
    }

    std::size_t delivered { 0 }; ///< How many messages arrived.
};

class SeveringConnector;

/// A client socket a `SeveringConnector` handed out, and can close from outside its transport.
class SeverableSocket final: public core::net::testing::SocketDecorator
{
  public:
    /// @param owned The client end; owned from here on.
    /// @param owner The connector that can sever it; must outlive this.
    SeverableSocket(std::unique_ptr<core::net::ISocket> owned, SeveringConnector& owner);

    SeverableSocket(SeverableSocket const&) = delete;
    SeverableSocket(SeverableSocket&&) = delete;
    SeverableSocket& operator=(SeverableSocket const&) = delete;
    SeverableSocket& operator=(SeverableSocket&&) = delete;

    /// Leaves the connector's list, so a sever never reaches a socket its transport destroyed.
    ~SeverableSocket() override;

  private:
    std::unique_ptr<core::net::ISocket> _owned;
    SeveringConnector& _owner;
};

/// Dials the leader's listener, and can drop every connection it made at once.
///
/// What a learner's network going away looks like from its transport: the connection closes under
/// it, synchronously, on this thread -- which the transport's own `RequestStop` cannot stand in
/// for, because that closes on the reactor's NEXT turn, and a leader whose outbox another thread
/// keeps full over an `InMemorySocket` (whose writes complete inline) never yields that turn.
class SeveringConnector final: public core::net::IConnector
{
  public:
    /// @param listener Where every dial arrives; must outlive the connector.
    explicit SeveringConnector(core::net::testing::InMemoryListener& listener) noexcept:
        _listener { listener }
    {
    }

    /// @copydoc IConnector::Connect
    [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                     std::uint16_t port,
                                                                     core::net::DialOptions options) override
    {
        std::ignore = host;
        std::ignore = port;
        std::ignore = options;
        auto socket = std::make_unique<SeverableSocket>(_listener.connectClient(), *this);
        _live.push_back(socket.get());
        co_return std::unique_ptr<core::net::ISocket> { std::move(socket) };
    }

    /// Close every connection this connector made that is still open. Each is taken off the list
    /// BEFORE it is closed: a close completes parked operations inline, which may end the session
    /// and destroy the socket, and nothing here touches it after.
    void Sever() noexcept
    {
        while (!_live.empty())
        {
            auto* const socket = _live.back();
            _live.pop_back();
            socket->close();
        }
    }

    /// @param socket A socket being destroyed.
    void Forget(SeverableSocket const* socket) noexcept
    {
        std::erase(_live, socket);
    }

  private:
    core::net::testing::InMemoryListener& _listener;
    std::vector<SeverableSocket*> _live; ///< Open connections; reactor-thread only.
};

SeverableSocket::SeverableSocket(std::unique_ptr<core::net::ISocket> owned, SeveringConnector& owner):
    SocketDecorator { *owned },
    _owned { std::move(owned) },
    _owner { owner }
{
}

SeverableSocket::~SeverableSocket()
{
    _owner.Forget(this);
}

/// Whether a learner's transport is started as it is built. Private: never transmitted or persisted.
enum class LearnerStart : std::uint8_t
{
    Idle,     ///< Built only; the case's first send starts it.
    Dialling, ///< Started, so it dials at the next drain.
};

/// A learner: its own connector and a two-way transport over it, stopped and drained on the
/// case's thread when it goes.
///
/// The waiting `Stop()` in the transport's destructor must not wait on senders only this thread
/// can advance, so they are asked to stop and drained here first -- `~Link`'s reason, carried by
/// the object so a case that lets one go out of scope cannot forget it.
class LearnerEnd
{
  public:
    /// Builds the learner's transport over this end's own connector.
    using Builder = std::function<std::unique_ptr<RaftPeerTransport>(core::net::IConnector&)>;

    /// @param reactor The loop its senders run on, drained here.
    /// @param listener The leader's port, which its connector dials.
    /// @param build Makes its transport over the connector it is handed.
    LearnerEnd(core::net::testing::TestLoop& reactor, core::net::testing::InMemoryListener& listener, Builder const& build):
        _reactor { reactor },
        _connector { listener },
        _transport { build(_connector) }
    {
    }

    LearnerEnd(LearnerEnd const&) = delete;
    LearnerEnd(LearnerEnd&&) = delete;
    LearnerEnd& operator=(LearnerEnd const&) = delete;
    LearnerEnd& operator=(LearnerEnd&&) = delete;

    ~LearnerEnd()
    {
        Reset();
    }

    /// @return The transport.
    [[nodiscard]] RaftPeerTransport* operator->() const noexcept
    {
        return _transport.get();
    }

    /// The learner goes away: its connection drops under it at once, and its process stops.
    void GoAway()
    {
        _connector.Sever();
        _transport->RequestStop();
    }

    /// Stop the transport, drain its senders, and destroy it. Idempotent.
    void Reset()
    {
        if (_transport == nullptr)
            return;
        _transport->RequestStop();
        _reactor.drain();
        _reactor.drain();
        _transport.reset();
    }

  private:
    core::net::testing::TestLoop& _reactor;
    SeveringConnector _connector; ///< Declared before the transport, which dials through it.
    std::unique_ptr<RaftPeerTransport> _transport;
};

/// How many messages the leader queues for one learner in these cases.
///
/// Small on purpose: the cross-thread case floods 1 MiB snapshots at a learner while its session
/// ends, and the transport's default bound (256) would let one cycle hold 256 MiB. No case here
/// queues more than one message between drains, so the bound only ever caps the flood.
constexpr std::size_t LearnerQueueBound = 4;

/// @param records What a logger captured.
/// @param level The lowest level counted.
/// @return How many records were at @p level or above.
[[nodiscard]] std::size_t CountAtOrAbove(std::vector<CapturingLogger::Record> const& records, LogLevel level)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(records, [level](CapturingLogger::Record const& record) { return record.level >= level; }));
}

/// A leader (server + transport, one reactor) and a learner that dials it TWO-WAY.
///
/// The leader's transport knows NO peer and dials through a counter, so any dial it makes shows.
/// Its server's inbound links are that transport, as `ConsensusTier` wires them. The learner is a
/// real `RaftPeerTransport` in `TwoWay` mode whose one peer is the leader; it is built here and
/// started by its first `LearnerSends`, so a case that never sends has a learner that never dials.
struct LearnerLink
{
    /// @param sharedRoster What both ends believe about everybody's keys.
    explicit LearnerLink(std::shared_ptr<Testing::SharedRoster> sharedRoster = Testing::SharedRoster::Of({ "office",
                                                                                                           "laptop" })):
        roster { std::move(sharedRoster) },
        leaderIdentity { NodeId { "office" }, Testing::TestKeyPair("office"), roster },
        learnerIdentity { NodeId { "laptop" }, Testing::TestKeyPair("laptop"), roster }
    {
        leaderTransport = std::make_unique<RaftPeerTransport>(std::vector<PeerEndpoint> {},
                                                              reactor,
                                                              leaderConnector,
                                                              leaderReturned,
                                                              leaderLogger,
                                                              leaderMetrics,
                                                              leaderIdentity,
                                                              leaderRandom,
                                                              PeerTransportOptions { .reconnectBackoff = ReconnectBackoff,
                                                                                     .maxQueuedPerPeer = LearnerQueueBound,
                                                                                     .handshakeBound = 0ms });
        leaderServer = std::make_unique<RaftPeerServer>(probing,
                                                        reactor,
                                                        leaderSink,
                                                        *leaderTransport,
                                                        leaderLogger,
                                                        leaderMetrics,
                                                        leaderIdentity,
                                                        leaderRandom,
                                                        leaderAcceptLoops,
                                                        PeerServerOptions { .handshakeBound = 0ms });
        [](RaftPeerServer* accepting) -> core::async::DetachedTask {
            co_await accepting->Run();
        }(leaderServer.get());
        leaderTransport->Start();

        // Placed the way production places it: the node's `LearnMembers` reads the laptop's
        // seat's link out of the state and tells the transport it dials in.
        leaderTransport->LearnDialsIn({ NodeId { "laptop" } });

        learnerTransport->ObserveOwnKeyRevoked([this](NodeId const& acceptor) { ownKeyRevokedBy.push_back(acceptor); });
        reactor.drain();
    }

    LearnerLink(LearnerLink const&) = delete;
    LearnerLink(LearnerLink&&) = delete;
    LearnerLink& operator=(LearnerLink const&) = delete;
    LearnerLink& operator=(LearnerLink&&) = delete;

    /// The learner goes first, then the leader's transport, then its listener -- each drained on
    /// this thread, as `~Link` does.
    ~LearnerLink()
    {
        learnerTransport.Reset();
        leaderTransport->RequestStop();
        reactor.drain();
        clock.advance(50ms);
        reactor.drain();
        listener.close();
        reactor.drain();
        leaderServer.reset();
        leaderTransport.reset();
    }

    /// How a learner's transport is made: under the learner's identity, dialling the leader
    /// two-way, and not started.
    /// @param sink Where what the leader writes to it lands.
    /// @param start Whether it is started, so it dials at the next drain.
    /// @return The builder `LearnerEnd` runs over its own connector.
    [[nodiscard]] LearnerEnd::Builder LearnerBuilder(IRaftMessageSink& sink, LearnerStart start)
    {
        return [this, &sink, start](core::net::IConnector& connector) {
            auto transport = BuildLearner(connector, sink);
            if (start == LearnerStart::Dialling)
                transport->Start();
            return transport;
        };
    }

    /// @param connector How the learner dials.
    /// @param sink Where what the leader writes to it lands.
    /// @return A learner's transport, not started.
    [[nodiscard]] std::unique_ptr<RaftPeerTransport> BuildLearner(core::net::IConnector& connector, IRaftMessageSink& sink)
    {
        return std::make_unique<RaftPeerTransport>(
            std::vector { PeerEndpoint { .id = NodeId { "office" }, .host = "in-memory", .port = 1 } },
            reactor,
            connector,
            sink,
            learnerLogger,
            learnerMetrics,
            learnerIdentity,
            learnerRandom,
            PeerTransportOptions { .handshakeBound = 0ms, .direction = RaftWire::SessionDirection::TwoWay });
    }

    /// Another learner end under the same identity: the same machine, dialling again.
    /// @param sink Where what the leader writes to it lands.
    /// @return It, dialling at the next drain.
    [[nodiscard]] LearnerEnd NewLearner(IRaftMessageSink& sink)
    {
        return LearnerEnd { reactor, listener, LearnerBuilder(sink, LearnerStart::Dialling) };
    }

    /// Hand the leader's transport one AppendEntries for the learner, and let it go.
    /// @param term The message's term, so two deliveries are distinguishable.
    void LeaderSends(std::uint64_t term)
    {
        leaderTransport->Send(NodeId { "laptop" }, LeaderMessage(term));
        reactor.drain();
    }

    /// Hand the learner one AppendEntries answer for the leader, and let it go. The first call
    /// starts the learner's transport, which dials.
    /// @param term The message's term.
    void LearnerSends(std::uint64_t term)
    {
        learnerTransport->Start();
        learnerTransport->Send(NodeId { "office" }, LearnerMessage(term));
        reactor.drain();
    }

    /// @param term The term.
    /// @return What the leader sends: an AppendEntries naming the leader.
    [[nodiscard]] static RaftMessage LeaderMessage(std::uint64_t term)
    {
        return RaftMessage { AppendEntriesRequest { .term = Term { .value = term },
                                                    .leaderId = NodeId { "office" },
                                                    .prevLogIndex = LogIndex {},
                                                    .prevLogTerm = Term {},
                                                    .entries = {},
                                                    .leaderCommit = LogIndex {} } };
    }

    /// @param term The term.
    /// @return What the learner sends: an AppendEntries answer naming the learner.
    [[nodiscard]] static RaftMessage LearnerMessage(std::uint64_t term)
    {
        return RaftMessage { AppendEntriesResponse { .term = Term { .value = term },
                                                     .result = AppendResult::Accepted,
                                                     .matchIndex = LogIndex {},
                                                     .followerId = NodeId { "laptop" } } };
    }

    /// @param refusal An acceptor refusal.
    /// @return How many times the leader counted it.
    [[nodiscard]] std::uint64_t Refused(AcceptorRefusal refusal) const
    {
        return leaderMetrics.Read(RowFor(refusal).counter);
    }

    std::shared_ptr<Testing::SharedRoster> roster;
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    core::net::testing::InMemoryListener listener;   ///< The leader's Raft port.
    DestructionLog destroyed;                        ///< Which thread destroyed each accepted socket.
    ProbingListener probing { listener, destroyed }; ///< What the leader's server accepts through.
    Testing::CountingConnector leaderConnector;      ///< Connects nowhere; counts every attempt.
    RecordingSink leaderSink;                        ///< What the leader's server delivered.
    RecordingSink leaderReturned;                    ///< What the leader's one-way transport read back: nothing.
    RecordingSink learnerSink;                       ///< What the learner's transport read from the leader.
    AtomicMetricsSink leaderMetrics;
    AtomicMetricsSink learnerMetrics;
    CapturingLogger leaderLogger; ///< Everything the leader's transport and server said.
    NullLogger learnerLogger;
    Testing::TestPeerIdentity const leaderIdentity;
    Testing::TestPeerIdentity const learnerIdentity;
    SystemSecureRandom leaderRandom;
    SystemSecureRandom learnerRandom;
    core::net::AcceptLoopHealth leaderAcceptLoops; ///< Where the leader's server reports its accept loop.

    /// Every acceptor whose SIGNED verdict told the learner its own key was revoked, in order.
    std::vector<NodeId> ownKeyRevokedBy;

    std::unique_ptr<RaftPeerTransport> leaderTransport; ///< Peers {}; declared before the server that borrows it.
    std::unique_ptr<RaftPeerServer> leaderServer;       ///< Its inbound links are `*leaderTransport`.

    /// The learner's end: built with everything above, and started by `LearnerSends`.
    LearnerEnd learnerTransport { reactor, listener, LearnerBuilder(learnerSink, LearnerStart::Idle) };
};

/// One server ("n2") and any number of real transports dialling it, each under the identity a case
/// names -- so the same member can dial twice while its first session is still open, which is what
/// a voter whose address moved looks like from the acceptor: nothing reads an EOF from a path that
/// vanished, so the old session is still being served when the new one proves the same id.
///
/// The server accepts through a `ProbingListener`, so a case can see which accepted sockets the
/// server ended and destroyed.
struct RedialLink
{
    RedialLink()
    {
        [](RaftPeerServer* accepting) -> core::async::DetachedTask {
            co_await accepting->Run();
        }(&server);
        reactor.drain();
    }

    RedialLink(RedialLink const&) = delete;
    RedialLink(RedialLink&&) = delete;
    RedialLink& operator=(RedialLink const&) = delete;
    RedialLink& operator=(RedialLink&&) = delete;

    /// Every transport stops first, then the listener closes -- each drained on this thread, as
    /// `~Link` does.
    ~RedialLink()
    {
        for (auto const& transport: transports)
            transport->RequestStop();
        reactor.drain();
        clock.advance(50ms);
        reactor.drain();
        transports.clear();
        listener.close();
        reactor.drain();
    }

    /// Start a transport proving @p member, dialling the server, and let it send one vote.
    /// @param member The id the transport proves.
    /// @param term The vote's term, so deliveries are distinguishable.
    /// @param direction Which way the session it dials flows.
    void Dial(std::string const& member,
              std::uint64_t term,
              RaftWire::SessionDirection direction = RaftWire::SessionDirection::OneWay)
    {
        auto const& identity = identities.emplace_back(
            std::make_unique<Testing::TestPeerIdentity>(NodeId { member }, Testing::TestKeyPair(member), roster));
        auto const& transport = transports.emplace_back(std::make_unique<RaftPeerTransport>(
            std::vector { PeerEndpoint { .id = NodeId { "n2" }, .host = "in-memory", .port = 1 } },
            reactor,
            connector,
            returned,
            logger,
            diallerMetrics,
            *identity,
            diallerRandom,
            PeerTransportOptions { .reconnectBackoff = ReconnectBackoff, .handshakeBound = 0ms, .direction = direction }));
        transport->Start();
        transport->Send(NodeId { "n2" },
                        RaftMessage { RequestVoteResponse { .term = Term { .value = term },
                                                            .decision = VoteDecision::Granted,
                                                            .voterId = NodeId { member } } });
        reactor.drain();
    }

    /// @return Every acceptor refusal the server counted, summed.
    [[nodiscard]] std::uint64_t AcceptorRefusalsCounted() const
    {
        auto total = std::uint64_t { 0 };
        for (auto const& row: AcceptorRefusals)
            total += serverMetrics.Read(row.counter);
        return total;
    }

    std::shared_ptr<Testing::SharedRoster> roster { Testing::SharedRoster::Of({ "n1", "n2", "n3" }) };
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    core::net::testing::InMemoryListener listener;   ///< The server's Raft port.
    DestructionLog destroyed;                        ///< Which thread destroyed each accepted socket.
    ProbingListener probing { listener, destroyed }; ///< What the server accepts through.
    Testing::ListenerConnector connector { listener };
    RecordingSink sink;     ///< What the server delivered.
    RecordingSink returned; ///< What the transports read back.
    AtomicMetricsSink serverMetrics;
    AtomicMetricsSink diallerMetrics;
    CapturingLogger serverLogger; ///< Everything the server said.
    NullLogger logger;
    Testing::TestPeerIdentity const serverIdentity { NodeId { "n2" }, Testing::TestKeyPair("n2"), roster };
    SystemSecureRandom serverRandom;
    SystemSecureRandom diallerRandom;
    Testing::NoInboundLinks inbound; ///< What the server attached.
    core::net::AcceptLoopHealth acceptLoops;
    RaftPeerServer server {
        probing,       reactor,        sink,         inbound,     serverLogger,
        serverMetrics, serverIdentity, serverRandom, acceptLoops, PeerServerOptions { .handshakeBound = 0ms }
    };

    std::vector<std::unique_ptr<Testing::TestPeerIdentity>> identities; ///< One per `Dial`, outliving its transport.
    std::vector<std::unique_ptr<RaftPeerTransport>> transports;         ///< One per `Dial`, in order.
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

// A learner nobody can dial: it dials the leader TWO-WAY, the leader's server attaches that
// session to the leader's transport, and the transport writes to the learner there -- and never
// dials it, whatever it has to send.

TEST_CASE("A leader sends to a learner over the session the learner dialled and never dials it",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1); // the learner's first frame opens the session
    REQUIRE(link.leaderSink.received.size() == 1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    link.LeaderSends(7);
    REQUIRE(link.learnerSink.received.size() == 1);
    CHECK(std::get<AppendEntriesRequest>(link.learnerSink.received[0]).term.value == 7);
    CHECK(link.leaderConnector.Attempts() == 0); // the leader never dialled anybody
    CHECK(link.leaderTransport->DroppedNoSession() == 0);

    // The control for the redial case: one session is one session, and supersedes nothing.
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 0);
}

TEST_CASE("A leader with no session to a learner drops its message counted and dials nobody",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link; // the learner has sent nothing, so nothing is attached
    link.LeaderSends(7);
    CHECK(link.learnerSink.received.empty());
    CHECK(link.leaderTransport->DroppedNoSession() == 1);
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession) == 1);
    CHECK(link.leaderConnector.Attempts() == 0);

    // A peer that dials in and has no session yet is PLACED, so it is not the unknown-peer row.
    CHECK(link.leaderTransport->DroppedUnknownPeer() == 0);
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftSendsDroppedUnknownPeer) == 0);
}

TEST_CASE("A message for a peer the transport cannot place is dropped on a row of its own and dials nobody",
          "[consensus][raft][learner][formation]")
{
    // Neither dialled nor dialling in: a member recorded with no address in a seat that is
    // dialled, or an id nothing told this node about. Not the no-session row, which says a
    // learner is offline -- a confident wrong signal about a machine that may not exist.
    LearnerLink link;
    link.leaderTransport->Send(NodeId { "desk" }, LearnerLink::LeaderMessage(7));
    link.reactor.drain();

    CHECK(link.leaderTransport->DroppedUnknownPeer() == 1);
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftSendsDroppedUnknownPeer) == 1);
    CHECK(link.leaderTransport->DroppedNoSession() == 0);
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession) == 0);
    CHECK(link.leaderTransport->DroppedMessages() == 1);
    CHECK(link.leaderConnector.Attempts() == 0);
}

TEST_CASE("A peer the transport is no longer told dials in is no longer placed", "[consensus][raft][learner][formation]")
{
    // Each call REPLACES what the last one said, because the caller hands over the whole
    // state on every pass: a learner promoted to voter, or forgotten, stops dialling in.
    LearnerLink link;
    link.leaderTransport->LearnDialsIn({});
    link.LeaderSends(7);

    CHECK(link.leaderTransport->DroppedUnknownPeer() == 1);
    CHECK(link.leaderTransport->DroppedNoSession() == 0);
}

TEST_CASE("A one-way session is never written on by the acceptor", "[consensus][raft][learner][formation]")
{
    // The control: the classic voter link. The acceptor's transport must not attach it.
    Link link { LinkShape {} };
    link.SendVote(1);
    REQUIRE(link.sink.received.size() == 1);
    // Link's server gets NoInboundLinks, which counts attaches; the one-way session made none.
    CHECK(link.inbound.Attaches() == 0);
}

TEST_CASE("A learner's session detaches when it closes and the next send is a counted drop",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);
    link.learnerTransport->RequestStop();
    link.reactor.drain();
    link.clock.advance(50ms);
    link.reactor.drain();
    CHECK(link.leaderTransport->InboundLinks() == 0);
    link.LeaderSends(8);
    CHECK(link.leaderTransport->DroppedNoSession() == 1);
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftSendsDroppedNoSession) == 1);
    CHECK(link.leaderTransport->DroppedUnknownPeer() == 0);
}

TEST_CASE("A learner that goes away while the leader is writing to it leaves no link and no sender behind",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);
    REQUIRE(link.leaderTransport->InboundSendersRunning() == 1);

    // Queued and not yet written: the learner's socket closes before the leader's sender runs,
    // so the write goes to a connection whose other end has gone.
    link.leaderTransport->Send(NodeId { "laptop" }, LearnerLink::LeaderMessage(8));
    link.learnerTransport.GoAway();
    link.reactor.drain();

    CHECK(link.learnerSink.received.empty());
    CHECK(link.leaderTransport->InboundLinks() == 0);
    CHECK(link.leaderTransport->InboundSendersRunning() == 0);

    // And the transport holds nothing a later send could reach.
    link.LeaderSends(9);
    CHECK(link.leaderTransport->DroppedNoSession() == 1);
    CHECK(link.leaderConnector.Attempts() == 0);
}

TEST_CASE("A leader's transport stopping ends its learners' sessions and leaves no link behind",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    link.leaderTransport->RequestStop();
    link.reactor.drain();

    // The stop's `CloseSockets` closed the session's socket, which ended the server's reader and
    // so detached the link; the sender ended because the stop closed its outbox.
    CHECK(link.leaderTransport->InboundSendersRunning() == 0);
    CHECK(link.leaderTransport->InboundLinks() == 0);

    // A learner dialling in after the stop is served, but not attached: no sender starts that
    // `Stop()` has already stopped waiting for.
    RecordingSink lateSink;
    auto late = link.NewLearner(lateSink);
    late->Send(NodeId { "office" }, LearnerLink::LearnerMessage(2));
    link.reactor.drain();
    CHECK(link.leaderSink.received.size() == 2);
    CHECK(link.leaderTransport->InboundLinks() == 0);
    CHECK(link.leaderTransport->InboundSendersRunning() == 0);
}

TEST_CASE("A learner that redials replaces its old session, and the old one's end leaves the new one attached",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    // The same machine dials again -- a laptop waking with a new socket before the old one's
    // close has reached the leader.
    RecordingSink secondSink;
    auto second = link.NewLearner(secondSink);
    second->Send(NodeId { "office" }, LearnerLink::LearnerMessage(2));
    link.reactor.drain();
    REQUIRE(link.leaderSink.received.size() == 2);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);
    CHECK(link.leaderTransport->InboundSendersRunning() == 1); // the replaced one's sender ended
    CHECK(link.leaderMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 1);

    // The superseded session was closed by the leader, so what the old socket carries no longer
    // arrives -- and that close, which ended the old session's reader and so its detach, did not
    // take the new session with it.
    link.LearnerSends(3);
    CHECK(link.leaderSink.received.size() == 2);
    CHECK(link.leaderTransport->InboundLinks() == 1);

    link.LeaderSends(7);
    REQUIRE(secondSink.received.size() == 1);
    CHECK(std::get<AppendEntriesRequest>(secondSink.received[0]).term.value == 7);
    CHECK(link.learnerSink.received.empty());
    CHECK(link.leaderTransport->DroppedNoSession() == 0);
}

TEST_CASE("A learner whose key is withdrawn is closed at the leader's next send, counted once as the acceptor's",
          "[consensus][raft][learner][formation][revocation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    link.roster->Revoke("laptop");
    link.LeaderSends(7);

    // The sender asks the roster before sealing, so nothing reaches the learner; it closes the
    // socket, the server's reader sees only that close, and the one count is the acceptor's row.
    CHECK(link.learnerSink.received.empty());
    CHECK(link.Refused(AcceptorRefusal::KeyWithdrawn) == 1);
    CHECK(link.leaderTransport->InboundLinks() == 0);
    CHECK(link.leaderTransport->InboundSendersRunning() == 0);
    CHECK(link.leaderConnector.Attempts() == 0);
}

TEST_CASE("A learner's socket is destroyed on the reactor even while another thread is sending to it",
          "[consensus][raft][learner][formation]")
{
    // `Send` runs on any thread -- the driver's, the reconciler's, a proposal's -- and holds what
    // it looked up across the encode. Had it held the session's entry, a session ending meanwhile
    // would leave `Send`'s copy the LAST owner of the link, and the accepted socket would be
    // destroyed on the sending thread while the reactor ran. It holds only the outbox now, so
    // every destruction is on this thread, the one that drives the reactor.
    //
    // A race, so it is run over many sessions with a thread sending throughout each ending, and
    // what it sends is a snapshot large enough that the helper spends most of its time inside
    // `Send`'s encode: with the fix no cycle can destroy the socket anywhere but here, and
    // without it a cycle does so whenever the sender lets go while that encode is running.
    constexpr auto Cycles = 256;
    constexpr auto SnapshotBytes = std::size_t { 1024 } * 1024;
    static_assert(SnapshotBytes < PeerServerOptions {}.maxFrameBytes, "the snapshot must be one the learner reads");
    LearnerLink link;
    auto const reactorThread = std::this_thread::get_id();
    auto const snapshot = RaftMessage { InstallSnapshotRequest {
        .term = Term { .value = 1 },
        .leaderId = NodeId { "office" },
        .lastIncludedIndex = LogIndex {},
        .lastIncludedTerm = Term {},
        .configuration = Configuration { .voters = { NodeId { "office" } }, .learners = { NodeId { "laptop" } } },
        .state = std::vector<std::byte>(SnapshotBytes, std::byte { 0x5A }) } };

    for (auto const cycle: std::views::iota(0, Cycles))
    {
        CountingSink sink;
        auto learner = link.NewLearner(sink);
        learner->Send(NodeId { "office" }, LearnerLink::LearnerMessage(static_cast<std::uint64_t>(cycle) + 1));
        link.reactor.drain();
        REQUIRE(link.leaderTransport->InboundLinks() == 1);

        {
            // RAII: stopped and joined on every way out of this scope, a failed REQUIRE included.
            std::atomic<std::size_t> sent { 0 };
            std::jthread const sending { [&link, &snapshot, &sent](std::stop_token const& stop) {
                while (!stop.stop_requested())
                {
                    link.leaderTransport->Send(NodeId { "laptop" }, snapshot);
                    sent.fetch_add(1, std::memory_order_relaxed);
                }
            } };

            // Ended only once the helper is sending, or the session could end before its first
            // lookup and the case would test nothing.
            REQUIRE(Testing::WaitUntil(
                "the helper thread to have sent twice",
                [&sent] { return sent.load(std::memory_order_relaxed) >= 2; },
                [&sent] { return std::format("{} sent", sent.load(std::memory_order_relaxed)); }));
            learner.GoAway();
            link.reactor.drain();
        }
        link.reactor.drain();
        learner.Reset();
    }

    auto const threads = link.destroyed.Threads();
    CHECK(threads.size() == static_cast<std::size_t>(Cycles));
    CHECK(std::ranges::count(threads, reactorThread) == static_cast<std::ptrdiff_t>(threads.size()));
}

// The learner's half of the same link, now a real transport dialling two-way: it reads what the
// leader writes on the connection it dialled, and hears -- signed -- when its own key is revoked.

TEST_CASE("A learner's session reads what the leader sends and replies on the same connection",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    link.LearnerSends(3);
    REQUIRE(link.leaderSink.received.size() == 2);
    REQUIRE(link.learnerSink.received.size() == 1);
    CHECK(link.learnerTransport->ConnectedPeers() == 1);
    CHECK(link.probing.Accepted() == 1); // one connection carried both directions
}

TEST_CASE("A revoked learner key closes the two-way session in both directions at the next frame",
          "[consensus][raft][learner][formation]")
{
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    REQUIRE(link.learnerSink.received.size() == 1);

    link.roster->Revoke("laptop");
    link.LeaderSends(3); // the leader's sender re-asks StillProves before sealing, and ends the session
    CHECK(link.learnerSink.received.size() == 1);
    CHECK(link.leaderMetrics.Read(RowFor(AcceptorRefusal::KeyWithdrawn).counter) == 1);
    CHECK(link.leaderTransport->InboundLinks() == 0);

    // The redial is refused SIGNED, and the learner hears that its own key was revoked.
    link.clock.advance(std::chrono::seconds { 1 });
    link.reactor.drain();
    CHECK(link.learnerMetrics.Read(RowFor(DiallerRefusal::OwnKeyRevoked).counter) == 1);
    CHECK(link.ownKeyRevokedBy == std::vector<NodeId> { "office" });
}

TEST_CASE("A leader whose key is withdrawn is closed by the learner at the leader's next frame, on the dialler's row",
          "[consensus][raft][learner][formation][revocation]")
{
    // The other direction of the case above: the key withdrawn is the ACCEPTOR's, so the end that
    // notices is the learner's reader -- the loop the acceptor runs, pulling `StillProves` on
    // every frame -- and the count is the dialler's own row.
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    REQUIRE(link.learnerSink.received.size() == 1);
    REQUIRE(link.learnerTransport->ConnectedPeers() == 1);

    link.roster->Revoke("office");
    link.LeaderSends(3);
    CHECK(link.learnerSink.received.size() == 1);
    CHECK(link.learnerMetrics.Read(RowFor(DiallerRefusal::KeyWithdrawn).counter) == 1);
    CHECK(link.learnerTransport->ConnectedPeers() == 0);
    CHECK(link.leaderTransport->InboundLinks() == 0);

    // Counted once, by the end that noticed: the leader saw only a closed connection.
    CHECK(link.leaderMetrics.Read(RowFor(AcceptorRefusal::KeyWithdrawn).counter) == 0);
}

TEST_CASE("An idle learner session whose key was withdrawn is closed by a recheck, and the learner hears it signed",
          "[consensus][raft][learner][formation][revocation]")
{
    // A forgotten learner is sent NOTHING once the configuration drops it, so the per-frame
    // question has no frame to ride on and its session idles on; the learner, which never
    // campaigns, would never dial again to hear the verdict. `RecheckProofs` asks without a
    // frame. The tier asks it on every reconcile pass (`ConsensusTier_test`, "A forgotten learner
    // hears its key revoked...").
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    REQUIRE(link.learnerSink.received.size() == 1);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    link.roster->Revoke("laptop");
    link.reactor.drain();
    REQUIRE(link.leaderTransport->InboundLinks() == 1); // nothing was sent, so nothing asked

    link.leaderTransport->RecheckProofs();
    link.reactor.drain();
    CHECK(link.Refused(AcceptorRefusal::KeyWithdrawn) == 1);
    CHECK(link.leaderTransport->InboundLinks() == 0);
    CHECK(link.learnerSink.received.size() == 1);
    CHECK(link.leaderTransport->DroppedMessages() == 0); // a wake is not a message

    link.clock.advance(DialBackoffOf(RaftWire::SessionDirection::TwoWay).initial);
    link.reactor.drain();
    CHECK(link.learnerMetrics.Read(RowFor(DiallerRefusal::OwnKeyRevoked).counter) == 1);
    CHECK(link.ownKeyRevokedBy == std::vector<NodeId> { "office" });
}

TEST_CASE("A recheck's wake into a full outbox counts the message it displaces",
          "[consensus][raft][learner][formation][revocation]")
{
    // The outbox is bounded and gives up its OLDEST entry to a push past the bound: `Send` and a
    // reader's end count what they displace, and a recheck's wake displaces a real message the same way.
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    link.roster->Revoke("laptop");
    for (auto const term:
         std::views::iota(std::uint64_t { 3 }, std::uint64_t { 3 } + static_cast<std::uint64_t>(LearnerQueueBound)))
        link.leaderTransport->Send(NodeId { "laptop" }, LearnerLink::LeaderMessage(term)); // fills it; nothing drains
    REQUIRE(link.leaderTransport->DroppedMessages() == 0);

    link.leaderTransport->RecheckProofs();
    CHECK(link.leaderTransport->DroppedMessages() == 1);
    link.reactor.drain();
}

TEST_CASE("A recheck leaves an idle learner session whose key still proves attached and uncounted",
          "[consensus][raft][learner][formation][revocation]")
{
    // The other half of the recheck: it asks, and a session that still proves is not ended by being
    // asked. Several passes, as the tier runs one a second for as long as the session idles. Two
    // layers keep it: the recheck wakes only a session whose key no longer proves, and a sender that
    // pops a wake for one that proves again steps over it (the next case). Either alone keeps this
    // green, so it is red only under a neuter of both.
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    REQUIRE(link.leaderTransport->InboundLinks() == 1);

    for ([[maybe_unused]] auto const pass: std::views::iota(0, 3))
    {
        link.leaderTransport->RecheckProofs();
        link.reactor.drain();
    }

    CHECK(link.leaderTransport->InboundLinks() == 1);
    CHECK(link.learnerTransport->ConnectedPeers() == 1);
    CHECK(link.Refused(AcceptorRefusal::KeyWithdrawn) == 0);
    CHECK(link.learnerMetrics.Read(RowFor(DiallerRefusal::KeyWithdrawn).counter) == 0);
    CHECK(link.leaderTransport->DroppedMessages() == 0);
    CHECK(link.probing.Accepted() == 1);
}

TEST_CASE("A wake a sender steps over never reaches the wire: the next real frame still verifies at both ends",
          "[consensus][raft][learner][formation][revocation]")
{
    // The wake rides the leader's outbox as a zero-length entry, and every frame on the session is
    // sealed under an IMPLICIT sequence number. A wake that was sealed would spend one the learner
    // never sees, and the leader's next real frame would fail its tag there -- the session closed
    // by asking whether it may stay open. So after several woken passes, a frame each way, each one
    // read and delivered by the other end's own check, on the one connection the session began on.
    //
    // Each pass wakes the session while its key does not prove and lets it prove again before the
    // sender pops the wake: the recheck wakes no session that proves, so this is the arrangement in
    // which an attached sender steps over a wake at all. The dialled sender's step-over -- a wake an
    // earlier session's reader left behind -- is `RaftPeerTransport_test`'s "A wake left behind by
    // one two-way session does not end the next", whose acceptor checks every tag.
    LearnerLink link;
    link.LearnerSends(1);
    link.LeaderSends(2);
    REQUIRE(link.learnerSink.received.size() == 1);
    REQUIRE(link.leaderSink.received.size() == 1);

    for ([[maybe_unused]] auto const pass: std::views::iota(0, 3))
    {
        link.roster->Revoke("laptop");
        link.leaderTransport->RecheckProofs(); // queued, not yet popped: the reactor runs below
        link.roster->Admit("laptop", Testing::TestKeyPair("laptop").PublicKey());
        link.reactor.drain();
    }
    REQUIRE(link.Refused(AcceptorRefusal::KeyWithdrawn) == 0);

    link.LeaderSends(3);
    link.LearnerSends(4);

    CHECK(link.learnerSink.received.size() == 2);
    CHECK(link.leaderSink.received.size() == 2);
    CHECK(link.learnerTransport->ConnectedPeers() == 1);
    CHECK(link.leaderTransport->InboundLinks() == 1);
    CHECK(link.probing.Accepted() == 1);
}

TEST_CASE("A recheck leaves a session this node dialled alone, whatever its peer's key",
          "[consensus][raft][handshake][revocation]")
{
    // Deliberately not woken. A forgotten member learns of its forget only from the verdict on a
    // dial of ITS OWN, so closing one this node dialled teaches the acceptor nothing -- and the
    // sender redials after its backoff with or without a message, so the idle socket would become
    // a refused handshake every backoff for as long as this process runs. The session still ends
    // at its next frame ("A key revoked mid-session closes that session...", the acceptor's section).
    Link link { LinkShape { .server = "n3", .dialler = "n1", .target = "n3" } };
    link.SendVote(1);
    REQUIRE(link.Connected() == 1);

    link.roster->Revoke("n3");
    link.transport->RecheckProofs();
    link.reactor.drain();
    for ([[maybe_unused]] auto const backoff: std::views::iota(0, 3))
    {
        link.clock.advance(ReconnectBackoff);
        link.reactor.drain();
    }

    CHECK(link.Connected() == 1);
    CHECK(link.AnyRefusals() == 0);
    CHECK(link.sink.received.size() == 1);
}

TEST_CASE("A two-way dialler backs off from one second doubling to thirty", "[consensus][raft][learner][formation]")
{
    auto delay = DialBackoffOf(RaftWire::SessionDirection::TwoWay).initial;
    auto const expected = std::array { 1000ms, 2000ms, 4000ms, 8000ms, 16000ms, 30000ms, 30000ms };
    for (auto const want: expected)
    {
        CHECK(delay == want);
        delay = NextBackoff(RaftWire::SessionDirection::TwoWay, delay);
    }
    CHECK(NextBackoff(RaftWire::SessionDirection::OneWay, 250ms) == 250ms);
}

TEST_CASE("An offline learner's leader logs nothing per retransmission and dials nobody",
          "[consensus][raft][learner][formation]")
{
    // What an offline learner costs the leader is nothing: no dial, since nobody can reach a
    // learner, and no Info line per retransmission, since a learner may be away for hours and
    // Raft retransmits every heartbeat.
    LearnerLink link; // the learner never dials
    for ([[maybe_unused]] auto const round: std::views::iota(0, 40))
    {
        link.LeaderSends(1);
        link.clock.advance(std::chrono::milliseconds { 250 });
        link.reactor.drain();
    }
    CHECK(link.leaderConnector.Attempts() == 0);
    CHECK(CountAtOrAbove(link.leaderLogger.Snapshot(), LogLevel::Info) == 0);

    // The control that makes the zero mean something: the same logger does record what the leader
    // says at Info, once a learner has actually dialled in.
    link.LearnerSends(1);
    CHECK(CountAtOrAbove(link.leaderLogger.Snapshot(), LogLevel::Info) > 0);
}

TEST_CASE("A second one-way session from the same member supersedes the first", "[consensus][raft][peerserver]")
{
    // A voter whose address moved closes its outbound sessions and redials; the acceptor never
    // reads an EOF from the path that vanished, and arms no idle bound on a one-way session, so
    // without this the old session would hold one of the listener's slots forever -- one more per
    // move.
    RedialLink link;
    link.Dial("n1", 1);
    REQUIRE(link.sink.received.size() == 1);
    REQUIRE(link.server.ActiveConnections() == 1);
    REQUIRE(link.destroyed.Threads().empty());

    link.Dial("n1", 2);
    REQUIRE(link.sink.received.size() == 2);
    CHECK(std::get<RequestVoteResponse>(link.sink.received[1]).term.value == 2);

    // The first session was ended by the server -- its accepted socket closed and destroyed -- and
    // only the newest one holds a slot.
    CHECK(link.probing.Accepted() == 2);
    CHECK(link.destroyed.Threads().size() == 1);
    CHECK(link.server.ActiveConnections() == 1);
    CHECK(link.serverMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 1);
    CHECK(link.AcceptorRefusalsCounted() == 0);
    auto const records = link.serverLogger.Snapshot();
    CHECK(std::ranges::any_of(records, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Info && record.message.contains("n1") && record.message.contains("superseded");
    }));

    // And the newest session is the one still served.
    link.transports.back()->Send(NodeId { "n2" },
                                 RaftMessage { RequestVoteResponse { .term = Term { .value = 3 },
                                                                     .decision = VoteDecision::Granted,
                                                                     .voterId = NodeId { "n1" } } });
    link.reactor.drain();
    REQUIRE(link.sink.received.size() == 3);
    CHECK(std::get<RequestVoteResponse>(link.sink.received[2]).term.value == 3);

    // A third move: the first session's end came AFTER the second replaced it, and must not have
    // erased the second's entry -- or this dial would find nothing to supersede.
    link.Dial("n1", 4);
    REQUIRE(link.sink.received.size() == 4);
    CHECK(link.destroyed.Threads().size() == 2);
    CHECK(link.server.ActiveConnections() == 1);
    CHECK(link.serverMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 2);
}

TEST_CASE("One-way sessions from different members coexist", "[consensus][raft][peerserver]")
{
    RedialLink link;
    link.Dial("n1", 1);
    link.Dial("n3", 2);
    REQUIRE(link.sink.received.size() == 2);

    CHECK(link.probing.Accepted() == 2);
    CHECK(link.destroyed.Threads().empty());
    CHECK(link.server.ActiveConnections() == 2);
    CHECK(link.serverMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 0);
    CHECK(link.AcceptorRefusalsCounted() == 0);
}

TEST_CASE("A member's two-way session and its one-way session do not supersede each other", "[consensus][raft][peerserver]")
{
    // A learner promoted to voter dials one-way while its two-way session is still attached: that
    // one belongs to the transport's links (`Attach` supersedes by id there), not to this rule.
    RedialLink link;
    link.Dial("n1", 1, RaftWire::SessionDirection::TwoWay);
    link.Dial("n1", 2);
    REQUIRE(link.sink.received.size() == 2);

    CHECK(link.inbound.Attaches() == 1);
    CHECK(link.destroyed.Threads().empty());
    CHECK(link.server.ActiveConnections() == 2);
    CHECK(link.serverMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 0);
}

TEST_CASE("A member's session that ended on its own is not superseded when it redials", "[consensus][raft][peerserver]")
{
    // The ordinary path: a dialler that restarts closes its session cleanly, and the session's own
    // end must take its entry with it -- an entry left naming the freed socket would make the
    // redial's proof close freed memory.
    RedialLink link;
    link.Dial("n1", 1);
    REQUIRE(link.sink.received.size() == 1);

    link.transports.front()->RequestStop();
    link.reactor.drain();
    link.clock.advance(50ms);
    link.reactor.drain();
    REQUIRE(link.destroyed.Threads().size() == 1);
    REQUIRE(link.server.ActiveConnections() == 0);

    link.Dial("n1", 2);
    REQUIRE(link.sink.received.size() == 2);
    CHECK(std::get<RequestVoteResponse>(link.sink.received[1]).term.value == 2);
    CHECK(link.server.ActiveConnections() == 1);
    CHECK(link.destroyed.Threads().size() == 1);
    CHECK(link.serverMetrics.Read(IMetricsSink::Counter::RaftInboundSessionsSuperseded) == 0);
    CHECK(link.AcceptorRefusalsCounted() == 0);
}

// SPDX-License-Identifier: Apache-2.0
//
// The outbound transport. Every case here drives a scripted connector rather
// than a socket: the properties worth pinning are about queueing, dropping and
// shutdown, and a real network would make each of them a race.
//
// The scripted socket is an ACCEPTOR as well as a sink (#1308, #178): it challenges, judges
// the transport's proof with the real `AcceptorHandshake` under an identity of the case's
// choosing, answers with a verdict, and then checks every frame's tag under the session it
// agreed before recording it -- so "a frame reached the peer" still means what it meant, on a
// connection whose two ends proved their ids.
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

/// A roster naming every member any case here dials or is, each under its own key.
/// @return A fresh one, so a case that revokes changes nobody else's.
[[nodiscard]] std::shared_ptr<Testing::SharedRoster> Roster()
{
    return Testing::SharedRoster::Of({ "n1", "n2", "n3" });
}

/// The bytes the scripted acceptor draws on the connection with @p seed: its nonce, and --
/// the script cycling -- the same bytes again as its ephemeral secret.
///
/// Scripted rather than drawn, so the acceptor can be REBUILT from the seed when the proof
/// arrives and hold the nonce and the ephemeral secret its challenge carried. Distinct for
/// every seed below 256, which is more connections than any case here opens.
/// @param seed The connection's seed.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> ConnectionNonceScript(std::uint64_t seed)
{
    return Testing::ScriptedSecureRandom::Ascending(NonceBytes, static_cast<std::uint8_t>(seed & 0xFFU));
}

/// How the scripted acceptor behaves before the session starts.
enum class AcceptorScript : std::uint8_t
{
    Answers,         ///< Challenges, judges the proof honestly, and answers with the verdict.
    NeverChallenges, ///< Sends nothing and reads nothing: a build from before the handshake.
    SendsRaftFirst,  ///< Opens with a Raft message, as a build from before the handshake would send.
};

/// A socket that is the acceptor's end of a connection: it answers the handshake,
/// then records what was written and can be told to fail.
class RecordingSocket final: public ISocket
{
  public:
    /// Shared state, so a test can read it after the transport has closed and
    /// released its socket.
    struct Record
    {
        std::mutex mutex;
        std::condition_variable wake;

        /// Every session frame that reached this end with a tag that verified, the tag
        /// removed. Handshake frames are not writes in the sense these cases count.
        std::vector<std::vector<std::byte>> writes;

        /// Session frames whose tag did NOT verify. Zero in every case, and asserted,
        /// so a transport sealing wrongly cannot pass by having its frames recorded
        /// regardless.
        std::size_t unverifiedWrites { 0 };

        /// Fail session writes. The handshake is unaffected, so a case that fails one
        /// write still means "a peer that accepted and then reset".
        bool failWrites { false };

        /// Park session writes instead of answering them, so a test can be inside the
        /// window where a peer has accepted and stopped reading.
        ///
        /// A fake that resolves every write inline cannot show the failure this
        /// whole migration exists to remove: the transport arms no I/O timeout, so
        /// over a blocking socket such a write was uninterruptible and `Stop()`
        /// joined a thread that was not waiting on anything. Only a parked write
        /// can demonstrate that closing the socket is what completes it.
        bool parkWrites { false };

        /// What one connection is given, taken in one step under the lock.
        struct Connection
        {
            std::string machine;                                 ///< Whose key its acceptor signs with.
            std::shared_ptr<Testing::SharedRoster const> roster; ///< What its acceptor believes.
            AcceptorScript script { AcceptorScript::Answers };   ///< How its acceptor opens.
            std::uint64_t seed { 0 };                            ///< Its nonce seed.
        };

        /// @return The next connection's acceptor, and a seed no other connection has.
        [[nodiscard]] Connection NextConnection()
        {
            std::scoped_lock const guard { mutex };
            return Connection { .machine = acceptorMachine, .roster = acceptorRoster, .script = script, .seed = nextSeed++ };
        }

        /// Whose private key the acceptor signs with. Empty means "the id it answers as",
        /// which is what every honest address does.
        std::string acceptorMachine;

        /// What the acceptor believes about everybody's keys.
        std::shared_ptr<Testing::SharedRoster> acceptorRoster { Roster() };

        /// Who the acceptor is. Unset means "whichever member was dialled", which is
        /// what every honest address answers as.
        std::optional<NodeId> acceptorId;

        /// How the acceptor opens the connection.
        AcceptorScript script { AcceptorScript::Answers };

        /// Seeds each connection's nonce, so no two connections share one.
        std::uint64_t nextSeed { 1 };
    };

    /// @param record Where to append; must outlive the socket.
    explicit RecordingSocket(std::shared_ptr<Record> const& record):
        RecordingSocket { record, record->NextConnection() }
    {
    }

    /// @param record Where to append; must outlive the socket.
    /// @param connection What this connection's acceptor is.
    RecordingSocket(std::shared_ptr<Record> record, Record::Connection connection):
        _record { std::move(record) },
        _machine { std::move(connection.machine) },
        _roster { std::move(connection.roster) },
        _script { connection.script },
        _seed { connection.seed }
    {
        switch (_script)
        {
            case AcceptorScript::Answers: {
                // The challenge draws nothing from who the acceptor is, so any identity makes it;
                // the one that JUDGES is built when the proof names whom it dialled.
                Testing::ScriptedSecureRandom random { ConnectionNonceScript(_seed) };
                Testing::TestPeerIdentity const anyone { "unused", Testing::TestKeyPair("unused"), _roster };
                auto const challenge =
                    RaftWire::EncodeChallenge(AcceptorHandshake::Create(anyone, random).value().Challenge());
                _inbound.insert(_inbound.end(), challenge.begin(), challenge.end());
                break;
            }
            case AcceptorScript::SendsRaftFirst: {
                auto const message = RaftWire::Encode(RaftMessage { RequestVoteResponse {
                    .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" } });
                _inbound.insert(_inbound.end(), message.begin(), message.end());
                break;
            }
            case AcceptorScript::NeverChallenges:
                break;
        }
    }

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override
    {
        if (_closed)
            return IoAwaitable { std::unexpected {
                NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = "socket closed" } } };

        if (!_inbound.empty())
        {
            auto const count = std::min(buffer.size(), _inbound.size());
            std::copy_n(_inbound.begin(), count, buffer.begin());
            _inbound.erase(_inbound.begin(), _inbound.begin() + static_cast<std::ptrdiff_t>(count));
            return IoAwaitable { IoResult { count } };
        }

        // An acceptor that says nothing: parked until `Close()`, which is what the
        // handshake bound does to it.
        if (_script == AcceptorScript::NeverChallenges)
        {
            IoAwaitable parked;
            parked.SetSuspendCallback(
                [](IoAwaitable* self, std::coroutine_handle<>) {
                    auto* const socket = static_cast<RecordingSocket*>(self->CallbackState());
                    socket->_parkedRead = self;
                },
                this);
            return parked;
        }

        // Nothing more is coming: the acceptor refused the proof, or has nothing to say
        // after the verdict.
        return IoAwaitable { IoResult { std::size_t { 0 } } };
    }

    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override
    {
        // A closed socket refuses, as every real one does. Recording the write
        // instead would let the transport go on feeding a connection it has
        // dropped -- and "the peer moved, so its connection was closed" is a case
        // whose whole observable consequence is the write that then fails.
        if (_closed)
            return IoAwaitable { std::unexpected {
                NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "socket closed" } } };

        if (!_opener.has_value())
            return AnswerProof(buffer);

        std::scoped_lock const guard { _record->mutex };
        if (_record->failWrites)
            return IoAwaitable { std::unexpected {
                NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "scripted write failure" } } };

        if (_record->parkWrites)
        {
            // Deferred, and stashed so `Close()` can complete it -- mirroring
            // `EpollSocket`, where closing is exactly what resolves a parked
            // operation.
            IoAwaitable parked;
            parked.SetSuspendCallback(
                [](IoAwaitable* self, std::coroutine_handle<>) {
                    auto* const socket = static_cast<RecordingSocket*>(self->CallbackState());
                    socket->_parkedWrite = self;
                },
                this);
            return parked;
        }

        // The tag is checked here, as the acceptor would, and the frame is recorded
        // without it -- so a case decoding a write reads exactly what the transport
        // framed.
        if (buffer.size() < RaftWire::HeaderSize + RaftWire::TagSize)
            ++_record->unverifiedWrites;
        else
        {
            auto const frame = buffer.first(buffer.size() - RaftWire::TagSize);
            SessionTag tag {};
            std::ranges::copy(buffer.last(RaftWire::TagSize), tag.begin());
            if (_opener->Open(frame.first(RaftWire::HeaderSize), frame.subspan(RaftWire::HeaderSize), tag))
                _record->writes.emplace_back(frame.begin(), frame.end());
            else
                ++_record->unverifiedWrites;
        }
        _record->wake.notify_all();
        return IoAwaitable { IoResult { buffer.size() } };
    }

    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        std::ignore = keepAlive;
        std::size_t total = 0;
        for (auto const& segment: segments)
            total += segment.size();
        return IoAwaitable { IoResult { total } };
    }

    void close() noexcept override
    {
        _closed = true;
        // Completing the parked operation is what `Close` MEANS on a reactor
        // socket, and it is the whole reason a stop can now reach a sender that
        // is mid-write -- or mid-handshake, parked on an acceptor that never speaks.
        //
        // BOTH are taken out before EITHER completes. Completing resumes the sender
        // inline, and the sender owns this socket, so a completion can be the last thing
        // that runs on it: reading `_parkedRead` after completing the write was a member
        // read on a destroyed object, which UBSan's vptr check caught under clang-debug
        // and GCC's build could not see. One coroutine parks one operation, so at most
        // one of the two is set.
        auto* const parkedWrite = std::exchange(_parkedWrite, nullptr);
        auto* const parkedRead = std::exchange(_parkedRead, nullptr);
        for (auto* const parked: { parkedWrite, parkedRead })
            if (parked != nullptr)
                parked->complete(std::unexpected {
                    NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "socket closed" } });
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

    [[nodiscard]] std::string peerAddress() const override
    {
        return "scripted";
    }

  private:
    /// Judge the proof the transport wrote, and queue the verdict for its next read.
    ///
    /// The real `AcceptorHandshake`, rebuilt from this connection's script so its nonce and
    /// its ephemeral secret are the ones the challenge carried. Rebuilt rather than kept
    /// because who this acceptor IS may be "whoever was dialled", which is known only once
    /// the proof names it.
    /// @param buffer The proof frame.
    /// @return The write's result.
    [[nodiscard]] IoAwaitable AnswerProof(std::span<std::byte const> buffer)
    {
        auto const header = RaftWire::DecodeHeader(buffer);
        auto const proof =
            header.has_value()
                ? RaftWire::DecodeProof(*header, buffer.subspan(RaftWire::HeaderSize))
                : std::expected<RaftWire::ProofFrame, ConsensusError> { std::unexpect, MalformedWireFrame("no header") };
        if (!proof.has_value())
            return IoAwaitable { IoResult { buffer.size() } };

        NodeId self;
        {
            std::scoped_lock const guard { _record->mutex };
            self = _record->acceptorId.value_or(proof->target);
        }

        Testing::ScriptedSecureRandom random { ConnectionNonceScript(_seed) };
        Testing::TestPeerIdentity const identity { self, Testing::TestKeyPair(_machine.empty() ? self : _machine), _roster };
        auto handshake = AcceptorHandshake::Create(identity, random).value();
        auto judgement = handshake.Judge(*proof);
        if (judgement.verdict.has_value())
        {
            auto const verdict = RaftWire::EncodeVerdict(*judgement.verdict);
            _inbound.insert(_inbound.end(), verdict.begin(), verdict.end());
        }
        if (judgement.outcome == ProofOutcome::Accepted && judgement.session.has_value())
            _opener.emplace(*std::move(judgement.session));
        return IoAwaitable { IoResult { buffer.size() } };
    }

    std::shared_ptr<Record> _record;
    std::string _machine;
    std::shared_ptr<Testing::SharedRoster const> _roster;
    AcceptorScript _script { AcceptorScript::Answers };
    std::uint64_t _seed { 0 };
    std::deque<std::byte> _inbound;
    std::optional<FrameOpener> _opener;
    IoAwaitable* _parkedWrite { nullptr };
    IoAwaitable* _parkedRead { nullptr };
    bool _closed { false };
};

/// A connector under the test's control: it can refuse, or hand out sockets
/// that record what the transport wrote.
class ScriptedConnector final: public IConnector
{
  public:
    /// @param record Shared write log handed to every socket produced.
    /// @param reactor Where a delayed dial parks; must outlive the connector.
    ScriptedConnector(std::shared_ptr<RecordingSocket::Record> record, IReactor& reactor) noexcept:
        _record { std::move(record) },
        _reactor { reactor }
    {
    }

    /// Make every subsequent dial fail.
    /// @param refuse Whether to refuse.
    void Refuse(bool refuse) noexcept
    {
        _refuse.store(refuse, std::memory_order_relaxed);
    }

    /// Park every subsequent dial for this long before it resolves.
    ///
    /// The one thing a connector that answers inline cannot produce: the window in
    /// which a dial is in flight. `PlatformConnector` really does suspend there, and
    /// a peer that moves during that window is a case with no other way in.
    /// @param delay How long to park; zero answers inline as before.
    void DelayDial(std::chrono::milliseconds delay) noexcept
    {
        _delay.store(delay.count(), std::memory_order_relaxed);
    }

    /// @return How many dials have been attempted.
    [[nodiscard]] std::size_t Attempts() const noexcept
    {
        return _attempts.load(std::memory_order_relaxed);
    }

    /// Where the last dial was aimed.
    ///
    /// Recorded because a peer that MOVED is otherwise invisible: it keeps its id,
    /// its outbox and its sender, and the only observable difference is the address
    /// the next dial names.
    /// @return `host:port`, or empty before any dial.
    [[nodiscard]] std::string LastTarget() const
    {
        std::scoped_lock const guard { _targetMutex };
        return _lastTarget;
    }

    /// @copydoc IConnector::Connect
    ///
    /// Answers inline unless `DelayDial` says otherwise, which is what keeps the
    /// cases that are not about dialling free of clock arithmetic. It *can* park,
    /// because `PlatformConnector` does: a dial in flight is a real state, and the
    /// transport has to behave when a peer moves during one.
    [[nodiscard]] Task<SocketResult> connect(std::string host, std::uint16_t port, DialOptions options) override
    {
        std::ignore = options;
        _attempts.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock const guard { _targetMutex };
            _lastTarget = std::format("{}:{}", host, port);
        }

        if (auto const delay = _delay.load(std::memory_order_relaxed); delay > 0)
            co_await SleepUntil(&_reactor, _reactor.clock().now() + std::chrono::milliseconds { delay });

        if (_refuse.load(std::memory_order_relaxed))
            co_return std::unexpected { NetError {
                .code = NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" } };
        co_return std::make_unique<RecordingSocket>(_record);
    }

  private:
    std::shared_ptr<RecordingSocket::Record> _record;
    IReactor& _reactor;
    std::atomic<bool> _refuse { false };
    std::atomic<std::int64_t> _delay { 0 };
    std::atomic<std::size_t> _attempts { 0 };
    mutable std::mutex _targetMutex;
    std::string _lastTarget;
};

/// A vote response, the smallest message that round-trips.
/// @param term Term to carry.
/// @return The message.
[[nodiscard]] RaftMessage Vote(std::uint64_t term)
{
    return RaftMessage { RequestVoteResponse {
        .term = Term { .value = term }, .decision = VoteDecision::Granted, .voterId = "n1" } };
}

/// One peer pointing anywhere; the scripted connector ignores the address.
/// @return The endpoint table.
[[nodiscard]] std::vector<PeerEndpoint> OnePeer()
{
    return { PeerEndpoint { .id = "n2", .host = "unused", .port = 1 } };
}

/// Clock, reactor and transport, wired the way production wires them.
///
/// Every wall-clock wait has left this file. The senders run on a `TestReactor`
/// driven by the test's own thread, so "let the sender make progress" is
/// `Drain()` and "let the backoff elapse" is `Advance()`. What that buys is not
/// only speed: the old file had a case whose comment described a race it had to
/// engineer around, and that race no longer exists.
struct Harness
{
    std::shared_ptr<RecordingSocket::Record> record { std::make_shared<RecordingSocket::Record>() };
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector { record, reactor };
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    SystemSecureRandom random;

    /// What the transport believes about everybody's keys. A case may replace it, or revoke
    /// from it, before `Build`; revoking after is a key withdrawn from an open session.
    std::shared_ptr<Testing::SharedRoster> roster { Roster() };

    /// Where the transport draws its nonces: `random`, unless a case points it elsewhere
    /// before `Build`.
    ISecureRandom* nonces { &random };

    /// Who the transport is: n1, under its own key, over `roster`. Built by `Build`.
    std::unique_ptr<Testing::TestPeerIdentity const> identity;

    std::unique_ptr<RaftPeerTransport> transport;

    /// Make the peer's socket refuse or accept writes.
    /// @param fail Whether writes should fail.
    void FailWrites(bool fail)
    {
        std::scoped_lock const guard { record->mutex };
        record->failWrites = fail;
    }

    /// Build the transport over one peer, without starting it.
    ///
    /// Split from `Start` because a peer learned BEFORE the start is one of the
    /// cases worth pinning, and a case that wired its own clock, reactor,
    /// connector and logger to reach that state would be a third copy of this
    /// fixture to keep in step.
    /// @param options Timeouts and queue bound.
    void Build(PeerTransportOptions options = {})
    {
        identity = Testing::TestPeerIdentity::Honest("n1", roster);
        transport =
            std::make_unique<RaftPeerTransport>(OnePeer(), reactor, connector, logger, metrics, *identity, *nonces, options);
    }

    /// Build the transport over one peer and start its sender.
    /// @param options Timeouts and queue bound.
    void Start(PeerTransportOptions options = {})
    {
        Build(options);
        transport->Start();
        reactor.Drain();
    }

    /// Ask the senders to finish and let them, without the blocking `Stop()`.
    ///
    /// Tests drive the reactor from their own thread, so they must never call the
    /// waiting form: it waits for coroutines only that thread can advance. That
    /// is what `RequestStop` exists for.
    /// @param wakeBound How far to advance the clock, so a parked backoff wakes.
    void RequestStopAndDrain(std::chrono::milliseconds wakeBound = std::chrono::milliseconds { 50 })
    {
        transport->RequestStop();
        reactor.Drain();
        clock.advance(wakeBound);
        reactor.Drain();
    }

    /// @return How many frames the peer socket has been handed, with a tag that verified.
    [[nodiscard]] std::size_t Writes() const
    {
        std::scoped_lock const guard { record->mutex };
        CHECK(record->unverifiedWrites == 0);
        return record->writes.size();
    }

    /// @param refusal A refusal.
    /// @return How many times it has been counted.
    [[nodiscard]] std::uint64_t Refused(DiallerRefusal refusal) const
    {
        return metrics.Read(RowFor(refusal).counter);
    }

    /// @return The bytes of one recorded frame.
    /// @param index Which frame.
    [[nodiscard]] std::vector<std::byte> Frame(std::size_t index) const
    {
        std::scoped_lock const guard { record->mutex };
        return record->writes.at(index);
    }
};

} // namespace

TEST_CASE("A sent message reaches the peer as a decodable frame", "[consensus][raft][transport]")
{
    Harness harness;
    harness.Start();

    harness.transport->Send("n2", Vote(7));
    // Not yet: `Push` hands the sender's handle to the reactor rather than
    // resuming it, which is what keeps it out of the driver's mutex.
    CHECK(harness.Writes() == 0);

    harness.reactor.Drain();
    REQUIRE(harness.Writes() == 1);

    // Asserted by decoding rather than by byte count: what matters is that the
    // peer can read what arrived, which a length check would not establish.
    auto const frame = harness.Frame(0);
    auto const header = RaftWire::DecodeHeader(frame);
    REQUIRE(header.has_value());
    auto const decoded = RaftWire::DecodeMessage(header.value_or(RaftWire::FrameHeader {}),
                                                 std::span<std::byte const> { frame }.subspan(RaftWire::HeaderSize));
    REQUIRE(decoded.has_value());
    auto const message = decoded.value_or(RaftMessage {});
    REQUIRE(std::holds_alternative<RequestVoteResponse>(message));
    CHECK(std::get<RequestVoteResponse>(message).term == Term { .value = 7 });

    harness.RequestStopAndDrain();
}

TEST_CASE("A dialler that cannot draw a nonce sends no proof and blames itself rather than the peer",
          "[consensus][raft][transport]")
{
    // The connection is abandoned before this node proves anything, rather than proved with a
    // weak nonce (#1527). Nothing about the PEER is wrong, so no `DiallerRefusal` moves -- a
    // count there would send an operator to a machine that is fine -- and the Error names this
    // host's generator instead.
    Harness harness;
    Testing::ScriptedSecureRandom denied { Testing::ScriptedSecureRandom::DeniedFailure() };
    harness.nonces = &denied;
    harness.Start();

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    CHECK(denied.FillCount() >= 1);
    CHECK(harness.Writes() == 0);
    for (auto const& row: DiallerRefusals)
        CHECK(harness.Refused(row.refusal) == 0);

    auto const lines = harness.logger.Snapshot();
    CHECK(std::ranges::any_of(lines, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains("cannot draw a handshake nonce")
               && record.message.contains(Testing::ScriptedSecureRandom::DeniedFailure().primitive);
    }));

    harness.RequestStopAndDrain();
}

TEST_CASE("Send hands the message over without advancing the sender", "[consensus][raft][transport]")
{
    // Sharper than "Send does not block", which a merely fast implementation
    // would also satisfy. `Send` is reached from `RaftDriver::Deliver` while it
    // holds the driver's mutex, so a queue that resumed the sender inline would
    // run the sender's next step under that lock -- and the sender calls back
    // into the transport.
    Harness harness;
    harness.Start();

    for (auto const term: std::views::iota(1, 5))
        harness.transport->Send("n2", Vote(static_cast<std::uint64_t>(term)));

    CHECK(harness.Writes() == 0);
    harness.reactor.Drain();
    CHECK(harness.Writes() == 4);

    harness.RequestStopAndDrain();
}

TEST_CASE("A message for this node itself is not sent anywhere", "[consensus][raft][transport]")
{
    // A configuration listing every member uniformly is the natural thing to
    // write, so the transport tolerates its own id rather than making each caller
    // filter -- but it must not give itself a socket and a sender.
    auto record = std::make_shared<RecordingSocket::Record>();
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector { record, reactor };
    NullLogger logger;
    AtomicMetricsSink metrics;
    auto const identity = Testing::TestPeerIdentity::Honest("n1", Roster());
    SystemSecureRandom random;

    std::vector<PeerEndpoint> peers { PeerEndpoint { .id = "n1", .host = "self", .port = 1 },
                                      PeerEndpoint { .id = "n2", .host = "unused", .port = 2 } };
    RaftPeerTransport transport { std::move(peers), reactor, connector, logger, metrics, *identity, random };
    transport.Start();
    reactor.Drain();

    transport.Send("n1", Vote(1));
    transport.Send("n2", Vote(2));
    reactor.Drain();

    {
        std::scoped_lock const guard { record->mutex };
        // Exactly one write: the peer's. The self-addressed one was refused.
        CHECK(record->writes.size() == 1);
    }

    // Drained before the transport is destroyed. Its destructor calls the
    // waiting `Stop()`, which asserts when called from the thread the senders run
    // on -- and in a test that thread is this one.
    transport.RequestStop();
    reactor.Drain();
    clock.advance(50ms);
    reactor.Drain();
    REQUIRE(transport.SendersRunning() == 0);
}

TEST_CASE("A message for an unknown peer is dropped and counted", "[consensus][raft][transport]")
{
    // Counted rather than thrown. This is reached from the driver's send loop, and
    // a throw there would take a node down over a configuration gap that costs it
    // one peer.
    Harness harness;
    harness.Start();

    harness.transport->Send("nobody", Vote(1));
    CHECK(harness.transport->DroppedMessages() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A queue for an unreachable peer is bounded", "[consensus][raft][transport]")
{
    // The property that keeps a peer being down from becoming a memory leak: an
    // hour of heartbeats for an unreachable follower must not accumulate.
    constexpr std::size_t Bound = 4;
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 10s, .reconnectBackoff = 10s, .maxQueuedPerPeer = Bound });

    for (auto const term: std::views::iota(1, 11))
        harness.transport->Send("n2", Vote(static_cast<std::uint64_t>(term)));

    // Six displaced, and reported: a drop nobody counts is invisible, and Raft
    // recovering from loss means a fleet dropping steadily looks healthy while
    // running slower than it should.
    CHECK(harness.transport->DroppedMessages() == 10 - Bound);
    CHECK(harness.Writes() == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("Stopping is prompt even while a peer is unreachable", "[consensus][raft][transport]")
{
    // The assertion is on the CLOCK, not on elapsed wall time. A version that
    // merely happened to be fast would pass a wall-clock bound; what has to hold
    // is that teardown does not wait out `reconnectBackoff`, so shutdown stays
    // independent of how unreachable a peer happens to be.
    constexpr auto Backoff = 30s;
    constexpr auto WakeBound = 50ms;

    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 10s, .reconnectBackoff = Backoff, .stopWakeBound = WakeBound });

    auto const started = harness.clock.now();
    harness.RequestStopAndDrain(WakeBound);

    CHECK(harness.transport->SendersRunning() == 0);
    // Woken within the bound, not after the backoff.
    CHECK(harness.clock.now() - started == WakeBound);
    CHECK(harness.reactor.PendingTimers() == 0);
    CHECK(harness.reactor.PendingSubmissions() == 0);
}

TEST_CASE("A refused dial is not retried faster than the backoff", "[consensus][raft][transport]")
{
    // The dial-storm guard. Without it a partitioned node opens connections as
    // fast as it can to a host that is down, which fills a conntrack table and
    // starts breaking unrelated traffic to that host.
    constexpr auto Backoff = 200ms;
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = Backoff, .stopWakeBound = 50ms });

    REQUIRE(harness.connector.Attempts() == 1);

    harness.clock.advance(Backoff / 2);
    harness.reactor.Drain();
    CHECK(harness.connector.Attempts() == 1);

    harness.clock.advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.connector.Attempts() == 2);

    harness.RequestStopAndDrain();
}

TEST_CASE("A connection that drops is also backed off", "[consensus][raft][transport]")
{
    // The regression guard for the shape the thread version had: it redialled
    // immediately after a dropped connection, so a peer that accepts and instantly
    // resets produced a tight connect/write-fail/connect loop with no sleep in it.
    // On a private thread that burned one core; on the shared reactor it starves
    // the election timers, which is the "nine role changes in twelve seconds"
    // failure this repository already has a name for.
    constexpr auto Backoff = 200ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = Backoff, .stopWakeBound = 50ms });
    REQUIRE(harness.connector.Attempts() == 1);

    // A write that fails ends the session, so the sender goes round the loop.
    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    // Still one: the redial waits for the backoff rather than happening at once.
    CHECK(harness.connector.Attempts() == 1);

    harness.clock.advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.connector.Attempts() == 2);

    harness.RequestStopAndDrain();
}

TEST_CASE("Stopping completes a write the peer never read", "[consensus][raft][transport]")
{
    // THE case. The transport arms no I/O timeout on its sockets, deliberately --
    // that decides when a peer is declared dead and is its own decision. Over a
    // blocking socket that made a write uninterruptible: a peer that accepted and
    // stopped reading parked the sender in `::send` once the buffer filled, and
    // `Stop()` cleared the outbox, notified a condition variable the sender was
    // not waiting on, and then joined it. The socket was a local of the sender, so
    // nothing else could close it. `~RaftPeerTransport` blocked forever and the
    // node died to SIGKILL.
    //
    // On the reactor the write suspends, and closing the socket completes it --
    // which is a property the thread-per-peer design could not have had.
    Harness harness;
    harness.Start();
    harness.record->parkWrites = true;

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    // The sender is inside the write, and nothing the peer does will end it.
    REQUIRE(harness.transport->SendersRunning() == 1);
    REQUIRE(harness.Writes() == 0);

    harness.RequestStopAndDrain();

    CHECK(harness.transport->SendersRunning() == 0);
    CHECK(harness.reactor.PendingSubmissions() == 0);
    CHECK(harness.reactor.PendingTimers() == 0);
}

TEST_CASE("A dropped connection is redialled", "[consensus][raft][transport]")
{
    constexpr auto Backoff = 10ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff, .stopWakeBound = 5ms });

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    REQUIRE(harness.Writes() == 1);
    auto const firstAttempts = harness.connector.Attempts();

    // Fail one write, which ends the session; then let the backoff elapse.
    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(2));
    harness.reactor.Drain();
    harness.clock.advance(Backoff);
    harness.reactor.Drain();

    CHECK(harness.connector.Attempts() > firstAttempts);

    // And the reconnected sender writes again. No race to engineer around: the
    // old version of this case had a comment describing one, and it no longer
    // exists because nothing here runs on a thread of its own.
    harness.record->failWrites = false;
    harness.transport->Send("n2", Vote(3));
    harness.reactor.Drain();
    CHECK(harness.Writes() == 2);

    harness.RequestStopAndDrain(5ms);
}

TEST_CASE("ConnectedPeers is exact across a reconnect, and zero after teardown", "[consensus][raft][transport]")
{
    // The RAII guard's property. A count kept by hand is decremented at each of
    // the session's exits, and the one that gets forgotten makes this report a
    // fleet talking to peers it is not talking to -- which a readiness probe
    // reads as healthy.
    constexpr auto Backoff = 10ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff, .stopWakeBound = 5ms });

    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.record->failWrites = false;
    harness.clock.advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.RequestStopAndDrain(5ms);
    CHECK(harness.transport->ConnectedPeers() == 0);
}

TEST_CASE("Stop is idempotent and safe before Start", "[consensus][raft][transport]")
{
    auto record = std::make_shared<RecordingSocket::Record>();
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector { record, reactor };
    NullLogger logger;
    AtomicMetricsSink metrics;
    auto const identity = Testing::TestPeerIdentity::Honest("n1", Roster());
    SystemSecureRandom random;

    RaftPeerTransport transport { OnePeer(), reactor, connector, logger, metrics, *identity, random };
    transport.Stop();
    transport.Stop();
    CHECK(transport.SendersRunning() == 0);
}

TEST_CASE("A peer learned after the start is dialled and served", "[consensus][raft][transport]")
{
    // What makes a cluster growable. The member set used to be fixed at
    // construction, so a node the cluster agreed to admit at runtime was one nobody
    // dialled -- counted towards a quorum it could never contribute to, which is a
    // cluster that stops forming one.
    Harness harness;
    harness.Start();
    REQUIRE(harness.transport->PeerCount() == 1);

    // Refused outright before it is learned, which is what the case has to rule out.
    harness.transport->Send("n3", Vote(1));
    CHECK(harness.transport->DroppedMessages() == 1);

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n3", .host = "elsewhere", .port = 7 }) == PeerChange::Added);
    CHECK(harness.transport->PeerCount() == 2);
    harness.reactor.Drain();

    harness.transport->Send("n3", Vote(2));
    harness.reactor.Drain();

    CHECK(harness.transport->DroppedMessages() == 1);
    CHECK(harness.Writes() == 1);
    CHECK(harness.connector.LastTarget() == "elsewhere:7");

    harness.RequestStopAndDrain();
}

TEST_CASE("A peer learned before the start is dialled when it starts", "[consensus][raft][transport]")
{
    // The other side of one state read under one lock. A peer added in the window
    // around `Start` must be submitted exactly once -- never, and it is silently
    // undialled; twice, and two senders share one outbox.
    Harness harness;
    harness.Build();

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n3", .host = "later", .port = 9 }) == PeerChange::Added);
    CHECK(harness.transport->SendersRunning() == 0);

    harness.transport->Start();
    harness.reactor.Drain();

    CHECK(harness.transport->SendersRunning() == 2);
    CHECK(harness.transport->ConnectedPeers() == 2);

    harness.RequestStopAndDrain();
}

TEST_CASE("Learning a peer that has not moved changes nothing", "[consensus][raft][transport]")
{
    // The natural caller is a reconciler comparing the cluster's member set against
    // what this node dials, on every pass of its own loop. If an unchanged record
    // dropped the connection, a healthy fleet would redial every peer once per pass
    // forever.
    Harness harness;
    harness.Start();

    auto const dials = harness.connector.Attempts();
    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n2", .host = "unused", .port = 1 }) == PeerChange::Unchanged);
    harness.reactor.Drain();

    CHECK(harness.transport->PeerCount() == 1);
    CHECK(harness.connector.Attempts() == dials);
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A peer that moved is redialled at its new address", "[consensus][raft][transport]")
{
    // Re-addressed in place rather than replaced: the outbox and whatever is queued
    // in it survive, because replacing the peer would mean destroying a coroutine
    // frame the reactor may still point into.
    Harness harness;
    harness.Start();
    REQUIRE(harness.connector.LastTarget() == "unused:1");

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n2", .host = "moved", .port = 2 }) == PeerChange::Readdressed);
    harness.reactor.Drain();

    // The socket is closed and the session is not yet over, which is the honest
    // shape rather than a shortcoming to hide: the sender is parked on its outbox,
    // and closing a socket does not wake a queue. Nothing is lost by that -- a peer
    // nobody is sending to does not care which address it is not being sent to.
    CHECK(harness.transport->ConnectedPeers() == 1);

    // The next message is what discovers it. That write fails on the closed
    // socket, so it is dropped and counted, and the sender backs off exactly as it
    // does for any dropped connection.
    auto const dropped = harness.transport->DroppedMessages();
    harness.transport->Send("n2", Vote(3));
    harness.reactor.Drain();
    CHECK(harness.transport->DroppedMessages() == dropped + 1);
    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(1s);
    harness.reactor.Drain();

    // And the redial names the new address, which is the whole property.
    CHECK(harness.transport->PeerCount() == 1);
    CHECK(harness.connector.LastTarget() == "moved:2");
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.transport->Send("n2", Vote(4));
    harness.reactor.Drain();
    CHECK(harness.Writes() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A peer that moves during a dial is not served at its old address", "[consensus][raft][transport]")
{
    // The window nothing else closes. Dropping a moved peer's connection can only
    // close a socket that exists, and during a dial there is none -- so a
    // re-address that lands here used to be lost outright, and the session that
    // followed would serve the peer at the address it had just stopped answering
    // on until that connection happened to break.
    constexpr auto DialTime = 500ms;

    Harness harness;
    harness.Start();
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    // Drop the connection so the sender redials, and make that dial park.
    harness.connector.DelayDial(DialTime);
    harness.FailWrites(true);
    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    REQUIRE(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(1s);
    harness.reactor.Drain();

    // In flight now: the dial has been made and has not resolved.
    auto const dialing = harness.connector.Attempts();
    REQUIRE(harness.connector.LastTarget() == "unused:1");

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n2", .host = "moved", .port = 5 }) == PeerChange::Readdressed);

    // The dial resolves -- at the address the peer has just stopped answering on.
    // It must be discarded rather than served: nothing else would ever notice,
    // because the session it would open never re-reads the address.
    harness.FailWrites(false);
    harness.connector.DelayDial(0ms);
    harness.clock.advance(DialTime);
    harness.reactor.Drain();
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.connector.Attempts() == dialing);

    harness.clock.advance(1s);
    harness.reactor.Drain();
    CHECK(harness.connector.LastTarget() == "moved:5");
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("Learning before the start does not queue work on an unrun reactor", "[consensus][raft][transport]")
{
    // A re-address hops onto the reactor to close the moved peer's socket, and a
    // detached task submitted to a loop nobody drives is a frame nobody ever frees
    // -- or, worse, one resumed after this transport is gone. Before `Start` there
    // is no socket to close either, so there is nothing to submit.
    Harness harness;
    harness.Build();

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n2", .host = "moved", .port = 6 }) == PeerChange::Readdressed);
    CHECK(harness.reactor.PendingSubmissions() == 0);

    // And the new address is the one it dials when it does start.
    harness.transport->Start();
    harness.reactor.Drain();
    CHECK(harness.connector.LastTarget() == "moved:6");

    harness.RequestStopAndDrain();
}

TEST_CASE("This node is never learned as a peer of itself", "[consensus][raft][transport]")
{
    // A caller handing over a whole member set does not have to filter it out, for
    // the reason `Send` tolerates its own id: the rule lives in one place rather
    // than at each call site.
    Harness harness;
    harness.Start();

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n1", .host = "self", .port = 1 }) == PeerChange::Self);
    CHECK(harness.transport->PeerCount() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A peer learned after a stop is refused", "[consensus][raft][transport]")
{
    // Otherwise its sender is submitted to a reactor nobody will drain again and
    // counted in `SendersRunning`, so `Stop` waits five seconds for a coroutine
    // that cannot run and then leaks its frame -- reported as a stuck peer, caused
    // by a race in teardown.
    Harness harness;
    harness.Start();
    harness.RequestStopAndDrain();

    CHECK(harness.transport->Learn(PeerEndpoint { .id = "n3", .host = "late", .port = 3 }) == PeerChange::Stopping);
    CHECK(harness.transport->PeerCount() == 1);
    CHECK(harness.transport->SendersRunning() == 0);
}

TEST_CASE("An acceptor that proves its id is sent the messages, and nothing is counted",
          "[consensus][raft][transport][handshake]")
{
    Harness harness;
    harness.Start();

    CHECK(harness.transport->ConnectedPeers() == 1);
    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    CHECK(harness.Writes() == 1);

    for (auto const& row: DiallerRefusals)
    {
        CAPTURE(row.says);
        CHECK(harness.metrics.Read(row.counter) == 0);
    }

    harness.RequestStopAndDrain();
}

TEST_CASE("An acceptor that holds no key for this node closes, and is never sent a Raft message",
          "[consensus][raft][transport][handshake]")
{
    // A member that was never given n1's key: it cannot verify the proof, so it has no verdict
    // it may sign, and what this end sees is the connection ending after its proof.
    Harness harness;
    harness.record->acceptorRoster = Testing::SharedRoster::Of({ "n2", "n3" });
    harness.Start();

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Refused(DiallerRefusal::EndedByAcceptor) == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A verdict signed with another machine's key is refused as the acceptor's proof",
          "[consensus][raft][transport][handshake]")
{
    // The impostor that goes further than closing: it answers at n2's address AS n2, judges
    // this node's proof honestly -- it holds the roster, every public key -- and signs its
    // acceptance with the only private key it has, which is not n2's.
    Harness harness;
    harness.record->acceptorMachine = "impostor";
    harness.Start();

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Refused(DiallerRefusal::AcceptorProof) == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A signed wrong-target verdict is counted as such, and nothing is sent", "[consensus][raft][transport][handshake]")
{
    // The address this node has for n2 now answers as n3. Both prove their ids, so the
    // refusal is SIGNED and reported by name -- never as the key problem an unsigned close
    // would read as (#1308, A1).
    Harness harness;
    harness.record->acceptorId = "n3";
    harness.Start();

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Refused(DiallerRefusal::WrongTarget) == 1);
    CHECK(harness.Refused(DiallerRefusal::EndedByAcceptor) == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("A signed own-id verdict is counted as such, and nothing is sent", "[consensus][raft][transport][handshake]")
{
    // A second machine answering to this node's own id, holding this node's private key: a
    // copied --cluster-dir. Only that key proves n1, so only such a copy can send this.
    Harness harness;
    harness.record->acceptorId = "n1";
    harness.Start();

    CHECK(harness.Writes() == 0);
    CHECK(harness.Refused(DiallerRefusal::OwnId) == 1);
    CHECK(harness.Refused(DiallerRefusal::EndedByAcceptor) == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("An acceptor this node holds no key for is sent nothing, counted by name",
          "[consensus][raft][transport][handshake]")
{
    // The member answered and signed; this node cannot tell whether it is n2, because it was
    // never given n2's key. Its own row, apart from a forgery: the remedy is to give the key.
    Harness harness;
    harness.roster = Testing::SharedRoster::Of({ "n1", "n3" });
    harness.Start();

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Refused(DiallerRefusal::AcceptorKeyUnknown) == 1);
    CHECK(harness.Refused(DiallerRefusal::AcceptorProof) == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("An acceptor whose key the cluster revoked is sent nothing, counted by name",
          "[consensus][raft][transport][handshake]")
{
    // A removed machine still answering at an address this node dials, with the key it
    // always had: it verifies, under a key this node's roster has revoked.
    Harness harness;
    harness.roster->Revoke("n2");
    harness.Start();

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Refused(DiallerRefusal::AcceptorKeyRevoked) == 1);
    CHECK(harness.Refused(DiallerRefusal::AcceptorProof) == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("A signed verdict that this node's key was revoked is counted by name, and nothing is sent",
          "[consensus][raft][transport][handshake]")
{
    // This machine was removed from the cluster. The acceptor could verify its proof -- under
    // a revoked key -- so it says so, signed, and this node reports its own removal rather than
    // a key problem at n2.
    Harness harness;
    harness.record->acceptorRoster->Revoke("n1");
    harness.Start();

    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Refused(DiallerRefusal::OwnKeyRevoked) == 1);
    CHECK(harness.Refused(DiallerRefusal::EndedByAcceptor) == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("A key revoked while its session is open ends the session before the next frame, counted",
          "[consensus][raft][transport][handshake][revocation]")
{
    // #178: an applied forget closes the sessions the forgotten member's key proved. The roster is asked
    // before each frame is sealed, so the frame after the revocation is never written, and
    // the redial is judged against the roster as it is then.
    constexpr auto Backoff = 10ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff, .stopWakeBound = 5ms });

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    REQUIRE(harness.Writes() == 1);
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    harness.roster->Revoke("n2");
    harness.transport->Send("n2", Vote(2));
    harness.reactor.Drain();

    CHECK(harness.Writes() == 1);
    CHECK(harness.Refused(DiallerRefusal::KeyWithdrawn) == 1);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.Refused(DiallerRefusal::AcceptorKeyRevoked) == 1);
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.Writes() == 1);

    harness.RequestStopAndDrain(5ms);
}

TEST_CASE("An acceptor that never challenges is abandoned at the handshake bound", "[consensus][raft][transport][handshake]")
{
    // What a build from before the handshake looks like from here: it accepts, and it
    // only ever reads. Without the bound this sender would sit on that connection forever
    // with every queued message for the peer going nowhere.
    constexpr auto Bound = 300ms;
    Harness harness;
    harness.record->script = AcceptorScript::NeverChallenges;
    harness.Start(
        PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = 10s, .stopWakeBound = 50ms, .handshakeBound = Bound });

    CHECK(harness.Refused(DiallerRefusal::Timeout) == 0);
    harness.clock.advance(Bound / 2);
    harness.reactor.Drain();
    CHECK(harness.Refused(DiallerRefusal::Timeout) == 0);

    harness.clock.advance(Bound);
    harness.reactor.Drain();
    CHECK(harness.Refused(DiallerRefusal::Timeout) == 1);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("An acceptor that opens with a Raft message is not a challenge", "[consensus][raft][transport][handshake]")
{
    Harness harness;
    harness.record->script = AcceptorScript::SendsRaftFirst;
    harness.Start();

    CHECK(harness.Refused(DiallerRefusal::NoChallenge) == 1);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.RequestStopAndDrain();
}

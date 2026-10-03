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
#include <FastCache/Consensus/IRaftMessageSink.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftSessionReader.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/net/SleepUntil.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
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
class RecordingSocket final: public core::net::ISocket
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

        /// A message the acceptor writes, sealed, right behind an accepting verdict -- in the same
        /// bytes, so the dialler's next read returns both at once.
        std::optional<RaftMessage> writeBehindVerdict;

        /// Once a session is accepted, park a read with nothing to return rather than report EOF,
        /// as a live acceptor with nothing to say does -- so a two-way session stays open until
        /// one end closes it.
        bool holdSessionOpen { false };

        /// The loop a parked session read names, so `WriteToDialler` SETTLES it at once and the
        /// reader resumes LATER, in that loop's drain -- as a real socket completes a read (G2).
        /// Null keeps the fake's inline resume. What lets a case place a writer's queued wake AHEAD
        /// of a read that has already completed, which an inline resume can never produce.
        core::net::EventLoop* deferCompletionsThrough { nullptr };

        /// Writes the transport attempted on a connection already closed. Each is refused, as a
        /// real socket refuses it; counted so a case can tell "never tried" from "tried and failed".
        std::size_t writesAfterClose { 0 };

        /// The bytes queued behind the proof on the last connection: its verdict, and whatever
        /// was written behind it.
        std::size_t queuedAfterProof { 0 };

        /// Every read that returned bytes, by how many it returned, in order.
        std::vector<std::size_t> readSizes;

        /// The connection now open, or null: what `Harness::AcceptorWrites` speaks through.
        RecordingSocket* current { nullptr };
    };

    /// How a frame the acceptor writes mid-session is sealed. Private: a test fixture's.
    enum class SealAs : std::uint8_t
    {
        Sealed,       ///< Under the session's acceptor-to-dialler key, at its next position.
        TagCorrupted, ///< Sealed, then one bit of the tag flipped.
        Unsealed,     ///< As given: for a frame the dialler refuses before it reads any tag.
    };

    /// @param record Where to append; must outlive the socket.
    explicit RecordingSocket(std::shared_ptr<Record> const& record):
        RecordingSocket { record, record->NextConnection() }
    {
        std::scoped_lock const guard { _record->mutex };
        _record->current = this;
    }

    RecordingSocket(RecordingSocket const&) = delete;
    RecordingSocket(RecordingSocket&&) = delete;
    RecordingSocket& operator=(RecordingSocket const&) = delete;
    RecordingSocket& operator=(RecordingSocket&&) = delete;

    /// Leaves the record, so a case never speaks through a connection the transport destroyed.
    ~RecordingSocket() override
    {
        std::scoped_lock const guard { _record->mutex };
        if (_record->current == this)
            _record->current = nullptr;
    }

    /// Write @p frame to the dialler as the acceptor would mid-session, and hand it to a read
    /// parked for it -- inline, as `Close()` completes one.
    /// @param frame A frame as `RaftWire::Encode` produced it, possibly altered by the case.
    /// @param seal How it is sealed.
    void WriteToDialler(std::vector<std::byte> frame, SealAs seal)
    {
        if (seal != SealAs::Unsealed && _sealer.has_value())
        {
            auto const bytes = std::span<std::byte const> { frame };
            auto tag = _sealer->Seal(bytes.first(RaftWire::HeaderSize), bytes.subspan(RaftWire::HeaderSize));
            if (seal == SealAs::TagCorrupted)
                tag.front() ^= std::byte { 0x01 };
            frame.insert(frame.end(), tag.begin(), tag.end());
        }
        _inbound.insert(_inbound.end(), frame.begin(), frame.end());

        auto* const parked = std::exchange(_parkedRead, nullptr);
        if (parked == nullptr)
            return;
        auto const count = std::min(_parkedBuffer.size(), _inbound.size());
        std::copy_n(_inbound.begin(), count, _parkedBuffer.begin());
        _inbound.erase(_inbound.begin(), _inbound.begin() + static_cast<std::ptrdiff_t>(count));
        parked->complete(core::net::IoResult { count });
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

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        if (_closed)
            return core::net::IoAwaitable { std::unexpected { core::net::NetError {
                .code = core::net::NetErrorCode::BadHandle, .systemCode = 0, .context = "socket closed" } } };

        if (!_inbound.empty())
        {
            auto const count = std::min(buffer.size(), _inbound.size());
            std::copy_n(_inbound.begin(), count, buffer.begin());
            _inbound.erase(_inbound.begin(), _inbound.begin() + static_cast<std::ptrdiff_t>(count));
            {
                std::scoped_lock const guard { _record->mutex };
                _record->readSizes.push_back(count);
            }
            return core::net::IoAwaitable { core::net::IoResult { count } };
        }

        // An acceptor that says nothing: parked until `Close()`, which is what the
        // handshake bound does to it -- or a session held open with nothing more to say.
        auto holdOpen = false;
        {
            std::scoped_lock const guard { _record->mutex };
            holdOpen = _record->holdSessionOpen && _opener.has_value();
            _deferThrough = _opener.has_value() ? _record->deferCompletionsThrough : nullptr;
        }
        if (_script == AcceptorScript::NeverChallenges || holdOpen)
        {
            _parkedBuffer = buffer;
            return core::net::IoAwaitable {
                [](void* owner, core::net::IoAwaitable& self) {
                    auto* const socket = static_cast<RecordingSocket*>(owner);
                    socket->_parkedRead = &self;
                    if (socket->_deferThrough != nullptr)
                        self.cancelThrough(*socket->_deferThrough, core::net::ParkId::invalid());
                },
                [](void* owner, void* awaitable) noexcept {
                    auto* const socket = static_cast<RecordingSocket*>(owner);
                    if (socket->_parkedRead == awaitable)
                        socket->_parkedRead = nullptr;
                },
                this,
            };
        }

        // Nothing more is coming: the acceptor refused the proof, or has nothing to say
        // after the verdict.
        return core::net::IoAwaitable { core::net::IoResult { std::size_t { 0 } } };
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        // A closed socket refuses, as every real one does. Recording the write
        // instead would let the transport go on feeding a connection it has
        // dropped -- and "the peer moved, so its connection was closed" is a case
        // whose whole observable consequence is the write that then fails.
        if (_closed)
        {
            {
                std::scoped_lock const guard { _record->mutex };
                ++_record->writesAfterClose;
            }
            return core::net::IoAwaitable { std::unexpected { core::net::NetError {
                .code = core::net::NetErrorCode::ConnReset, .systemCode = 0, .context = "socket closed" } } };
        }

        if (!_opener.has_value())
            return AnswerProof(buffer);

        std::scoped_lock const guard { _record->mutex };
        if (_record->failWrites)
            return core::net::IoAwaitable { std::unexpected { core::net::NetError {
                .code = core::net::NetErrorCode::ConnReset, .systemCode = 0, .context = "scripted write failure" } } };

        if (_record->parkWrites)
        {
            // Deferred, and stashed so `Close()` can complete it -- mirroring
            // `EpollSocket`, where closing is exactly what resolves a parked
            // operation.
            return core::net::IoAwaitable {
                [](void* owner, core::net::IoAwaitable& self) {
                    static_cast<RecordingSocket*>(owner)->_parkedWrite = &self;
                },
                [](void* owner, void* awaitable) noexcept {
                    auto* const socket = static_cast<RecordingSocket*>(owner);
                    if (socket->_parkedWrite == awaitable)
                        socket->_parkedWrite = nullptr;
                },
                this,
            };
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
        return core::net::IoAwaitable { core::net::IoResult { buffer.size() } };
    }

    [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                       std::shared_ptr<void const> keepAlive = {}) override
    {
        std::ignore = keepAlive;
        std::size_t total = 0;
        for (auto const& segment: segments)
            total += segment.size();
        return core::net::IoAwaitable { core::net::IoResult { total } };
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
                parked->complete(std::unexpected { core::net::NetError {
                    .code = core::net::NetErrorCode::Cancelled, .systemCode = 0, .context = "socket closed" } });
    }

    [[nodiscard]] bool isClosed() const noexcept override
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
    [[nodiscard]] core::net::IoAwaitable AnswerProof(std::span<std::byte const> buffer)
    {
        auto const header = RaftWire::DecodeHeader(buffer);
        auto const proof =
            header.has_value()
                ? RaftWire::DecodeProof(*header, buffer.subspan(RaftWire::HeaderSize))
                : std::expected<RaftWire::ProofFrame, ConsensusError> { std::unexpect, MalformedWireFrame("no header") };
        if (!proof.has_value())
            return core::net::IoAwaitable { core::net::IoResult { buffer.size() } };

        NodeId self;
        auto behind = std::optional<RaftMessage> {};
        {
            std::scoped_lock const guard { _record->mutex };
            self = _record->acceptorId.value_or(proof->target);
            behind = _record->writeBehindVerdict;
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
        {
            _opener.emplace(std::move(judgement.session->diallerToAcceptor));
            _sealer.emplace(std::move(judgement.session->acceptorToDialler));

            // Sealed as the acceptor's transport seals it, at position zero of its own direction,
            // and appended to the SAME bytes as the verdict. Whatever the proof's direction, so a
            // one-way dialler is shown not to read it rather than simply never being sent it.
            if (behind.has_value())
            {
                auto frame = RaftWire::Encode(*behind);
                auto const bytes = std::span<std::byte const> { frame };
                auto const tag = _sealer->Seal(bytes.first(RaftWire::HeaderSize), bytes.subspan(RaftWire::HeaderSize));
                frame.insert(frame.end(), tag.begin(), tag.end());
                _inbound.insert(_inbound.end(), frame.begin(), frame.end());
            }
        }
        {
            std::scoped_lock const guard { _record->mutex };
            _record->queuedAfterProof = _inbound.size();
        }
        return core::net::IoAwaitable { core::net::IoResult { buffer.size() } };
    }

    std::shared_ptr<Record> _record;
    std::string _machine;
    std::shared_ptr<Testing::SharedRoster const> _roster;
    AcceptorScript _script { AcceptorScript::Answers };
    std::uint64_t _seed { 0 };
    std::deque<std::byte> _inbound;
    std::optional<FrameOpener> _opener;
    std::optional<FrameSealer> _sealer; ///< What this acceptor writes under, once it has accepted.
    core::net::IoAwaitable* _parkedWrite { nullptr };
    core::net::IoAwaitable* _parkedRead { nullptr };
    std::span<std::byte> _parkedBuffer;              ///< The parked read's buffer, which `WriteToDialler` fills.
    core::net::EventLoop* _deferThrough { nullptr }; ///< The loop a parked session read names, or none.
    bool _closed { false };
};

using SealAs = RecordingSocket::SealAs;

/// A connector under the test's control: it can refuse, or hand out sockets
/// that record what the transport wrote.
class ScriptedConnector final: public core::net::IConnector
{
  public:
    /// @param record Shared write log handed to every socket produced.
    /// @param reactor Where a delayed dial parks; must outlive the connector.
    ScriptedConnector(std::shared_ptr<RecordingSocket::Record> record, core::net::EventLoop& reactor) noexcept:
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
    /// which a dial is in flight. `core::net::makeConnector`'s connector really does suspend there, and
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
    /// because `core::net::makeConnector`'s connector does: a dial in flight is a real state, and the
    /// transport has to behave when a peer moves during one.
    [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                     std::uint16_t port,
                                                                     core::net::DialOptions options) override
    {
        std::ignore = options;
        _attempts.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock const guard { _targetMutex };
            _lastTarget = std::format("{}:{}", host, port);
        }

        if (auto const delay = _delay.load(std::memory_order_relaxed); delay > 0)
            co_await core::net::sleepUntil(&_reactor, _reactor.clock().now() + std::chrono::milliseconds { delay });

        if (_refuse.load(std::memory_order_relaxed))
            co_return std::unexpected { core::net::NetError {
                .code = core::net::NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" } };
        co_return std::make_unique<RecordingSocket>(_record);
    }

  private:
    std::shared_ptr<RecordingSocket::Record> _record;
    core::net::EventLoop& _reactor;
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

/// Records what a two-way session delivers.
class RecordingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        if (throwOnDeliver)
            throw std::runtime_error { "a sink that throws" };
        received.push_back(std::move(message));
    }

    std::vector<RaftMessage> received; ///< In arrival order; the reactor is this thread.
    bool throwOnDeliver { false };     ///< Throw instead of recording: a driver that failed.
};

/// Clock, reactor and transport, wired the way production wires them.
///
/// Every wall-clock wait has left this file. The senders run on a `core::net::testing::TestLoop`
/// driven by the test's own thread, so "let the sender make progress" is
/// `Drain()` and "let the backoff elapse" is `Advance()`. What that buys is not
/// only speed: the old file had a case whose comment described a race it had to
/// engineer around, and that race no longer exists.
struct Harness
{
    std::shared_ptr<RecordingSocket::Record> record { std::make_shared<RecordingSocket::Record>() };
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    ScriptedConnector connector { record, reactor };
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    SystemSecureRandom random;
    RecordingSink sink; ///< What an acceptor wrote back on a two-way session.

    /// What the transport believes about everybody's keys. A case may replace it, or revoke
    /// from it, before `Build`; revoking after is a key withdrawn from an open session.
    std::shared_ptr<Testing::SharedRoster> roster { Roster() };

    /// Where the transport draws its nonces: `random`, unless a case points it elsewhere
    /// before `Build`.
    ISecureRandom* nonces { &random };

    /// Who the transport is: n1, under its own key, over `roster`. Built by `Build`.
    std::unique_ptr<Testing::TestPeerIdentity const> identity;

    std::unique_ptr<RaftPeerTransport> transport;

    Harness() = default;
    Harness(Harness const&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness const&) = delete;
    Harness& operator=(Harness&&) = delete;

    /// Stops the senders on this thread, whichever way the case left: a failing `REQUIRE` unwinds
    /// past a case's own `RequestStopAndDrain`, and the transport's waiting `Stop()` must not run
    /// on the thread that drives its reactor. A second stop after the case's own is a no-op.
    ~Harness()
    {
        if (transport != nullptr)
            RequestStopAndDrain();
    }

    /// Make the acceptor write @p frame on the connection now open, handed to a read parked for it.
    /// @param frame A frame as `RaftWire::Encode` produced it, possibly altered by the case.
    /// @param seal How it is sealed.
    void AcceptorWrites(std::vector<std::byte> frame, SealAs seal) const
    {
        RecordingSocket* socket = nullptr;
        {
            std::scoped_lock const guard { record->mutex };
            socket = record->current;
        }
        REQUIRE(socket != nullptr);
        socket->WriteToDialler(std::move(frame), seal);
    }

    /// `AcceptorWrites`, run by the loop in its next turn rather than now.
    ///
    /// Posted from the case's thread, so it lands in the loop's inbound queue in the order posted: a
    /// `Send` made after this queues the writer's wake BEHIND it. The write then settles the parked
    /// read inside that turn, and a read naming the loop (`deferCompletionsThrough`) resumes its
    /// reader at the back of the ready queue -- behind that writer.
    /// @param frame A frame as `RaftWire::Encode` produced it.
    /// @param seal How it is sealed.
    void AcceptorWritesNextTurn(std::vector<std::byte> frame, SealAs seal)
    {
        [](Harness* harness, std::vector<std::byte> bytes, SealAs sealAs) -> core::async::DetachedTask {
            co_await core::async::ResumeOn { harness->reactor };
            harness->AcceptorWrites(std::move(bytes), sealAs);
        }(this, std::move(frame), seal);
    }

    /// Make the peer's socket refuse or accept writes.
    /// @param fail Whether writes should fail.
    void FailWrites(bool fail) const
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
        transport = std::make_unique<RaftPeerTransport>(
            OnePeer(), reactor, connector, sink, logger, metrics, *identity, *nonces, options);
    }

    /// Build the transport over one peer and start its sender.
    /// @param options Timeouts and queue bound.
    void Start(PeerTransportOptions options = {})
    {
        Build(options);
        transport->Start();
        reactor.drain();
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
        reactor.drain();
        clock.advance(wakeBound);
        reactor.drain();
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

    harness.reactor.drain();
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
    harness.reactor.drain();

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
    harness.reactor.drain();
    CHECK(harness.Writes() == 4);

    harness.RequestStopAndDrain();
}

TEST_CASE("A message for this node itself is not sent anywhere", "[consensus][raft][transport]")
{
    // A configuration listing every member uniformly is the natural thing to
    // write, so the transport tolerates its own id rather than making each caller
    // filter -- but it must not give itself a socket and a sender.
    auto record = std::make_shared<RecordingSocket::Record>();
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    ScriptedConnector connector { record, reactor };
    NullLogger logger;
    AtomicMetricsSink metrics;
    auto const identity = Testing::TestPeerIdentity::Honest("n1", Roster());
    SystemSecureRandom random;
    RecordingSink sink;

    std::vector<PeerEndpoint> peers { PeerEndpoint { .id = "n1", .host = "self", .port = 1 },
                                      PeerEndpoint { .id = "n2", .host = "unused", .port = 2 } };
    RaftPeerTransport transport { std::move(peers), reactor, connector, sink, logger, metrics, *identity, random };
    transport.Start();
    reactor.drain();

    transport.Send("n1", Vote(1));
    transport.Send("n2", Vote(2));
    reactor.drain();

    {
        std::scoped_lock const guard { record->mutex };
        // Exactly one write: the peer's. The self-addressed one was refused.
        CHECK(record->writes.size() == 1);
    }

    // Drained before the transport is destroyed. Its destructor calls the
    // waiting `Stop()`, which asserts when called from the thread the senders run
    // on -- and in a test that thread is this one.
    transport.RequestStop();
    reactor.drain();
    clock.advance(50ms);
    reactor.drain();
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
    //
    // And with the clock NOT moved at all (#1596): the backoff is one park that the stop cancels,
    // where it used to be slept in `stopWakeBound` steps so a stop was noticed within one.
    constexpr auto Backoff = 30s;

    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 10s, .reconnectBackoff = Backoff });

    auto const started = harness.clock.now();
    harness.RequestStopAndDrain(0ms);

    CHECK(harness.transport->SendersRunning() == 0);
    // Woken by the stop itself, not by a step and not after the backoff.
    CHECK(harness.clock.now() == started);
    CHECK(harness.reactor.pendingTimers() == 0);
    CHECK(harness.reactor.pendingSubmissions() == 0);
}

TEST_CASE("A refused dial is not retried faster than the backoff", "[consensus][raft][transport]")
{
    // The dial-storm guard. Without it a partitioned node opens connections as
    // fast as it can to a host that is down, which fills a conntrack table and
    // starts breaking unrelated traffic to that host.
    constexpr auto Backoff = 200ms;
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = Backoff });

    REQUIRE(harness.connector.Attempts() == 1);

    harness.clock.advance(Backoff / 2);
    harness.reactor.drain();
    CHECK(harness.connector.Attempts() == 1);

    harness.clock.advance(Backoff);
    harness.reactor.drain();
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
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = Backoff });
    REQUIRE(harness.connector.Attempts() == 1);

    // A write that fails ends the session, so the sender goes round the loop.
    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();

    // Still one: the redial waits for the backoff rather than happening at once.
    CHECK(harness.connector.Attempts() == 1);

    harness.clock.advance(Backoff);
    harness.reactor.drain();
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
    harness.reactor.drain();

    // The sender is inside the write, and nothing the peer does will end it.
    REQUIRE(harness.transport->SendersRunning() == 1);
    REQUIRE(harness.Writes() == 0);

    harness.RequestStopAndDrain();

    CHECK(harness.transport->SendersRunning() == 0);
    CHECK(harness.reactor.pendingSubmissions() == 0);
    CHECK(harness.reactor.pendingTimers() == 0);
}

TEST_CASE("A dropped connection is redialled", "[consensus][raft][transport]")
{
    constexpr auto Backoff = 10ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff });

    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();
    REQUIRE(harness.Writes() == 1);
    auto const firstAttempts = harness.connector.Attempts();

    // Fail one write, which ends the session; then let the backoff elapse.
    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(2));
    harness.reactor.drain();
    harness.clock.advance(Backoff);
    harness.reactor.drain();

    CHECK(harness.connector.Attempts() > firstAttempts);

    // And the reconnected sender writes again. No race to engineer around: the
    // old version of this case had a comment describing one, and it no longer
    // exists because nothing here runs on a thread of its own.
    harness.record->failWrites = false;
    harness.transport->Send("n2", Vote(3));
    harness.reactor.drain();
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
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff });

    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.record->failWrites = false;
    harness.clock.advance(Backoff);
    harness.reactor.drain();
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.RequestStopAndDrain(5ms);
    CHECK(harness.transport->ConnectedPeers() == 0);
}

TEST_CASE("Stop is idempotent and safe before Start", "[consensus][raft][transport]")
{
    auto record = std::make_shared<RecordingSocket::Record>();
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    ScriptedConnector connector { record, reactor };
    NullLogger logger;
    AtomicMetricsSink metrics;
    auto const identity = Testing::TestPeerIdentity::Honest("n1", Roster());
    SystemSecureRandom random;
    RecordingSink sink;

    RaftPeerTransport transport { OnePeer(), reactor, connector, sink, logger, metrics, *identity, random };
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
    harness.reactor.drain();

    harness.transport->Send("n3", Vote(2));
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.reactor.drain();
    CHECK(harness.transport->DroppedMessages() == dropped + 1);
    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(1s);
    harness.reactor.drain();

    // And the redial names the new address, which is the whole property.
    CHECK(harness.transport->PeerCount() == 1);
    CHECK(harness.connector.LastTarget() == "moved:2");
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.transport->Send("n2", Vote(4));
    harness.reactor.drain();
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
    harness.reactor.drain();
    REQUIRE(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(1s);
    harness.reactor.drain();

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
    harness.reactor.drain();
    CHECK(harness.transport->ConnectedPeers() == 0);
    CHECK(harness.connector.Attempts() == dialing);

    harness.clock.advance(1s);
    harness.reactor.drain();
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
    CHECK(harness.reactor.pendingSubmissions() == 0);

    // And the new address is the one it dials when it does start.
    harness.transport->Start();
    harness.reactor.drain();
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
    harness.reactor.drain();
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
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.reactor.drain();

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
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff });

    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();
    REQUIRE(harness.Writes() == 1);
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    harness.roster->Revoke("n2");
    harness.transport->Send("n2", Vote(2));
    harness.reactor.drain();

    CHECK(harness.Writes() == 1);
    CHECK(harness.Refused(DiallerRefusal::KeyWithdrawn) == 1);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(Backoff);
    harness.reactor.drain();
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
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = 10s, .handshakeBound = Bound });

    CHECK(harness.Refused(DiallerRefusal::Timeout) == 0);
    harness.clock.advance(Bound / 2);
    harness.reactor.drain();
    CHECK(harness.Refused(DiallerRefusal::Timeout) == 0);

    harness.clock.advance(Bound);
    harness.reactor.drain();
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

// A learner's transport dials TWO-WAY: the acceptor writes back on the connection, and this end
// reads what arrives there through the reader its handshake used.

namespace
{

/// What the scripted acceptor writes back on a two-way session: an AppendEntries naming it.
/// @param term The message's term, so two deliveries are distinguishable.
/// @return The message.
[[nodiscard]] RaftMessage LeaderAppend(std::uint64_t term)
{
    return RaftMessage { AppendEntriesRequest { .term = Term { .value = term },
                                                .leaderId = NodeId { "n2" },
                                                .prevLogIndex = LogIndex {},
                                                .prevLogTerm = Term {},
                                                .entries = {},
                                                .leaderCommit = LogIndex {} } };
}

/// The dial counts either side of a backoff's end.
struct DialsAround
{
    std::size_t before { 0 }; ///< One millisecond before the wait elapsed.
    std::size_t after { 0 };  ///< Once it had.
};

/// Advance the clock to one millisecond short of @p wait and then to it, and report the dials
/// counted at each point -- so a case asserts the redial happened AT the wait, not merely after.
/// @param harness The transport under test.
/// @param wait The backoff the sender is expected to be taking.
/// @return The dial counts either side.
[[nodiscard]] DialsAround StepThrough(Harness& harness, std::chrono::milliseconds wait)
{
    harness.clock.advance(wait - 1ms);
    harness.reactor.drain();
    auto const before = harness.connector.Attempts();
    harness.clock.advance(1ms);
    harness.reactor.drain();
    return DialsAround { .before = before, .after = harness.connector.Attempts() };
}

} // namespace

TEST_CASE("A two-way dialler reads the frame an acceptor wrote in the same read as its verdict",
          "[consensus][raft][transport][learner]")
{
    // The acceptor writes its first session frame right behind its verdict, and the two arrive
    // in ONE read -- so a reader built for the handshake alone and dropped at the verdict would
    // take the frame with it, silently. The arrangement is asserted as well as the delivery, or
    // a fake that happened to split the bytes would pass without testing anything.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->writeBehindVerdict = LeaderAppend(5);
    }
    harness.Start(PeerTransportOptions { .reconnectBackoff = 10s, .direction = RaftWire::SessionDirection::TwoWay });

    REQUIRE(harness.sink.received.size() == 1);
    CHECK(std::get<AppendEntriesRequest>(harness.sink.received[0]).term.value == 5);

    {
        std::scoped_lock const guard { harness.record->mutex };
        auto const frameBytes = RaftWire::Encode(LeaderAppend(5)).size() + RaftWire::TagSize;
        CHECK(harness.record->queuedAfterProof > frameBytes); // the verdict AND the frame
        REQUIRE_FALSE(harness.record->readSizes.empty());
        CHECK(harness.record->readSizes.back() == harness.record->queuedAfterProof);
    }

    harness.RequestStopAndDrain();
}

TEST_CASE("A one-way dialler never reads past the verdict", "[consensus][raft][transport][learner]")
{
    // The control for the case above: the same acceptor writing the same frame behind its
    // verdict, and a transport that proves one-way. It reads nothing after the verdict, so the
    // frame is never delivered and its sender is unaffected.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->writeBehindVerdict = LeaderAppend(5);
    }
    harness.Start(PeerTransportOptions { .reconnectBackoff = 10s });
    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();

    CHECK(harness.Writes() == 1);
    CHECK(harness.sink.received.empty());
    harness.RequestStopAndDrain();
}

TEST_CASE("A two-way dialler that cannot connect waits one second, doubling to thirty",
          "[consensus][raft][transport][learner]")
{
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .direction = RaftWire::SessionDirection::TwoWay });
    REQUIRE(harness.connector.Attempts() == 1);

    auto dials = std::size_t { 1 };
    for (auto const wait: { 1000ms, 2000ms, 4000ms, 8000ms, 16000ms, 30000ms, 30000ms })
    {
        auto const around = StepThrough(harness, wait);
        CHECK(around.before == dials);
        CHECK(around.after == dials + 1);
        dials = around.after;
    }

    harness.RequestStopAndDrain();
}

TEST_CASE("A one-way dialler that cannot connect waits a flat quarter second", "[consensus][raft][transport]")
{
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start();
    REQUIRE(harness.connector.Attempts() == 1);

    auto dials = std::size_t { 1 };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 6))
    {
        auto const around = StepThrough(harness, 250ms);
        CHECK(around.before == dials);
        CHECK(around.after == dials + 1);
        dials = around.after;
    }

    harness.RequestStopAndDrain();
}

TEST_CASE("A two-way session that carried a frame each way starts the backoff again from one second",
          "[consensus][raft][transport][learner]")
{
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->writeBehindVerdict = LeaderAppend(3);
        harness.record->holdSessionOpen = true;
    }
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .direction = RaftWire::SessionDirection::TwoWay });

    // Two refusals grow the wait to four seconds.
    CHECK(StepThrough(harness, 1000ms).after == 2);
    CHECK(StepThrough(harness, 2000ms).after == 3);

    // The third redial is answered: a frame arrives, and one goes out.
    harness.connector.Refuse(false);
    REQUIRE(StepThrough(harness, 4000ms).after == 4);
    REQUIRE(harness.sink.received.size() == 1);
    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();
    REQUIRE(harness.Writes() == 1);
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    // The session breaks; the peer was up, so the next dial is one second away, not eight.
    harness.FailWrites(true);
    harness.transport->Send("n2", Vote(2));
    harness.reactor.drain();
    REQUIRE(harness.transport->ConnectedPeers() == 0);
    auto const around = StepThrough(harness, 1000ms);
    CHECK(around.before == 4);
    CHECK(around.after == 5);

    harness.RequestStopAndDrain();
}

TEST_CASE("A two-way session the acceptor ends wakes its idle writer", "[consensus][raft][transport][learner]")
{
    // The reader ends first, with nothing queued: only the zero-length frame it pushes wakes the
    // writer parked on the outbox, and without it the session would sit half-closed -- counted as
    // connected, and never redialled -- until the next message came.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->writeBehindVerdict = LeaderAppend(4);
    }
    harness.Start(PeerTransportOptions { .reconnectBackoff = 100ms, .direction = RaftWire::SessionDirection::TwoWay });

    // The scripted acceptor reports EOF once its frame is read, so the session ends at once --
    // and ends WHOLE, the writer included, with no message ever sent.
    REQUIRE(harness.sink.received.size() == 1);
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.clock.advance(100ms);
    harness.reactor.drain();
    CHECK(harness.connector.Attempts() == 2);
    CHECK(harness.sink.received.size() == 2);

    harness.RequestStopAndDrain();
}

TEST_CASE("A wake left behind by one two-way session does not end the next", "[consensus][raft][transport][learner]")
{
    // A message is queued before the first session, so its writer is busy with it -- a write on a
    // socket the reader has just closed -- when the reader ends and pushes its zero-length frame.
    // Nobody pops that frame in the first session; the SECOND session's writer finds it first,
    // and must step over it, since that session's reader is alive.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->writeBehindVerdict = LeaderAppend(4);
    }
    harness.Build(PeerTransportOptions { .reconnectBackoff = 100ms, .direction = RaftWire::SessionDirection::TwoWay });
    harness.transport->Send("n2", Vote(8));
    harness.transport->Start();
    harness.reactor.drain();

    REQUIRE(harness.sink.received.size() == 1);
    CHECK(harness.Writes() == 0); // the queued vote went to a closed socket, and was dropped
    CHECK(harness.transport->ConnectedPeers() == 0);

    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->holdSessionOpen = true;
    }
    harness.clock.advance(100ms);
    harness.reactor.drain();
    REQUIRE(harness.connector.Attempts() == 2);
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.transport->Send("n2", Vote(9));
    harness.reactor.drain();
    CHECK(harness.Writes() == 1);
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A withdrawn acceptor key the reader counted is not counted again by the writer",
          "[consensus][raft][transport][learner][revocation]")
{
    // The interleaving is placed, not waited for: a `Send` posts the writer's wake, and before the
    // reactor turns, the acceptor's frame reaches the parked reader -- which finds the acceptor's
    // key withdrawn, counts it and ends the session. Only then does the writer pop the vote. It must
    // not judge that vote against the roster a second time: the session is already over.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->holdSessionOpen = true;
    }
    harness.Start(PeerTransportOptions { .reconnectBackoff = 10s, .direction = RaftWire::SessionDirection::TwoWay });
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    harness.roster->Revoke("n2");
    harness.transport->Send("n2", Vote(1));
    harness.AcceptorWrites(RaftWire::Encode(LeaderAppend(7)), SealAs::Sealed);
    harness.reactor.drain();

    CHECK(harness.Refused(DiallerRefusal::KeyWithdrawn) == 1);
    CHECK(harness.Writes() == 0); // the vote popped after the ending went nowhere
    CHECK(harness.sink.received.empty());
    CHECK(harness.transport->ConnectedPeers() == 0);
}

TEST_CASE("A frame popped after the reader ended is dropped, never written to the socket the reader closed",
          "[consensus][raft][transport][learner]")
{
    // The reader ends on a frame whose tag fails, closes the socket and wakes the writer -- behind a
    // vote a `Send` had already queued. That vote is dropped with the session. Judging it instead
    // would find the key still good, seal it, and write it to a socket the reader had closed: a write
    // a real socket refuses too, so only the ATTEMPT tells the two apart.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->holdSessionOpen = true;
    }
    harness.Start(PeerTransportOptions { .reconnectBackoff = 10s, .direction = RaftWire::SessionDirection::TwoWay });
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    harness.transport->Send("n2", Vote(1));
    harness.AcceptorWrites(RaftWire::Encode(LeaderAppend(7)), SealAs::TagCorrupted);
    harness.reactor.drain();

    CHECK(harness.Refused(DiallerRefusal::FrameTag) == 1);
    CHECK(harness.Writes() == 0);
    CHECK(harness.transport->DroppedMessages() == 1);
    std::scoped_lock const guard { harness.record->mutex };
    CHECK(harness.record->writesAfterClose == 0);
}

TEST_CASE("A withdrawn acceptor key the writer counted is not counted again by the reader",
          "[consensus][raft][transport][learner][revocation]")
{
    // The mirror of the case above. A real socket settles a read when its data arrives and resumes
    // the reader later, in the loop's drain, behind whatever is already ready. So the writer can run
    // FIRST -- the roster refuses it, it counts the withdrawal and ends the session -- and the reader
    // then resumes with an acceptor frame that had already arrived, and finds the same withdrawn key.
    //
    // Placed, not waited for: the acceptor's write is posted first and the `Send` second, so in the
    // next turn the write settles the read while the writer's wake is already ready, and the reader's
    // resumption files in behind it. Neither half's own check can stop the second count here -- the
    // reader has not ended when the writer counts, and the writer has when the reader does -- so
    // only the session's once-only count can.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->holdSessionOpen = true;
        harness.record->deferCompletionsThrough = &harness.reactor;
    }
    harness.Start(PeerTransportOptions { .reconnectBackoff = 10s, .direction = RaftWire::SessionDirection::TwoWay });
    REQUIRE(harness.transport->ConnectedPeers() == 1);

    harness.roster->Revoke("n2");
    harness.AcceptorWritesNextTurn(RaftWire::Encode(LeaderAppend(7)), SealAs::Sealed);
    harness.transport->Send("n2", Vote(1));
    harness.reactor.drain();

    CHECK(harness.Refused(DiallerRefusal::KeyWithdrawn) == 1);
    CHECK(harness.Writes() == 0);
    CHECK(harness.sink.received.empty());
    CHECK(harness.transport->ConnectedPeers() == 0);
}

TEST_CASE("A frame the acceptor writes that the dialler cannot use ends the session on the dialler's own row",
          "[consensus][raft][transport][learner]")
{
    // Only the dialler reads this direction, so a frame it refuses here is counted nowhere else.
    // One row per `SessionEnd` but a close, and the case asserts WHICH moved: the named one, once,
    // and no other dialler row at all. A row whose refusal went missing from `DiallerSessionEndRows`
    // fails here under its own name.
    struct Row
    {
        std::string_view what;
        SessionEnd end;
        DiallerRefusal expected;
        SealAs seal;
        std::vector<std::byte> (*frame)();
        void (*arrange)(Harness&) { nullptr };
    };
    auto const rows = std::array {
        Row { .what = "a key withdrawn before the acceptor's frame",
              .end = SessionEnd::KeyWithdrawn,
              .expected = DiallerRefusal::KeyWithdrawn,
              .seal = SealAs::Sealed,
              .frame = [] { return RaftWire::Encode(LeaderAppend(1)); },
              .arrange = [](Harness& harness) { harness.roster->Revoke("n2"); } },
        Row { .what = "a tag that does not verify",
              .end = SessionEnd::BadTag,
              .expected = DiallerRefusal::FrameTag,
              .seal = SealAs::TagCorrupted,
              .frame = [] { return RaftWire::Encode(LeaderAppend(1)); } },
        Row { .what = "a verified message naming another member",
              .end = SessionEnd::WrongSender,
              .expected = DiallerRefusal::FrameSender,
              .seal = SealAs::Sealed,
              .frame =
                  [] {
                      return RaftWire::Encode(RaftMessage { AppendEntriesRequest { .term = Term { .value = 1 },
                                                                                   .leaderId = NodeId { "n3" },
                                                                                   .prevLogIndex = LogIndex {},
                                                                                   .prevLogTerm = Term {},
                                                                                   .entries = {},
                                                                                   .leaderCommit = LogIndex {} } });
                  } },
        Row { .what = "a verified frame at another wire version",
              .end = SessionEnd::Unreadable,
              .expected = DiallerRefusal::FrameUnreadable,
              .seal = SealAs::Sealed,
              .frame =
                  [] {
                      auto frame = RaftWire::Encode(LeaderAppend(1));
                      frame[1] = std::byte { 0x7F };
                      return frame;
                  } },
        Row {
            .what = "a frame declaring more than the dialler buffers",
            .end = SessionEnd::OverCap,
            .expected = DiallerRefusal::FrameOverCap,
            .seal = SealAs::Unsealed,
            .frame =
                [] {
                    auto frame = RaftWire::Encode(LeaderAppend(1));
                    WireFields::PutBigEndian<std::uint32_t>(
                        frame, WireFrame::LengthOffset, static_cast<std::uint32_t>(SessionReadLimits {}.maxFrameBytes + 1));
                    return frame;
                } },
        Row { .what = "a frame that is not this wire",
              .end = SessionEnd::BadMagic,
              .expected = DiallerRefusal::FrameBadMagic,
              .seal = SealAs::Unsealed,
              .frame =
                  [] {
                      auto frame = RaftWire::Encode(LeaderAppend(1));
                      frame[0] ^= std::byte { 0xFF };
                      return frame;
                  } },
    };

    // Every ending but a close has a row here, so a new `SessionEnd` cannot go uncounted unnoticed.
    for (auto const ending: Enumerators<SessionEnd>())
    {
        INFO("SessionEnd " << static_cast<int>(ending));
        auto const covered = std::ranges::any_of(rows, [ending](Row const& row) { return row.end == ending; });
        CHECK(covered == (ending != SessionEnd::PeerClosed));
    }

    for (auto const& row: rows)
    {
        INFO(row.what);
        Harness harness;
        {
            std::scoped_lock const guard { harness.record->mutex };
            harness.record->holdSessionOpen = true;
        }
        harness.Start(PeerTransportOptions { .reconnectBackoff = 10s, .direction = RaftWire::SessionDirection::TwoWay });
        REQUIRE(harness.transport->ConnectedPeers() == 1);

        if (row.arrange != nullptr)
            row.arrange(harness);
        harness.AcceptorWrites(row.frame(), row.seal);
        harness.reactor.drain();

        CHECK(harness.sink.received.empty());
        CHECK(harness.transport->ConnectedPeers() == 0);
        for (auto const& refusal: DiallerRefusals)
            CHECK(harness.Refused(refusal.refusal) == (refusal.refusal == row.expected ? 1U : 0U));
    }
}

TEST_CASE("A two-way session whose reader throws ends whole rather than stranding its writer",
          "[consensus][raft][transport][learner]")
{
    // `whenAll` waits for both halves, and the writer ends only when the reader tells it to. A reader
    // that threw and skipped that would leave the writer parked on an empty outbox for good: the
    // peer counted as connected, and nothing reading its connection.
    Harness harness;
    {
        std::scoped_lock const guard { harness.record->mutex };
        harness.record->writeBehindVerdict = LeaderAppend(2);
        harness.record->holdSessionOpen = true;
    }
    harness.sink.throwOnDeliver = true;
    harness.Start(PeerTransportOptions { .reconnectBackoff = 10s, .direction = RaftWire::SessionDirection::TwoWay });

    CHECK(harness.transport->ConnectedPeers() == 0);
    auto const lines = harness.logger.Snapshot();
    CHECK(std::ranges::any_of(lines, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains("sender threw");
    }));

    // And the sender lives on: it redials at the backoff, as after any ended session.
    harness.sink.throwOnDeliver = false;
    harness.clock.advance(10s);
    harness.reactor.drain();
    CHECK(harness.connector.Attempts() == 2);
    CHECK(harness.sink.received.size() == 1);
}

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Net/SocketDeadline.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace FastCache::Consensus
{

namespace
{
    /// Keeps one connection in `OpenConnections` for as long as it is being served.
    ///
    /// RAII rather than a call at each of the loop's exits, because the one that gets
    /// forgotten is a dangling pointer `Shutdown` would then close -- and a
    /// use-after-free at teardown is the shape of bug that reproduces once in a hundred
    /// runs and never on the machine looking at it.
    class RegisteredConnection
    {
      public:
        /// @param open Where to register; must outlive this.
        /// @param socket The connection.
        RegisteredConnection(OpenConnections* open, ISocket* socket) noexcept:
            _open { open },
            _socket { socket }
        {
            auto const guard = std::scoped_lock { _open->mutex };
            _open->sockets.push_back(_socket);
        }

        RegisteredConnection(RegisteredConnection const&) = delete;
        RegisteredConnection(RegisteredConnection&&) = delete;
        RegisteredConnection& operator=(RegisteredConnection const&) = delete;
        RegisteredConnection& operator=(RegisteredConnection&&) = delete;

        ~RegisteredConnection()
        {
            auto const guard = std::scoped_lock { _open->mutex };
            std::erase(_open->sockets, _socket);
        }

      private:
        OpenConnections* _open;
        ISocket* _socket;
    };

    /// A connection that has proved its id: who it proved, with which key, and the session
    /// its frames are bound to.
    struct ProvenPeer
    {
        NodeId dialler;             ///< The member the connection proved.
        Ed25519PublicKey provenKey; ///< The key it proved that with; re-checked on every frame.
        SessionKey session;         ///< What its frames are sealed under.
    };

    /// The tag after a session frame, copied out of what the reader returned.
    /// @param trailer Exactly `RaftWire::TagSize` bytes.
    /// @return The tag.
    [[nodiscard]] Sha256::Digest TagOf(std::span<std::byte const> trailer) noexcept
    {
        Sha256::Digest tag {};
        std::ranges::copy(trailer.first(std::min(trailer.size(), tag.size())), tag.begin());
        return tag;
    }
} // namespace

/// Grants the per-connection coroutines access to the server's privates.
///
/// Free functions taking raw pointers rather than members or lambdas, the shape
/// `RaftPeerTransport`'s `PeerSenderAccess` uses: a coroutine's frame outlives the
/// expression that created it, so a captured `this` is a lifetime question at every
/// suspension point rather than a documented one here. `Shutdown` drains before
/// returning, which is what makes the borrowed pointers safe.
struct PeerServerAccess
{
    /// Serve one accepted connection: the handshake, then its frames.
    /// @param self The server; outlives this by `Shutdown`'s drain.
    /// @param socket The accepted connection; owned for its lifetime.
    static DetachedTask ServePeer(RaftPeerServer* self, std::unique_ptr<ISocket> socket);

    /// Challenge, read one proof, judge it and answer, all within the handshake bound.
    /// @param self The server.
    /// @param socket The connection.
    /// @param reader The connection's reader, which the session goes on using.
    /// @param peer The address the connection came from.
    /// @return The proven peer, or nothing when the connection is not to be served.
    static Task<std::optional<ProvenPeer>> Handshake(RaftPeerServer* self,
                                                     ISocket* socket,
                                                     ByteReader* reader,
                                                     std::string peer);

    /// Read, check and deliver the frames of a connection that proved its id.
    /// @param self The server.
    /// @param reader The connection's reader.
    /// @param peer The address the connection came from.
    /// @param proven Who it proved, and its session.
    static Task<void> Serve(RaftPeerServer* self, ByteReader* reader, std::string peer, ProvenPeer proven);
};

DetachedTask PeerServerAccess::ServePeer(RaftPeerServer* self, std::unique_ptr<ISocket> socket)
{
    RegisteredConnection const registration { &self->_open, socket.get() };

    // No line is ever read on this wire, so the line cap is nominal. The payload cap
    // covers the largest read either phase makes -- a session frame and its tag -- and
    // the handshake's much smaller ceiling is checked against the declared length
    // before anything of that size is asked for.
    ByteReader reader { *socket,
                        /*maxLineBytes=*/1,
                        std::max(self->_options.maxFrameBytes + RaftWire::TagSize, RaftWire::MaxHandshakePayload) };
    auto peer = socket->peerAddress();

    if (auto proven = co_await Handshake(self, socket.get(), &reader, peer); proven.has_value())
        co_await Serve(self, &reader, std::move(peer), *std::move(proven));

    socket->close();
    self->_active.fetch_sub(1, std::memory_order_acq_rel);
}

Task<std::optional<ProvenPeer>> PeerServerAccess::Handshake(RaftPeerServer* self,
                                                            ISocket* socket,
                                                            ByteReader* reader,
                                                            std::string peer)
{
    // Armed before the first write, so the bound covers the whole exchange: a peer that
    // never reads the challenge stalls the write, and one that never proves stalls the
    // read, and both are the same stranger holding a slot. Expiry closes the socket,
    // which completes whichever of the two is parked -- and the flag is how an ending
    // is told apart from a peer that simply went away.
    SocketDeadlineTarget expiry { .socket = socket };
    auto deadline = ArmSocketDeadline(&self->_reactor, self->_options.handshakeBound, &expiry);

    // A connection that ended without refusing anything: gone, or out of time.
    auto const ended = [self, &expiry, &peer]() -> std::optional<ProvenPeer> {
        if (expiry.expired)
            self->NotePreAuthRefusal(AcceptorRefusal::HandshakeTimeout, peer, "");
        return std::nullopt;
    };

    // Drawn before anything is written: a connection this node cannot challenge with a fresh
    // nonce is one it must not challenge at all (#1527), so it is closed unchallenged.
    auto begun = AcceptorHandshake::Create(self->_identity, self->_random);
    if (!begun.has_value())
    {
        self->NoteNoNonce(peer, begun.error());
        co_return std::nullopt;
    }
    auto& handshake = *begun;

    // FIRST, before a byte is read. The challenge is what makes a proof unreplayable,
    // and it costs a stranger nothing it could use: a nonce is not signed by anything.
    auto const challenge = RaftWire::EncodeChallenge(handshake.Challenge());
    if (auto const written = co_await socket->write(challenge); !written.has_value() || *written != challenge.size())
        co_return ended();

    auto const headerBytes = co_await reader->ReadExactly(RaftWire::HeaderSize);
    if (!headerBytes.has_value())
        co_return ended();

    auto const header = RaftWire::DecodeHeader(*headerBytes);
    if (!header.has_value())
    {
        self->NotePreAuthRefusal(AcceptorRefusal::NoHandshake, peer, "it does not begin with this wire's magic");
        co_return std::nullopt;
    }

    // The type and the ceiling BEFORE the payload is buffered: this is the one read a
    // stranger chooses the size of, and checking afterwards would let it make this node
    // allocate what the ceiling exists to refuse.
    auto const* const row = RaftWire::FindMessage(header->kindRaw);
    if (row == nullptr || row->type != RaftWire::MessageType::Proof)
    {
        auto const seen = row != nullptr ? std::string { row->name } : std::format("type 0x{:02X}", header->kindRaw);
        self->NotePreAuthRefusal(
            AcceptorRefusal::NoHandshake,
            peer,
            std::format("its first frame is a {} at wire version {}, where this build expects a Proof at version {} -- a "
                        "build from before the handshake sends a Raft message first",
                        seen,
                        unsigned { header->version },
                        unsigned { RaftWire::CurrentVersion }));
        co_return std::nullopt;
    }
    if (header->payloadLength > row->phase.Ceiling())
    {
        self->NotePreAuthRefusal(AcceptorRefusal::NoHandshake,
                                 peer,
                                 std::format("its proof declares {} bytes, over the {}-byte ceiling",
                                             header->payloadLength,
                                             row->phase.Ceiling()));
        co_return std::nullopt;
    }

    auto const payload = co_await reader->ReadExactly(header->payloadLength);
    if (!payload.has_value())
        co_return ended();

    auto const proof = RaftWire::DecodeProof(*header, *payload);
    if (!proof.has_value())
    {
        self->NotePreAuthRefusal(AcceptorRefusal::NoHandshake, peer, proof.error().context);
        co_return std::nullopt;
    }

    auto judgement = handshake.Judge(*proof);

    // Answered with nothing: a verdict exists only for a proof whose signature verified. The
    // claimed id is not printed for either -- it is exactly what nobody proved.
    if (!judgement.verdict.has_value())
    {
        self->NotePreAuthRefusal(
            judgement.outcome == ProofOutcome::UnknownKey ? AcceptorRefusal::UnknownKey : AcceptorRefusal::Proof, peer, "");
        co_return std::nullopt;
    }

    // Every outcome from here is about a peer whose signature verified, so each is answered
    // SIGNED -- including the refusals, which a bare close would leave the dialler to report as
    // its key being unknown here (#1308, A1).
    auto const verdict = RaftWire::EncodeVerdict(*judgement.verdict);
    auto const written = co_await socket->write(verdict);
    auto const sent = written.has_value() && *written == verdict.size();

    switch (judgement.outcome)
    {
        case ProofOutcome::WrongTarget:
            self->NoteProvenRefusal(AcceptorRefusal::WrongTarget,
                                    peer,
                                    judgement.dialler,
                                    std::format("it dialled {} and this node is {}", proof->target, self->_identity.Self()));
            co_return std::nullopt;
        case ProofOutcome::OwnId:
            self->NoteProvenRefusal(AcceptorRefusal::OwnId, peer, judgement.dialler, "");
            co_return std::nullopt;
        case ProofOutcome::RevokedKey:
            // The key is named, whole: it is what the signature proved, and it is what an
            // operator matches against the revocation they made.
            self->NotePreAuthRefusal(AcceptorRefusal::RevokedKey,
                                     peer,
                                     std::format("the key is {}", FormatEd25519PublicKey(judgement.provenKey)));
            co_return std::nullopt;
        case ProofOutcome::Forged:
        case ProofOutcome::UnknownKey:
            // Handled above; named so a new outcome is a compile error rather than a
            // connection served by accident.
            co_return std::nullopt;
        case ProofOutcome::Accepted:
            break;
    }

    if (!sent || !judgement.session.has_value())
        co_return ended();

    // Disarmed only now: the bound is on proving an id, and a member that has proved one is
    // held to the same no-deadline stream every peer connection always was.
    deadline.reset();
    co_return ProvenPeer { .dialler = std::move(judgement.dialler),
                           .provenKey = judgement.provenKey,
                           .session = *std::move(judgement.session) };
}

Task<void> PeerServerAccess::Serve(RaftPeerServer* self, ByteReader* reader, std::string peer, ProvenPeer proven)
{
    FrameOpener opener { std::move(proven.session) };

    while (true)
    {
        auto const headerBytes = co_await reader->ReadExactly(RaftWire::HeaderSize);
        if (!headerBytes.has_value())
            break; // EOF, or the peer went away. Ordinary.

        auto const header = RaftWire::DecodeHeader(*headerBytes);
        if (!header.has_value())
        {
            // A wrong magic is the one condition under which the reader cannot find
            // where this frame ends. There is nothing to resynchronize to, so every
            // later byte would be a guess.
            self->_logger.Log(
                LogLevel::Warn,
                std::format("raft: peer {} ({}) sent a frame with no valid magic; closing", proven.dialler, peer));
            break;
        }

        if (header->payloadLength > self->_options.maxFrameBytes)
        {
            // Refused BEFORE the payload is buffered, exactly as the compile-cache
            // handler's cap is: checking afterwards would let a peer force the very
            // allocation the cap exists to deny, once per frame.
            self->_logger.Log(LogLevel::Warn,
                              std::format("raft: peer {} ({}) declared a {}-byte frame over the {}-byte cap; closing",
                                          proven.dialler,
                                          peer,
                                          header->payloadLength,
                                          self->_options.maxFrameBytes));
            break;
        }

        auto const body = co_await reader->ReadExactly(std::size_t { header->payloadLength } + RaftWire::TagSize);
        if (!body.has_value())
            break;

        auto const bytes = std::span<std::byte const> { *body };
        auto const payload = bytes.first(header->payloadLength);

        // The tag BEFORE anything the frame says is acted on -- before its type decides
        // whether it is stepped over, and before its message reaches the node.
        if (!opener.Open(*headerBytes, payload, TagOf(bytes.last(RaftWire::TagSize))))
        {
            self->NoteProvenRefusal(AcceptorRefusal::FrameTag, peer, proven.dialler, "");
            break;
        }

        // The roster, asked again for every frame: a key revoked since the handshake ends the
        // connection here, at the first frame after the decision, rather than whenever the
        // connection happens to break. Asked after the tag, so a frame nobody sealed is counted
        // as what it is.
        if (!self->_identity.StillProves(proven.dialler, proven.provenKey))
        {
            self->NoteProvenRefusal(AcceptorRefusal::KeyWithdrawn, peer, proven.dialler, "");
            break;
        }

        auto decoded = RaftWire::DecodeMessage(*header, payload);
        if (decoded.has_value())
        {
            // The member the connection proved is the one that speaks on it. A message
            // naming another sender is refused rather than delivered under that name:
            // everything the node decides about a sender -- whom it votes for, whose
            // entries it takes -- is then a fact about the key, not about a field.
            if (auto const& sender = SenderOf(*decoded); sender != proven.dialler)
            {
                self->NoteProvenRefusal(
                    AcceptorRefusal::FrameSender, peer, proven.dialler, std::format("the message named {}", sender));
                break;
            }

            self->_sink.Deliver(*std::move(decoded));
            self->_delivered.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // The payload and its tag have been consumed and verified, so the reader is still
        // in sync -- which is the whole point of the declared length.
        if (decoded.error().code == ConsensusErrorCode::UnknownMessageType)
        {
            // Stepped over, not fatal. A peer running a newer build is the ordinary
            // condition during a rolling upgrade, and closing here would partition this
            // node from every peer ahead of it.
            self->_skipped.fetch_add(1, std::memory_order_relaxed);
            self->_logger.Log(
                LogLevel::Debug,
                std::format("raft: skipped a frame from peer {} ({}): {}", proven.dialler, peer, decoded.error().context));
            continue;
        }

        // A malformed payload, a handshake frame out of place, or a version other than
        // the one the handshake settled: this reader and that sender disagree about the
        // bytes, so the connection is no longer trustworthy even though this frame was
        // consumed cleanly.
        self->_logger.Log(LogLevel::Warn,
                          std::format("raft: peer {} ({}) sent a frame this node cannot read ({}); closing",
                                      proven.dialler,
                                      peer,
                                      decoded.error().context));
        break;
    }
}

RaftPeerServer::RaftPeerServer(IListener& listener,
                               IReactor& reactor,
                               IRaftMessageSink& sink,
                               ILogger& logger,
                               IMetricsSink& metrics,
                               IRaftPeerIdentity const& identity,
                               ISecureRandom& random,
                               PeerServerOptions options):
    _listener { listener },
    _reactor { reactor },
    _sink { sink },
    _logger { logger },
    _metrics { metrics },
    _identity { identity },
    _random { random },
    _options { options }
{
}

void RaftPeerServer::NotePreAuthRefusal(AcceptorRefusal refusal, std::string_view peer, std::string_view detail)
{
    auto const& row = RowFor(refusal);
    _metrics.Increment(row.counter);

    {
        auto const guard = std::scoped_lock { _reportMutex };
        auto const now = _reactor.clock().now();
        if (now < _nextPreAuthReport)
            return;
        _nextPreAuthReport = now + PreAuthReportInterval;
    }

    _logger.Log(LogLevel::Warn,
                std::format("raft: refused a peer connection from {} because {}{}{} (every refusal is counted; this line "
                            "repeats at most once a minute)",
                            peer,
                            row.says,
                            detail.empty() ? "" : ": ",
                            detail));
}

void RaftPeerServer::NoteNoNonce(std::string_view peer, SecureRandomError const& error)
{
    {
        auto const guard = std::scoped_lock { _reportMutex };
        auto const now = _reactor.clock().now();
        if (now < _nextNoNonceReport)
            return;
        _nextNoNonceReport = now + PreAuthReportInterval;
    }

    _logger.Log(LogLevel::Error,
                std::format("raft: closed the peer connection from {} without challenging it, because this node "
                            "cannot draw a handshake nonce: {}. Every peer connection fails this way until it can; "
                            "the fault is this host's, not the peer's (this line repeats at most once a minute)",
                            peer,
                            error.ToString()));
}

void RaftPeerServer::NoteProvenRefusal(AcceptorRefusal refusal,
                                       std::string_view peer,
                                       std::string_view dialler,
                                       std::string_view detail)
{
    // Not throttled: only the member that proved its id can provoke one, so the log cannot
    // be filled from outside, and each is a member worth naming every time.
    auto const& row = RowFor(refusal);
    _metrics.Increment(row.counter);
    _logger.Log(
        LogLevel::Warn,
        std::format(
            "raft: refused peer {} at {} because {}{}{}", dialler, peer, row.says, detail.empty() ? "" : ": ", detail));
}

Task<void> RaftPeerServer::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            // A poll timeout is how this loop wakes to observe Shutdown() on
            // POSIX, where closing the listening socket does not unblock a
            // parked accept(). Not a failure.
            if (IsDeadlineExpiry(accepted.error().code))
                continue;
            _logger.Log(LogLevel::Debug, std::format("raft: peer accept loop ended ({})", accepted.error().ToString()));
            co_return;
        }

        auto const before = _active.fetch_add(1, std::memory_order_acq_rel);
        if (before >= _options.maxConnections)
        {
            // Closed rather than answered: nothing has been proved, so there is nobody
            // to sign a verdict for. A peer whose connection is refused redials on its
            // own backoff.
            _active.fetch_sub(1, std::memory_order_acq_rel);
            NotePreAuthRefusal(AcceptorRefusal::Full, (*accepted)->peerAddress(), "");
            (*accepted)->close();
            continue;
        }

        // Detached, because peer connections are long-lived and concurrent.
        // Serving them inline -- which is right for the compile worker, whose
        // connections are one CPU-bound job each -- would mean reading only one
        // peer ever, and a cluster that never hears from the rest.
        PeerServerAccess::ServePeer(this, std::move(*accepted));
    }
    co_return;
}

void RaftPeerServer::CloseAll() noexcept
{
    _listener.Close();

    // Copied out under the lock rather than closed under it, because a close
    // resumes the task that removes itself from this very vector.
    auto sockets = std::vector<ISocket*> {};
    {
        auto const guard = std::scoped_lock { _open.mutex };
        sockets = _open.sockets;
    }
    for (auto* socket: sockets)
        socket->close();
}

void RaftPeerServer::Shutdown() noexcept
{
    if (_shuttingDown.exchange(true, std::memory_order_acq_rel))
        return;

    // Posted onto the reactor, never done here. See the declaration: on epoll and
    // kqueue `Close` completes a parked read by resuming its coroutine INLINE, so
    // closing from the stopping thread runs this server's connection tasks there --
    // and destroys the socket each of them owns off the reactor, which is the
    // teardown rule (#668) violated at a second owner (#885).
    //
    // Borrows `this` rather than sharing state, which is sound for the same reason
    // the connection tasks may: the drain below does not return until this has run.
    [](RaftPeerServer* self) -> DetachedTask {
        co_await ResumeOn { self->_reactor };
        self->CloseAll();
        self->_closesRan.store(true, std::memory_order_release);
        co_return;
    }(this);

    // Detached connection coroutines borrow the sink, the logger and the
    // counters held on this object, so they must drain before Shutdown returns
    // -- otherwise a reader suspended on a slow socket could touch freed members
    // after the server is destroyed. Bounded rather than unconditional: a stuck
    // peer must not turn a stop into a hang, which is how a service ends up
    // killed by its supervisor instead of stopping.
    //
    // This wait was correct, and was then copied twice by loops that cited it and
    // counted their polls instead of measuring them. So it is now the shared
    // `DrainWithin`, and the ceiling and the cadence live there with it (#452).
    //
    // It waits for the posted closes AS WELL, and that half is what makes borrowing
    // `this` above safe: nothing may free this object while a task holding it is
    // still queued. It is also the only thing that ends the wait at all -- until
    // the listener is closed the accept loop keeps running and no connection is
    // told to finish.
    auto const outcome = DrainWithin(
        [this] { return !_closesRan.load(std::memory_order_acquire) || _active.load(std::memory_order_acquire) > 0; });

    if (outcome != DrainResult::Ceiling)
        return;

    // Named separately, because they are opposite diagnoses fixed by different
    // people: connections outstanding is a peer that will not finish, while closes
    // that never ran is a reactor that stopped before it could run what it was
    // handed -- and reporting the second as the first sends somebody hunting a
    // slow peer that does not exist.
    if (!_closesRan.load(std::memory_order_acquire))
        _logger.Log(LogLevel::Error,
                    "raft: the peer listener's closes never ran within the stop ceiling; the reactor was not turning");
    if (auto const stuck = _active.load(std::memory_order_acquire); stuck > 0)
        _logger.Log(LogLevel::Error,
                    std::format("raft: {} peer connection(s) did not finish within the stop ceiling", stuck));
}

} // namespace FastCache::Consensus

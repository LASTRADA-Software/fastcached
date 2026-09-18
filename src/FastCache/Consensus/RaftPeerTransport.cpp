// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/InterruptibleSleep.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Net/SocketDeadline.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Consensus
{

namespace
{

    /// Write every byte of `bytes` to `socket`.
    ///
    /// A coroutine because `ISocket::Write` is awaitable, awaited by the peer's
    /// sender on the reactor. It used to be driven with `SyncRun`, which was sound
    /// only because the socket was a blocking one; now it really suspends, which
    /// is what makes a write completable by closing the socket rather than only by
    /// the peer reading it.
    ///
    /// The socket arrives by **pointer rather than reference**, which reads as a
    /// stylistic slip and is not: a coroutine's reference parameter is bound
    /// before the first suspension and then outlives every frame that could have
    /// kept it alive. `RaftDriver::Run` takes its reactor the same way, for the
    /// same reason.
    ///
    /// `ISocket::Write` is write-all by contract -- "Resolves with the byte count
    /// actually written (== buffer.size() on success)" -- so the count is checked
    /// rather than looped over. A resume loop here would be dead code that reads as
    /// a statement about the interface, and a reader who believed it would write
    /// partial-write handling at every other call site too; the three that already
    /// exist just check the count.
    /// @param socket The connected peer socket; never null.
    /// @param bytes The framed message.
    /// @return Whether every byte reached the socket.
    [[nodiscard]] Task<bool> WriteFrame(ISocket* socket, std::span<std::byte const> bytes)
    {
        auto const written = co_await socket->Write(bytes);
        co_return written.has_value() && *written == bytes.size();
    }

    /// What reading one handshake frame produced.
    struct HandshakeFrameRead
    {
        /// How the read ended.
        enum class Outcome : std::uint8_t
        {
            Read,       ///< A frame of the expected type, within its ceiling.
            Ended,      ///< The connection ended first, or was closed under the read.
            Unreadable, ///< Something arrived that is not the expected handshake frame.
        };

        Outcome outcome { Outcome::Ended }; ///< How the read ended.
        RaftWire::FrameHeader header {};    ///< The frame's header, when read.
        std::vector<std::byte> payload;     ///< The frame's payload, when read.
        std::string detail;                 ///< What was seen instead, when unreadable.
    };

    /// Read one handshake frame of type @p Type from an acceptor.
    ///
    /// The type and the ceiling are checked BEFORE the payload is read, for the reason
    /// the acceptor checks them before reading a proof: the declared length is the one
    /// size the other end chooses.
    /// @tparam Type The handshake frame this step expects.
    /// @param reader The connection's reader; never null.
    /// @return What the read produced.
    template <RaftWire::MessageType Type>
    [[nodiscard]] Task<HandshakeFrameRead> ReadHandshakeFrame(ByteReader* reader)
    {
        using Outcome = HandshakeFrameRead::Outcome;
        auto const& expected = RaftWire::Detail::RowOf<Type>;

        auto const headerBytes = co_await reader->ReadExactly(RaftWire::HeaderSize);
        if (!headerBytes.has_value())
            co_return HandshakeFrameRead { .outcome = Outcome::Ended, .header = {}, .payload = {}, .detail = {} };

        auto const header = RaftWire::DecodeHeader(*headerBytes);
        if (!header.has_value())
            co_return HandshakeFrameRead { .outcome = Outcome::Unreadable,
                                           .header = {},
                                           .payload = {},
                                           .detail = "it does not speak this wire: no valid magic" };

        auto const* const row = RaftWire::FindMessage(header->kindRaw);
        if (row == nullptr || row->type != Type)
        {
            auto const seen = row != nullptr ? std::string { row->name } : std::format("type 0x{:02X}", header->kindRaw);
            co_return HandshakeFrameRead { .outcome = Outcome::Unreadable,
                                           .header = *header,
                                           .payload = {},
                                           .detail = std::format("it sent a {} at wire version {} where this build "
                                                                 "expects a {} at version {}",
                                                                 seen,
                                                                 unsigned { header->version },
                                                                 expected.name,
                                                                 unsigned { RaftWire::CurrentVersion }) };
        }
        if (header->payloadLength > row->phase.Ceiling())
            co_return HandshakeFrameRead { .outcome = Outcome::Unreadable,
                                           .header = *header,
                                           .payload = {},
                                           .detail = std::format("its {} declares {} bytes, over the {}-byte ceiling",
                                                                 row->name,
                                                                 header->payloadLength,
                                                                 row->phase.Ceiling()) };

        auto payload = co_await reader->ReadExactly(header->payloadLength);
        if (!payload.has_value())
            co_return HandshakeFrameRead { .outcome = Outcome::Ended, .header = *header, .payload = {}, .detail = {} };

        co_return HandshakeFrameRead {
            .outcome = Outcome::Read, .header = *header, .payload = *std::move(payload), .detail = {}
        };
    }

    /// An acceptor that proved its id and took this node: the key it proved that with, and the
    /// session its frames go out under.
    struct ProvenAcceptor
    {
        Ed25519PublicKey provenKey; ///< Re-checked against the roster before every frame.
        SessionKey session;         ///< What this end's frames are sealed under.
    };

} // namespace

/// Grants the free-function senders access to the transport's privates.
///
/// The senders are free functions taking raw pointers rather than members or
/// lambdas: a coroutine's frame outlives the expression that created it, so
/// capturing `this` is a use-after-free waiting to happen -- the shape
/// `RaftPeerServer::ServePeer` already uses for the same reason.
struct PeerSenderAccess
{
    /// How one attempt at serving a peer ended.
    enum class Outcome : std::uint8_t
    {
        Retry, ///< Dial or write failed; back off and try again.
        Stop,  ///< The transport is shutting down.
    };

    /// Holds one peer in the connected count for as long as its session lasts.
    ///
    /// RAII rather than a decrement at each of the session's exits, because the
    /// one that gets forgotten makes `ConnectedPeers()` report a fleet talking to
    /// peers it is not talking to -- and a readiness probe reading that is a node
    /// that looks healthy and is not. A coroutine frame's locals are destroyed
    /// when the frame completes or is destroyed, so the count is right on every
    /// path including the ones nobody wrote.
    class Session
    {
      public:
        /// @param transport Whose counter to hold.
        /// @param where Which peer and where it was reached, for the log lines. A
        ///        snapshot the caller already holds rather than the `Peer` itself,
        ///        because that peer's address is guarded by `_peersMutex` and this
        ///        is not the place to take it. Only the id outlives this
        ///        constructor: the address is a fact about the session that has
        ///        just begun, and by the time it ends it may no longer be where
        ///        that peer answers.
        Session(RaftPeerTransport* transport, PeerEndpoint const& where) noexcept:
            _transport { transport },
            _peerId { where.id }
        {
            _transport->_connected.fetch_add(1, std::memory_order_relaxed);
            _transport->_logger.Log(LogLevel::Info,
                                    std::format("raft: connected to peer {} at {}:{}", where.id, where.host, where.port));
        }

        Session(Session const&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session const&) = delete;
        Session& operator=(Session&&) = delete;

        ~Session()
        {
            _transport->_connected.fetch_sub(1, std::memory_order_relaxed);
            _transport->_logger.Log(LogLevel::Info, std::format("raft: peer {} disconnected", _peerId));
        }

      private:
        RaftPeerTransport* _transport;
        NodeId _peerId;
    };

    /// One connection's life: dial, prove this node's id, serve, end.
    static Task<Outcome> ServeOnce(RaftPeerTransport* self, RaftPeerTransport::Peer* peer);

    /// Prove this node's id to the acceptor at `peer->socket`, and check its verdict, within
    /// the handshake bound.
    /// @param self The transport.
    /// @param peer The peer being dialled; its socket is connected.
    /// @param where Where it was dialled, for the log line.
    /// @return The key the acceptor proved itself with and the session's key, when it
    ///         accepted this node as the member it dialled; nothing otherwise, the refusal
    ///         already counted.
    static Task<std::optional<ProvenAcceptor>> Handshake(RaftPeerTransport* self,
                                                         RaftPeerTransport::Peer* peer,
                                                         PeerEndpoint where);

    /// One peer's whole life. Ends only on a stop.
    static Task<void> RunSender(RaftPeerTransport* self, RaftPeerTransport::Peer* peer);

    /// Close live peer sockets, on the reactor's thread.
    ///
    /// One peer or all of them: a shutdown ends every session, and a peer that has
    /// moved needs exactly its own dropped so its sender redials the new address.
    /// One function rather than two, because what makes this correct is the thread
    /// it runs on and that argument is the same either way.
    /// @param self The transport.
    /// @param only Which peer, or nullopt for every one.
    static DetachedTask CloseSockets(RaftPeerTransport* self, std::optional<NodeId> only);
};

Task<PeerSenderAccess::Outcome> PeerSenderAccess::ServeOnce(RaftPeerTransport* self, RaftPeerTransport::Peer* peer)
{
    // No I/O bound on the socket, which is unchanged and deliberate: it decides
    // when a peer is declared dead, and that is its own decision rather than a
    // side effect of dialling. What is new is that it is no longer dangerous --
    // a write with no timeout used to be uninterruptible, and now closing the
    // socket completes it.
    //
    // Read once, before the dial, and used for the whole session: a peer that
    // moves mid-session has its socket closed under it, so the session ends and the
    // next one reads the new address here.
    auto const where = self->AddressOf(*peer);

    auto dialed = co_await self->_connector.Connect(
        where.host, where.port, DialOptions { .connectTimeout = self->_options.dialTimeout });
    if (!dialed.has_value())
    {
        // Debug, not Warn. A peer being down is the ordinary condition Raft is
        // built for, and one line per backoff interval per peer at Warn would
        // bury the messages that do need reading.
        self->_logger.Log(
            LogLevel::Debug,
            std::format(
                "raft: could not reach peer {} at {}:{}: {}", where.id, where.host, where.port, dialed.error().context));
        co_return Outcome::Retry;
    }

    // A stop that arrived while the dial was in flight. The dial cannot be
    // abandoned -- destroying a suspended task frees a frame the reactor still
    // points into -- so it is always awaited to completion and the socket it
    // produced is closed here.
    if (self->_stop.Token().IsCancelled())
    {
        dialed.value()->Close();
        co_return Outcome::Stop;
    }

    peer->socket = std::move(dialed.value());

    // A re-address that landed while this dial was in flight closed a socket that
    // did not exist yet, so it closed nothing -- and this connection is to the
    // address the peer has just stopped answering on. Nothing else would notice:
    // the session below is long-lived and never re-reads the address, so the peer
    // would be served at its old address until that connection happened to break.
    if (auto const current = self->AddressOf(*peer); current.host != where.host || current.port != where.port)
    {
        peer->socket->Close();
        peer->socket.reset();
        co_return Outcome::Retry;
    }

    // Nothing is sent to an address until what answers there has proved the member's id and
    // accepted this node as the member it dialled -- and until then the peer is not
    // CONNECTED either, so `ConnectedPeers()` counts authenticated sessions only.
    auto proven = co_await Handshake(self, peer, where);
    if (!proven.has_value())
    {
        peer->socket->Close();
        peer->socket.reset();
        co_return Outcome::Retry;
    }

    Session const session { self, where };
    FrameSealer sealer { std::move(proven->session) };

    while (true)
    {
        auto frame = co_await peer->outbox.Pop();
        if (!frame.has_value())
            co_return Outcome::Stop; // the outbox was closed

        // The roster, asked again before every frame: a key revoked since the handshake ends
        // the session here, and the redial is judged against the roster as it is now. The
        // frame is dropped with the session, which Raft already survives for any message.
        if (!self->_identity.StillProves(where.id, proven->provenKey))
        {
            self->NoteDialRefusal(*peer, where, DiallerRefusal::KeyWithdrawn, "");
            self->_dropped.fetch_add(1, std::memory_order_relaxed);
            peer->socket->Close();
            peer->socket.reset();
            co_return Outcome::Retry;
        }

        // Sealed HERE rather than when queued, because a frame's tag is bound to the
        // connection it goes out on and to its position there -- and a queued frame
        // outlives the connection that was current when it was framed. Appended to the
        // frame so it goes out in the one write `ISocket::Write`'s contract makes whole.
        auto const bytes = std::span<std::byte const> { *frame };
        auto const headerSize = std::min(bytes.size(), RaftWire::HeaderSize);
        auto const tag = sealer.Seal(bytes.first(headerSize), bytes.subspan(headerSize));
        frame->insert(frame->end(), tag.begin(), tag.end());

        // Written from a local of this frame, never from a queue element:
        // `ISocket::Write` requires the buffer to stay at a stable address until
        // the awaitable resumes, and a deque's element addresses do not.
        if (!co_await WriteFrame(peer->socket.get(), *frame))
        {
            self->_dropped.fetch_add(1, std::memory_order_relaxed);
            peer->socket->Close();
            peer->socket.reset();
            co_return Outcome::Retry;
        }
    }
}

Task<std::optional<ProvenAcceptor>> PeerSenderAccess::Handshake(RaftPeerTransport* self,
                                                                RaftPeerTransport::Peer* peer,
                                                                PeerEndpoint where)
{
    using ReadOutcome = HandshakeFrameRead::Outcome;
    auto* const socket = peer->socket.get();

    // Armed before the first read, so the bound covers the whole exchange: an acceptor
    // that never challenges -- a build from before the handshake, which only ever reads --
    // and one that never answers the proof are both a connection this sender would
    // otherwise sit on forever, with nothing queued for it reaching anybody.
    SocketDeadlineTarget expiry { .socket = socket };
    auto deadline = ArmSocketDeadline(&self->_reactor, self->_options.handshakeBound, &expiry);

    // The handshake frames and nothing larger: this reader is abandoned at the verdict,
    // since nothing an acceptor sends after it is read.
    ByteReader reader { *socket, /*maxLineBytes=*/1, RaftWire::MaxHandshakePayload };

    auto const challengeRead = co_await ReadHandshakeFrame<RaftWire::MessageType::Challenge>(&reader);
    if (challengeRead.outcome != ReadOutcome::Read)
    {
        if (expiry.expired)
            self->NoteDialRefusal(*peer, where, DiallerRefusal::Timeout, "");
        else
            self->NoteDialRefusal(*peer,
                                  where,
                                  DiallerRefusal::NoChallenge,
                                  challengeRead.outcome == ReadOutcome::Unreadable ? challengeRead.detail
                                                                                   : "it closed before sending one");
        co_return std::nullopt;
    }

    auto const challenge = RaftWire::DecodeChallenge(challengeRead.header, challengeRead.payload);
    if (!challenge.has_value())
    {
        self->NoteDialRefusal(*peer, where, DiallerRefusal::NoChallenge, challenge.error().context);
        co_return std::nullopt;
    }

    // Both failures below are faults of THIS node rather than of the peer, so neither is a
    // dial refusal and neither moves a peer counter -- but each is every connection to every
    // peer, so it is said, at most once per interval per peer. A nonce that cannot be drawn
    // abandons the connection rather than proving with a weak one (#1527).
    auto begun = DiallerHandshake::Create(self->_identity, where.id, self->_random);
    if (!begun.has_value())
    {
        self->NoteOwnFault(
            *peer, where, std::format("this node cannot draw a handshake nonce: {}", begun.error().ToString()));
        co_return std::nullopt;
    }
    auto& dialling = *begun;

    auto const proof = dialling.Answer(*challenge);
    if (!proof.has_value())
    {
        self->NoteOwnFault(*peer, where, proof.error());
        co_return std::nullopt;
    }

    if (!co_await WriteFrame(socket, RaftWire::EncodeProof(*proof)))
    {
        self->NoteDialRefusal(*peer, where, expiry.expired ? DiallerRefusal::Timeout : DiallerRefusal::EndedByAcceptor, "");
        co_return std::nullopt;
    }

    auto const verdictRead = co_await ReadHandshakeFrame<RaftWire::MessageType::Verdict>(&reader);
    if (verdictRead.outcome == ReadOutcome::Ended)
    {
        // EOF after the proof with no verdict is how an acceptor refuses what it cannot
        // sign: a proof whose signature it could not verify -- it holds no key for this
        // node's id, or another one -- or one it could not read. A refusal it COULD sign
        // arrives as a verdict below.
        self->NoteDialRefusal(*peer, where, expiry.expired ? DiallerRefusal::Timeout : DiallerRefusal::EndedByAcceptor, "");
        co_return std::nullopt;
    }

    // Anything but a verdict that verifies is an acceptor this node cannot tell is the member
    // it claims to be, which is all an unverifiable answer can mean.
    auto const verdict =
        verdictRead.outcome == ReadOutcome::Read
            ? RaftWire::DecodeVerdict(verdictRead.header, verdictRead.payload)
            : std::expected<RaftWire::VerdictFrame, ConsensusError> { std::unexpect,
                                                                      MalformedWireFrame(verdictRead.detail) };
    if (!verdict.has_value())
    {
        self->NoteDialRefusal(*peer, where, DiallerRefusal::AcceptorProof, verdict.error().context);
        co_return std::nullopt;
    }

    auto conclusion = dialling.Conclude(*verdict);
    switch (conclusion.outcome)
    {
        case VerdictOutcome::Accepted:
            // `Conclude` agrees the key before it answers Accepted, so this is never absent;
            // were it ever, it is an acceptance this end cannot act on, counted as one.
            if (!conclusion.session.has_value())
            {
                self->NoteDialRefusal(*peer, where, DiallerRefusal::AcceptorProof, "");
                co_return std::nullopt;
            }
            // Disarmed only now: the bound is on proving an id, and a session after it is
            // the unbounded stream a peer connection always was.
            deadline.reset();
            co_return ProvenAcceptor { .provenKey = conclusion.provenKey, .session = *std::move(conclusion.session) };
        case VerdictOutcome::Forged:
            self->NoteDialRefusal(*peer, where, DiallerRefusal::AcceptorProof, "");
            co_return std::nullopt;
        case VerdictOutcome::AcceptorKeyUnknown:
            self->NoteDialRefusal(
                *peer, where, DiallerRefusal::AcceptorKeyUnknown, std::format("{} answered there", verdict->acceptor));
            co_return std::nullopt;
        case VerdictOutcome::AcceptorKeyRevoked:
            self->NoteDialRefusal(
                *peer, where, DiallerRefusal::AcceptorKeyRevoked, std::format("{} answered there", verdict->acceptor));
            co_return std::nullopt;
        case VerdictOutcome::OwnKeyRevoked:
            self->NoteDialRefusal(*peer, where, DiallerRefusal::OwnKeyRevoked, "");
            co_return std::nullopt;
        case VerdictOutcome::WrongTarget:
            self->NoteDialRefusal(
                *peer,
                where,
                DiallerRefusal::WrongTarget,
                std::format("{} answered there, where this node dialled {}", conclusion.acceptor, where.id));
            co_return std::nullopt;
        case VerdictOutcome::OwnId:
            self->NoteDialRefusal(*peer, where, DiallerRefusal::OwnId, "");
            co_return std::nullopt;
    }
    co_return std::nullopt;
}

Task<void> PeerSenderAccess::RunSender(RaftPeerTransport* self, RaftPeerTransport::Peer* peer)
{
    self->_reactorThread.store(std::this_thread::get_id(), std::memory_order_relaxed);

    auto const token = self->_stop.Token();
    while (!token.IsCancelled())
    {
        auto outcome = Outcome::Retry;
        try
        {
            outcome = co_await ServeOnce(self, peer);
        }
        catch (...)
        {
            // The hazard inverts rather than disappearing. With a `DetachedTask`
            // an escaped exception is std::terminate -- a node killed over one
            // peer. With an unawaited `Task<void>` it is stored in the promise
            // and never read, so the sender would simply end, forever, with
            // nothing logged anywhere: "three nodes sat at undecided with no
            // error" spelled for one peer. So it is caught and reported.
            self->NoteSenderThrew(peer->endpoint.id);
        }

        if (outcome == Outcome::Stop || token.IsCancelled())
            break;

        // Backed off after EVERY session, not only a failed dial. The thread
        // version redialled immediately after a dropped connection, so a peer
        // that accepts and instantly resets produced a tight
        // connect/write-fail/connect loop with no sleep in it. On a private
        // thread that burned one core; on the shared reactor it starves the
        // election timers, which is the "nine role changes in twelve seconds"
        // failure this repository already has a name for.
        if (co_await InterruptibleSleepUntil(&self->_reactor,
                                             token,
                                             self->_reactor.Clock().Now() + self->_options.reconnectBackoff,
                                             self->_options.stopWakeBound)
            == WakeReason::Cancelled)
            break;
    }

    peer->socket.reset();
    self->NoteSenderFinished();
    co_return;
}

DetachedTask PeerSenderAccess::CloseSockets(RaftPeerTransport* self, std::optional<NodeId> only)
{
    // The hop is the point. On epoll and kqueue `ISocket::Close` completes a
    // parked awaitable by resuming its coroutine INLINE, so closing from the
    // thread calling Stop() would run a peer's sender there while the reactor
    // thread is live. IOCP routes cancellation back through the port and does
    // not, which is precisely what would make this a bug that passes CI on
    // Windows and corrupts state on Linux and macOS.
    co_await ResumeOn { self->_reactor };

    // Collected under the lock, closed outside it, and the split is the point.
    // `Close` resumes a parked sender INLINE on epoll and kqueue -- on this thread,
    // which is the whole reason for the hop above -- so a lock held across it is a
    // lock held across arbitrary sender code, with `Send` and therefore the driver's
    // mutex waiting behind it. `Peer::socket` is reactor-thread-only, so nothing but
    // the map lookup needs the lock at all.
    auto closing = std::vector<ISocket*> {};
    {
        auto const guard = std::shared_lock { self->_peersMutex };
        if (only.has_value())
        {
            if (auto const found = self->_peers.find(*only); found != self->_peers.end())
                closing.push_back(found->second->socket.get());
        }
        else
        {
            closing.reserve(self->_peers.size());
            for (auto& [id, peer]: self->_peers)
                closing.push_back(peer->socket.get());
        }
    }

    for (auto* const socket: closing)
        if (socket != nullptr)
            socket->Close();
    co_return;
}

RaftPeerTransport::RaftPeerTransport(std::vector<PeerEndpoint> peers,
                                     IReactor& reactor,
                                     IConnector& connector,
                                     ILogger& logger,
                                     IMetricsSink& metrics,
                                     IRaftPeerIdentity const& identity,
                                     ISecureRandom& random,
                                     PeerTransportOptions options):
    _identity { identity },
    _self { identity.Self() },
    _reactor { reactor },
    _connector { connector },
    _logger { logger },
    _metrics { metrics },
    _random { random },
    _options { options }
{
    for (auto& endpoint: peers)
        Learn(std::move(endpoint));
}

PeerChange RaftPeerTransport::Learn(PeerEndpoint where)
{
    if (where.id == _self)
        // A node does not dial itself; `Send` refuses a message addressed here for
        // the same reason. Answering rather than silently skipping means a caller
        // handing over a whole member set does not have to filter it, and the rule
        // stays in one place.
        return PeerChange::Self;

    auto const id = where.id;
    {
        auto const guard = std::unique_lock { _peersMutex };

        // A peer added after a stop would have a sender nobody ever finishes. This
        // and the submission below read the one value under the one lock, which is
        // what keeps a peer learned around `Start` or `RequestStop` from being
        // dialled twice or never dialled at all.
        if (_lifecycle == Lifecycle::Stopping)
            return PeerChange::Stopping;

        auto const found = _peers.find(id);
        if (found == _peers.end())
        {
            auto const inserted =
                _peers.emplace(id,
                               std::make_unique<Peer>(std::move(where),
                                                      _reactor,
                                                      AsyncQueueOptions { .capacity = _options.maxQueuedPerPeer,
                                                                          .overflow = AsyncQueueOverflow::DropOldest }));
            if (_lifecycle == Lifecycle::Running)
                StartSender(*inserted.first->second);

            return PeerChange::Added;
        }

        auto& peer = *found->second;
        if (peer.endpoint.host == where.host && peer.endpoint.port == where.port)
            return PeerChange::Unchanged;

        // Re-addressed in place, keeping the outbox and whatever is queued in it.
        // Replacing the peer would mean destroying a coroutine frame the reactor
        // may still point into, which is undefined behaviour rather than a tidy-up.
        // The id is not touched: it is this peer's key in the map, and it is the
        // one field a sender reads without the lock.
        peer.endpoint.host = std::move(where.host);
        peer.endpoint.port = where.port;

        // Only while a sender is actually running, and the guard is about lifetime
        // rather than efficiency: `CloseSockets` is a detached task, so submitting
        // one nothing will ever resume leaks its frame, and one resumed after this
        // transport is destroyed dereferences it. A live sender is what makes the
        // reactor a loop that is being driven -- and with none there is no socket to
        // close either, since a peer learned before `Start` has never dialled. The
        // same guard `RequestStop` uses, for the same reason.
        //
        // Submitted INSIDE the lock, which is what makes the guard mean anything:
        // `RequestStop` takes this same lock exclusively, so it cannot slip between
        // the test and the submission. Safe because `ResumeOn` never resumes inline
        // -- it suspends and posts -- so nothing here runs on the reactor's thread,
        // and the frame's first lock acquisition happens after this scope ends.
        if (_lifecycle == Lifecycle::Running && _sendersRunning.load(std::memory_order_acquire) != 0)
            // The sender is parked either in its own dial, in a write, or on its
            // outbox; closing the socket ends the second at once, is a no-op for the
            // first -- which `ServeOnce` catches when its dial resolves -- and the
            // third ends at the next message, which for a member is the next
            // heartbeat.
            PeerSenderAccess::CloseSockets(this, id);
    }

    return PeerChange::Readdressed;
}

std::size_t RaftPeerTransport::PeerCount() const noexcept
{
    auto const guard = std::shared_lock { _peersMutex };
    return _peers.size();
}

PeerEndpoint RaftPeerTransport::AddressOf(Peer const& peer) const
{
    auto const guard = std::shared_lock { _peersMutex };
    return peer.endpoint;
}

void RaftPeerTransport::StartSender(Peer& peer)
{
    peer.sender = PeerSenderAccess::RunSender(this, &peer);
    _sendersRunning.fetch_add(1, std::memory_order_acq_rel);

    // Submitted rather than started: see `Start()`'s doc comment for why the task
    // is lazy and what depends on it. Submitting under `_peersMutex` is safe for
    // exactly that reason -- the body's first instruction runs on the reactor
    // thread, so nothing here runs it inline.
    _reactor.Submit(peer.sender.Native());
}

RaftPeerTransport::~RaftPeerTransport()
{
    Stop();
}

void RaftPeerTransport::Start()
{
    // The state is moved and the senders submitted under one lock, so `Learn` sees
    // either `Idle` (this loop will pick the new peer up) or `Running` (this loop is
    // done, and `Learn` submits its own). Between those, on a bare atomic, a peer
    // learned in the window would be submitted twice.
    auto const guard = std::unique_lock { _peersMutex };
    if (_lifecycle != Lifecycle::Idle)
        return;

    _lifecycle = Lifecycle::Running;
    for (auto& [id, peer]: _peers)
        StartSender(*peer);
}

void RaftPeerTransport::RequestStop() noexcept
{
    _stop.Cancel();

    // `Stopping` and the outbox closures under one lock, so a `Learn` racing this
    // either finishes first -- and has its outbox closed by the loop below -- or
    // sees `Stopping` and refuses. Closing an outbox takes only that queue's own
    // lock, which nothing here holds in the other order.
    {
        auto const guard = std::unique_lock { _peersMutex };
        _lifecycle = Lifecycle::Stopping;
        for (auto& [id, peer]: _peers)
            peer->outbox.Close();
    }

    if (_sendersRunning.load(std::memory_order_acquire) != 0)
        PeerSenderAccess::CloseSockets(this, std::nullopt);
}

void RaftPeerTransport::Stop() noexcept
{
    RequestStop();

    // Nothing to wait for means nothing to deadlock on, so this is safe from any
    // thread -- which is what lets a single-threaded test drive the reactor
    // itself, drain the senders with `RequestStop()`, and then let the destructor
    // call this without tripping the check below.
    if (_sendersRunning.load(std::memory_order_acquire) == 0)
        return;

    // Only now: waiting for coroutines that can only progress on the reactor's
    // thread, FROM that thread, is a deadlock. Asserting turns it into a failed
    // assertion in the debug preset for the price of one comparison, rather than
    // a hang naming nothing.
    assert(_reactorThread.load(std::memory_order_relaxed) != std::this_thread::get_id()
           && "RaftPeerTransport::Stop must not be called from the reactor's own thread");

    // Bounded, because a stuck peer must not turn a stop into a hang -- which is
    // exactly what this transport used to do. Through `DrainWithin`, the one
    // bounded drain here: this loop had been a copy that named
    // `RaftPeerServer::Shutdown`'s ceiling and then accumulated the requested poll
    // rather than measuring it, enforcing 7.5 s on a host whose 10 ms sleep costs
    // 15 (#452).
    auto const outcome = DrainWithin([this] { return _sendersRunning.load(std::memory_order_acquire) != 0; });

    if (auto const stuck = _sendersRunning.load(std::memory_order_acquire); outcome == DrainResult::Ceiling && stuck != 0)
    {
        // Released rather than destroyed. The reactor may still hold these
        // handles, so destroying the frames is undefined behaviour while leaking
        // them is a deliberate, logged, diagnosable loss -- which is the right way
        // round.
        _logger.Log(LogLevel::Error,
                    std::format("raft: {} peer sender(s) did not finish within {} ms; leaking their frames",
                                stuck,
                                DrainBound {}.ceiling.count()));
        auto const guard = std::unique_lock { _peersMutex };
        for (auto& [id, peer]: _peers)
            std::ignore = peer->sender.Release();
    }
}

std::size_t RaftPeerTransport::SendersRunning() const noexcept
{
    return _sendersRunning.load(std::memory_order_acquire);
}

void RaftPeerTransport::NoteSenderFinished() noexcept
{
    _sendersRunning.fetch_sub(1, std::memory_order_acq_rel);
}

void RaftPeerTransport::NoteSenderThrew(NodeId const& peer) noexcept
{
    // `noexcept` and calling the logger straight, matching
    // `LogConnectionFirewallException`: a logger that throws is a programmer
    // error rather than a condition to recover from, and wrapping it here would
    // only move where that is discovered.
    _logger.Log(LogLevel::Error, std::format("raft: peer {} sender threw; the connection was dropped", peer));
}

void RaftPeerTransport::NoteOwnFault(Peer& peer, PeerEndpoint const& where, std::string_view reason)
{
    auto const now = _reactor.Clock().Now();
    if (now < peer.nextRefusalReport)
        return;
    peer.nextRefusalReport = now + RefusalReportInterval;

    _logger.Log(LogLevel::Error, std::format("raft: cannot prove this node's id to peer {}: {}", where.id, reason));
}

void RaftPeerTransport::NoteDialRefusal(Peer& peer,
                                        PeerEndpoint const& where,
                                        DiallerRefusal refusal,
                                        std::string_view detail)
{
    auto const& row = RowFor(refusal);
    _metrics.Increment(row.counter);

    auto const now = _reactor.Clock().Now();
    if (now < peer.nextRefusalReport)
        return;
    peer.nextRefusalReport = now + RefusalReportInterval;

    _logger.Log(LogLevel::Warn,
                std::format("raft: gave up on peer {} at {}:{} because {}{}{} (every refusal is counted; this line repeats "
                            "at most once a minute per peer)",
                            where.id,
                            where.host,
                            where.port,
                            row.says,
                            detail.empty() ? "" : ": ",
                            detail));
}

void RaftPeerTransport::Send(NodeId const& to, RaftMessage message)
{
    if (_stop.Token().IsCancelled())
        return;

    if (to == _self)
    {
        // A node does not talk to itself over a socket. Refused rather than
        // looped back, so a driver bug shows up here instead of as a connection
        // to this node's own listener.
        _logger.Log(LogLevel::Debug, "raft: refusing to send to self");
        return;
    }

    Peer* target = nullptr;
    {
        // The lookup is under the lock and the push below is not. A peer is only
        // ever added or re-addressed, never erased, so the pointer stays valid for
        // this transport's life -- which is what lets the queue's own lock be taken
        // outside this one rather than nested inside it.
        auto const guard = std::shared_lock { _peersMutex };
        auto const found = _peers.find(to);
        if (found != _peers.end())
            target = found->second.get();
    }

    if (target == nullptr)
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _logger.Log(LogLevel::Debug, std::format("raft: no endpoint configured for peer {}", to));
        return;
    }

    // Framed HERE, on the caller's thread, so the queue bound bounds BYTES rather
    // than a count of messages whose size nobody knows yet.
    auto frame = RaftWire::Encode(message);
    if (frame.empty())
    {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _logger.Log(LogLevel::Debug, std::format("raft: could not frame a message for peer {}", to));
        return;
    }

    // Never resumes the sender inline -- see `AsyncQueue`. This matters here
    // specifically because `Send` is reached from `RaftDriver::Deliver`, which
    // holds the driver's mutex.
    auto const pushed = target->outbox.Push(std::move(frame));
    _dropped.fetch_add(pushed.displaced, std::memory_order_relaxed);
}

} // namespace FastCache::Consensus

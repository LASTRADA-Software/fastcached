// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftSessionLink.hpp>
#include <FastCache/Consensus/RaftSessionReader.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>
#include <FastCache/Transport/AcceptLoopReporter.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <core/async/ResumeOn.hpp>
#include <core/net/SocketDeadline.hpp>

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
        RegisteredConnection(OpenConnections* open, core::net::ISocket* socket) noexcept:
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
        core::net::ISocket* _socket;
    };

    /// Makes one one-way session the newest its dialler holds, for as long as it is being served.
    ///
    /// RAII for `RegisteredConnection`'s reason. The entry is erased only while it still names
    /// THIS session's socket: a session a newer one superseded ends after the newer one replaced
    /// it, and erasing by id alone would forget the newer one -- whose own successor would then
    /// find nothing to close.
    class NewestOneWaySession
    {
      public:
        /// @param open Where the entry lives; must outlive this.
        /// @param dialler The member the session proved.
        /// @param socket The session's connection.
        NewestOneWaySession(OpenConnections* open, NodeId dialler, core::net::ISocket* socket):
            _open { open },
            _dialler { std::move(dialler) },
            _socket { socket },
            _superseded { Replace(open, _dialler, socket) }
        {
        }

        NewestOneWaySession(NewestOneWaySession const&) = delete;
        NewestOneWaySession(NewestOneWaySession&&) = delete;
        NewestOneWaySession& operator=(NewestOneWaySession const&) = delete;
        NewestOneWaySession& operator=(NewestOneWaySession&&) = delete;

        ~NewestOneWaySession()
        {
            auto const guard = std::scoped_lock { _open->mutex };
            if (auto const entry = _open->oneWay.find(_dialler); entry != _open->oneWay.end() && entry->second == _socket)
                _open->oneWay.erase(entry);
        }

        /// @return The socket of the session this one replaced, or null when there was none.
        [[nodiscard]] core::net::ISocket* Superseded() const noexcept
        {
            return _superseded;
        }

      private:
        /// Name @p socket as @p dialler's newest one-way session, under the map's lock.
        /// @param open Where the entry lives.
        /// @param dialler The member the session proved.
        /// @param socket The session's connection.
        /// @return The socket the entry named before, or null when there was none.
        [[nodiscard]] static core::net::ISocket* Replace(OpenConnections* open,
                                                         NodeId const& dialler,
                                                         core::net::ISocket* socket)
        {
            auto const guard = std::scoped_lock { open->mutex };
            return std::exchange(open->oneWay[dialler], socket);
        }

        OpenConnections* _open;
        NodeId _dialler;
        core::net::ISocket* _socket;
        core::net::ISocket* _superseded; ///< Initialised last: it is read from the entry `_dialler` names.
    };

    /// Keeps one two-way session attached to the transport for as long as it is being served.
    ///
    /// RAII for `RegisteredConnection`'s reason: a detach forgotten on one of the session's exits
    /// is a transport still writing to a learner through a link whose reader has gone, and every
    /// way out of `PeerServerAccess::Serve` is one of those exits. Holds nothing, and does
    /// nothing, for a one-way session.
    class AttachedLink
    {
      public:
        /// @param links Where to attach; must outlive this.
        /// @param link The session, or null when this end does not write on it.
        AttachedLink(IRaftInboundLinks* links, std::shared_ptr<RaftSessionLink> link):
            _links { links },
            _link { std::move(link) }
        {
            if (_link != nullptr)
                _links->Attach(_link);
        }

        AttachedLink(AttachedLink const&) = delete;
        AttachedLink(AttachedLink&&) = delete;
        AttachedLink& operator=(AttachedLink const&) = delete;
        AttachedLink& operator=(AttachedLink&&) = delete;

        ~AttachedLink()
        {
            if (_link != nullptr)
                _links->Detach(*_link);
        }

      private:
        IRaftInboundLinks* _links;
        std::shared_ptr<RaftSessionLink> _link;
    };

    /// A connection that has proved its id: who it proved, with which key, the session its
    /// frames are bound to, and which way it asked for them to flow.
    struct ProvenPeer
    {
        NodeId dialler;             ///< The member the connection proved.
        Ed25519PublicKey provenKey; ///< The key it proved that with; re-checked on every frame.
        SessionKeys session;        ///< What its frames are sealed under; it writes `diallerToAcceptor`.

        /// What the dialler's SIGNED proof asked for: whether this end writes on the connection
        /// too. Carried from the judgement, because a two-way session this end never attaches is
        /// a learner the leader never reaches, and nothing about that would be visible.
        RaftWire::SessionDirection direction { RaftWire::SessionDirection::OneWay };
    };

    /// How a `RaftSessionReader` ending is counted and logged at the ACCEPTING end.
    ///
    /// `PeerServerAccess::Serve` used to decide this inline, once per way its own loop could
    /// end; now the loop lives in `ReadProvenSession`, shared with the dialling end, so what is
    /// left here is exactly the acceptor's judgement of an ending -- a table rather than a
    /// `switch` that would repeat the ordering `ReadProvenSession` already enforced. The
    /// dialling end has a table of its own in `RaftPeerTransport.cpp`, phrased as the dialler.
    using AcceptorSessionEndRow = SessionEndRow<AcceptorRefusal>;

    /// One row per `SessionEnd`, in enumerator order.
    ///
    /// Every ending but a peer closing -- the ordinary way a connection ends -- moves an
    /// `AcceptorRefusal` of its own, as the dialler's table does for the direction it reads. A bad
    /// magic and an over-cap frame are refused before any tag verifies, so their counters say what
    /// was OBSERVED on a proven connection, never that the proven member sent it: anything on the
    /// path can produce either. Each counted row's sentence is its `AcceptorRefusals` row's.
    constexpr EnumTable<SessionEnd, AcceptorSessionEndRow> SessionEndRows { {
        { .end = SessionEnd::PeerClosed, .refusal = std::nullopt, .level = std::nullopt, .sentence = "" },
        { .end = SessionEnd::BadMagic, .refusal = AcceptorRefusal::FrameBadMagic, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::OverCap, .refusal = AcceptorRefusal::FrameOverCap, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::BadTag, .refusal = AcceptorRefusal::FrameTag, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::KeyWithdrawn,
          .refusal = AcceptorRefusal::KeyWithdrawn,
          .level = LogLevel::Warn,
          .sentence = "" },
        { .end = SessionEnd::WrongSender, .refusal = AcceptorRefusal::FrameSender, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::Unreadable,
          .refusal = AcceptorRefusal::FrameUnreadable,
          .level = LogLevel::Warn,
          .sentence = "" },
        // Silent BY ROW: an acceptor arms no idle bound -- its dialler writes when it has
        // something to say, and a one-way dialler says nothing on purpose -- so this end never
        // names the ending. The row exists because the enum is shared with the dialler, which does.
        { .end = SessionEnd::Silent, .refusal = std::nullopt, .level = std::nullopt, .sentence = "" },
    } };

    static_assert(RowsInEnumeratorOrder(SessionEndRows, &AcceptorSessionEndRow::end),
                  "SessionEndRows must hold one row per SessionEnd, in enumerator order");
} // namespace

/// Grants the per-connection coroutines access to the server's privates.
///
/// Free functions taking raw pointers rather than members or lambdas, the shape
/// `RaftPeerTransport`'s `PeerSenderAccess` uses: a coroutine's frame outlives the
/// expression that created it, so a captured `this` is a lifetime question at every
/// suspension point rather than a documented one here. `Shutdown` drains before
/// returning, which is what makes the borrowed pointers safe.
///
/// `SessionObserver` below reaches the server's counters and its logger through the two static
/// methods here rather than being a friend of `RaftPeerServer` itself: it is defined in this
/// file's anonymous namespace, where a `friend` declaration inside `RaftPeerServer.hpp` cannot
/// name it, and `PeerServerAccess` already is the one name this header trusts with these members.
struct PeerServerAccess
{
    /// Serve one accepted connection: the handshake, then its frames.
    /// @param self The server; outlives this by `Shutdown`'s drain.
    /// @param accepted The accepted connection; owned for its lifetime, and shared with the
    ///        transport while a two-way session is attached.
    static core::async::DetachedTask ServePeer(RaftPeerServer* self, std::unique_ptr<core::net::ISocket> accepted);

    /// Challenge, read one proof, judge it and answer, all within the handshake bound.
    /// @param self The server.
    /// @param socket The connection.
    /// @param reader The connection's reader, which the session goes on using.
    /// @param peer The address the connection came from.
    /// @return The proven peer, or nothing when the connection is not to be served.
    static core::async::Task<std::optional<ProvenPeer>> Handshake(RaftPeerServer* self,
                                                                  core::net::ISocket* socket,
                                                                  ByteReader* reader,
                                                                  std::string peer);

    /// Read, check and deliver the frames of a connection that proved its id.
    /// @param self The server.
    /// @param reader The connection's reader.
    /// @param peer The address the connection came from.
    /// @param proven Who it proved, and its session.
    static core::async::Task<void> Serve(RaftPeerServer* self, ByteReader* reader, std::string peer, ProvenPeer proven);

    /// Bump the delivered-message counter.
    /// @param self The server.
    static void NoteDelivered(RaftPeerServer* self) noexcept;

    /// Bump the skipped-frame counter and log it, naming the peer that sent it.
    /// @param self The server.
    /// @param peer The address the connection came from.
    /// @param dialler The member the connection proved.
    /// @param error What the decode reported; @c error.context names the frame's own description.
    static void NoteSkipped(RaftPeerServer* self, std::string_view peer, NodeId const& dialler, ConsensusError const& error);
};

namespace
{
    /// Feeds a proven session's per-frame progress back to the counters and the log line a real
    /// connection needs, so `ReadProvenSession` itself never has to know either exists.
    ///
    /// Holds its own copies of `peer` and `dialler` rather than borrowing `PeerServerAccess::Serve`'s
    /// -- cheap for a per-connection object, and it is what lets this type make no claim at all
    /// about a coroutine frame's lifetime, which is `ReadProvenSession`'s rule to keep and not
    /// this one's to reason about.
    class SessionObserver final: public IProvenSessionObserver
    {
      public:
        /// @param self The server; outlives every connection by `Shutdown`'s drain.
        /// @param peer The address the connection came from.
        /// @param dialler The member the connection proved.
        SessionObserver(RaftPeerServer* self, std::string peer, NodeId dialler) noexcept:
            _self { self },
            _peer { std::move(peer) },
            _dialler { std::move(dialler) }
        {
        }

        void OnDelivered() override
        {
            PeerServerAccess::NoteDelivered(_self);
        }

        void OnSkipped(ConsensusError const& error) override
        {
            PeerServerAccess::NoteSkipped(_self, _peer, _dialler, error);
        }

      private:
        RaftPeerServer* _self;
        std::string _peer;
        NodeId _dialler;
    };
} // namespace

core::async::DetachedTask PeerServerAccess::ServePeer(RaftPeerServer* self, std::unique_ptr<core::net::ISocket> accepted)
{
    // Shared from the top, because a two-way session's socket is written by the transport for
    // as long as it holds the link -- which can outlast this coroutine by the sender's last
    // step. Every owner of the link lets go on this reactor (`RaftSessionLink`), so whichever
    // is last destroys the socket here.
    auto const socket = std::shared_ptr<core::net::ISocket> { std::move(accepted) };
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
    {
        // Attached only once the verdict is written, so this end's last write on the socket is
        // over before the transport's first: one writer at a time, which the socket's contract
        // enforces by ending the process. Detached before the close below, on every way `Serve`
        // ends.
        auto link = AcceptorWrites(proven->direction)
                        ? std::make_shared<RaftSessionLink>(proven->dialler,
                                                            proven->provenKey,
                                                            socket,
                                                            FrameSealer { std::move(proven->session.acceptorToDialler) })
                        : std::shared_ptr<RaftSessionLink> {};
        AttachedLink const attached { &self->_links, std::move(link) };

        // A one-way session is the newest its dialler holds, and closes the one it supersedes: a
        // dialler holds ONE outbound session to this node, so an older one is a path that vanished
        // -- a voter whose address moved -- and nothing else would ever end it. Closed outside the
        // map's lock, because a close may resume that session's task, whose own guard takes it.
        // Still alive here: its task ends on this reactor, which is running this one.
        auto newest = std::optional<NewestOneWaySession> {};
        if (!AcceptorWrites(proven->direction))
        {
            newest.emplace(&self->_open, proven->dialler, socket.get());
            if (auto* const superseded = newest->Superseded(); superseded != nullptr)
            {
                superseded->close();
                self->NoteSuperseded(peer, proven->dialler);
            }
        }

        co_await Serve(self, &reader, std::move(peer), *std::move(proven));
    }

    socket->close();
    self->_active.fetch_sub(1, std::memory_order_acq_rel);
}

core::async::Task<std::optional<ProvenPeer>> PeerServerAccess::Handshake(RaftPeerServer* self,
                                                                         core::net::ISocket* socket,
                                                                         ByteReader* reader,
                                                                         std::string peer)
{
    // Armed before the first write, so the bound covers the whole exchange: a peer that
    // never reads the challenge stalls the write, and one that never proves stalls the
    // read, and both are the same stranger holding a slot. Expiry closes the socket,
    // which completes whichever of the two is parked -- and the flag is how an ending
    // is told apart from a peer that simply went away.
    core::net::SocketDeadlineTarget expiry { .socket = socket };
    auto deadline = core::net::armSocketDeadline(&self->_reactor, self->_options.handshakeBound, &expiry);

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
                           .session = *std::move(judgement.session),
                           .direction = judgement.direction };
}

void PeerServerAccess::NoteDelivered(RaftPeerServer* self) noexcept
{
    self->_delivered.fetch_add(1, std::memory_order_relaxed);
}

void PeerServerAccess::NoteSkipped(RaftPeerServer* self,
                                   std::string_view peer,
                                   NodeId const& dialler,
                                   ConsensusError const& error)
{
    // Counted rather than only logged, because it is the number that says a fleet is
    // mid-upgrade: steady and non-zero means some peer speaks something this node does not,
    // which is expected during a rollout and a misconfiguration afterwards.
    self->_skipped.fetch_add(1, std::memory_order_relaxed);
    self->_logger.Log(LogLevel::Debug,
                      std::format("raft: skipped a frame from peer {} ({}): {}", dialler, peer, error.context));
}

core::async::Task<void> PeerServerAccess::Serve(RaftPeerServer* self,
                                                ByteReader* reader,
                                                std::string peer,
                                                ProvenPeer proven)
{
    FrameOpener opener { std::move(proven.session.diallerToAcceptor) };
    auto const who = ProvenSessionPeer { .id = proven.dialler, .key = proven.provenKey };
    SessionObserver observer { self, peer, proven.dialler };
    auto const ending = co_await ReadProvenSession(reader,
                                                   &opener,
                                                   &who,
                                                   &self->_identity,
                                                   &self->_sink,
                                                   SessionReadLimits { .maxFrameBytes = self->_options.maxFrameBytes },
                                                   &observer);
    self->NoteSessionEnd(ending, peer, proven.dialler);
}

RaftPeerServer::RaftPeerServer(core::net::IListener& listener,
                               core::net::EventLoop& reactor,
                               IRaftMessageSink& sink,
                               IRaftInboundLinks& links,
                               ILogger& logger,
                               IMetricsSink& metrics,
                               IRaftPeerIdentity const& identity,
                               ISecureRandom& random,
                               core::net::AcceptLoopHealth& acceptLoops,
                               PeerServerOptions options):
    _listener { listener },
    _reactor { reactor },
    _sink { sink },
    _links { links },
    _logger { logger },
    _metrics { metrics },
    _identity { identity },
    _random { random },
    _acceptLoops { acceptLoops },
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
    // Not throttled, and not because only the proven member can provoke one: anything on the path
    // can flip a tag bit, and a bad magic or an over-cap frame is refused before any tag verifies.
    // What bounds the volume is that each of these ENDS a proven connection, so another one costs a
    // whole new handshake the member itself must dial -- at most one per its redial backoff, the
    // `DialBackoffTable` row for the session's direction (`DialBackoffOf`). That is a rate the log
    // can carry, and each line names a member worth naming.
    auto const& row = RowFor(refusal);
    _metrics.Increment(row.counter);
    _logger.Log(
        LogLevel::Warn,
        std::format(
            "raft: refused peer {} at {} because {}{}{}", dialler, peer, row.says, detail.empty() ? "" : ": ", detail));
}

void RaftPeerServer::NoteSuperseded(std::string_view peer, NodeId const& dialler)
{
    _metrics.Increment(IMetricsSink::Counter::RaftInboundSessionsSuperseded);
    _logger.Log(LogLevel::Debug,
                std::format("raft: peer {} proved a newer one-way session from {}; its earlier one is superseded and "
                            "closed",
                            dialler,
                            peer));
}

void RaftPeerServer::NoteSessionEnd(SessionEnding const& ending, std::string_view peer, NodeId const& dialler)
{
    auto const& row = SessionEndRows[static_cast<std::size_t>(ending.end)];

    // Every ending but a close moves a counter of its own, logged in its refusal row's words.
    if (row.refusal.has_value())
    {
        NoteProvenRefusal(*row.refusal, peer, dialler, ending.detail);
        return;
    }

    // PeerClosed: silent BY ROW -- a fact the table states, not one inferred from an empty
    // sentence and an empty detail, which a future log-only ending with a sentence and no
    // detail would satisfy without meaning to be silent.
    if (!row.level.has_value())
        return;

    _logger.Log(*row.level,
                std::format("raft: peer {} ({}) {}{}{}; closing",
                            dialler,
                            peer,
                            row.sentence,
                            row.sentence.empty() || ending.detail.empty() ? "" : ": ",
                            ending.detail));
}

core::async::Task<void> RaftPeerServer::Run()
{
    // `core::net::AcceptErrorPolicy` decides, as it does for every accept loop in the tree, and
    // `AcceptLoopReporter` says so: a poll timeout -- how this loop wakes to observe `Shutdown()` on
    // POSIX, where closing the listening socket does not unblock a parked accept() -- is accepted
    // past silently, a peer that reset its queued connection with a rate-limited warning, and only
    // a closed or dead listener ends the loop. It used to end on anything but the poll tick, at
    // `Debug`, taking this node out of its cluster while the port still listened.
    AcceptLoopReporter acceptErrors { "raft: peer", "raft", _logger, _acceptLoops };
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.accept();
        if (!accepted.has_value())
        {
            auto const step = acceptErrors.OnError(
                accepted.error(), _reactor.clock().now(), _shuttingDown.load(std::memory_order_acquire));
            if (step.next == AcceptLoopNext::EndAndClose)
                _listener.close();
            if (step.next != AcceptLoopNext::AcceptAgain)
                co_return;
            if (step.delay > std::chrono::milliseconds {})
                co_await _reactor.delay(step.delay);
            continue;
        }
        acceptErrors.OnAccepted(_reactor.clock().now());

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
    // Shut down between accepts: a loop that was degraded says it stopped.
    acceptErrors.OnLoopEnded();
    co_return;
}

void RaftPeerServer::CloseAll() noexcept
{
    _listener.close();

    // Copied out under the lock rather than closed under it: the close wakes the task
    // that removes itself from this very vector, and although that task now resumes in
    // the loop's next drain rather than inside `close()` (core-cpp 0.2.1, guarantee G2),
    // a close is no place to hold a lock the woken code takes.
    auto sockets = std::vector<core::net::ISocket*> {};
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

    // Posted onto the reactor, never done here. See the declaration: a socket is closed
    // on the thread that drives it, because `close()` retires its registration with the
    // loop, and the connection tasks it wakes then resume in that loop's drain, on the
    // reactor -- which is where the socket each of them owns must be destroyed (the
    // teardown rule, #668, which the stopping thread violated at a second owner, #885).
    //
    // Borrows `this` rather than sharing state, which is sound for the same reason
    // the connection tasks may: the drain below does not return until this has run.
    [](RaftPeerServer* self) -> core::async::DetachedTask {
        co_await core::async::ResumeOn { self->_reactor };
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

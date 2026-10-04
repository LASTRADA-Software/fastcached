// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftSessionLink.hpp>
#include <FastCache/Consensus/RaftSessionReader.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/InterruptibleSleep.hpp>
#include <core/net/SocketDeadline.hpp>

namespace FastCache::Consensus
{

namespace
{

    /// Write every byte of `bytes` to `socket`.
    ///
    /// A coroutine because `core::net::ISocket::Write` is awaitable, awaited by the peer's
    /// sender on the reactor. It used to be driven with `core::async::syncRun`, which was sound
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
    /// `core::net::ISocket::Write` is write-all by contract -- "Resolves with the byte count
    /// actually written (== buffer.size() on success)" -- so the count is checked
    /// rather than looped over. A resume loop here would be dead code that reads as
    /// a statement about the interface, and a reader who believed it would write
    /// partial-write handling at every other call site too; the three that already
    /// exist just check the count.
    /// @param socket The connected peer socket; never null.
    /// @param bytes The framed message.
    /// @return Whether every byte reached the socket.
    [[nodiscard]] core::async::Task<bool> WriteFrame(core::net::ISocket* socket, std::span<std::byte const> bytes)
    {
        auto const written = co_await socket->write(bytes);
        co_return written.has_value() && *written == bytes.size();
    }

    /// Seal @p frame at @p sealer's next position and append its tag.
    ///
    /// Sealed when it is WRITTEN rather than when queued, because a frame's tag is bound to the
    /// connection it goes out on and to its position there -- and a queued frame outlives the
    /// connection that was current when it was framed. Appended to the frame so it goes out in
    /// the one write `core::net::ISocket::Write`'s contract makes whole. One function for both
    /// directions a session can be written in, so they cannot seal differently.
    /// @param sealer The writing end's sealer; moves on by one frame.
    /// @param frame A frame as `RaftWire::Encode` produced it; the tag is appended.
    void SealInPlace(FrameSealer& sealer, std::vector<std::byte>& frame)
    {
        auto const bytes = std::span<std::byte const> { frame };
        auto const headerSize = std::min(bytes.size(), RaftWire::HeaderSize);
        auto const tag = sealer.Seal(bytes.first(headerSize), bytes.subspan(headerSize));
        frame.insert(frame.end(), tag.begin(), tag.end());
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
    [[nodiscard]] core::async::Task<HandshakeFrameRead> ReadHandshakeFrame(ByteReader* reader)
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
        Ed25519PublicKey provenKey; ///< Re-checked against the roster before every frame, both ways.

        /// What this end's frames are sealed under (`diallerToAcceptor`), and on a two-way
        /// session what the acceptor's are opened under (`acceptorToDialler`).
        SessionKeys session;
    };

    /// How a session this node DIALLED is counted and logged when its reader ends.
    ///
    /// The acceptor's table (`RaftPeerServer.cpp`) judges the same endings for the session it
    /// accepted; this one is the dialler's, and every row but a peer closing moves a `DiallerRefusal`
    /// of its own. That is not the acceptor's split, and it should not be: an acceptor counts a
    /// frame it could not use on the direction IT reads, and only the dialler reads this one, so a
    /// tampered or foreign acceptor-to-dialler frame counted here is counted nowhere else. The
    /// withdrawn key is the row the writer moves too, when it is the one to notice -- counted once
    /// per session whichever half notices first, through `SessionHalves::withdrawalCounted`.
    /// Each counted row's sentence is its `DiallerRefusals` row's, logged by `NoteDialRefusal`.
    using DiallerSessionEndRow = SessionEndRow<DiallerRefusal>;

    /// One row per `SessionEnd`, in enumerator order.
    constexpr EnumTable<SessionEnd, DiallerSessionEndRow> DiallerSessionEndRows { {
        { .end = SessionEnd::PeerClosed, .refusal = std::nullopt, .level = std::nullopt, .sentence = "" },
        { .end = SessionEnd::BadMagic, .refusal = DiallerRefusal::FrameBadMagic, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::OverCap, .refusal = DiallerRefusal::FrameOverCap, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::BadTag, .refusal = DiallerRefusal::FrameTag, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::KeyWithdrawn,
          .refusal = DiallerRefusal::KeyWithdrawn,
          .level = LogLevel::Warn,
          .sentence = "" },
        { .end = SessionEnd::WrongSender, .refusal = DiallerRefusal::FrameSender, .level = LogLevel::Warn, .sentence = "" },
        { .end = SessionEnd::Unreadable,
          .refusal = DiallerRefusal::FrameUnreadable,
          .level = LogLevel::Warn,
          .sentence = "" },
        // Named by this end's own idle bound (`SessionSilence`), never by `ReadProvenSession`.
        { .end = SessionEnd::Silent, .refusal = DiallerRefusal::SessionSilent, .level = LogLevel::Debug, .sentence = "" },
    } };

    static_assert(RowsInEnumeratorOrder(DiallerSessionEndRows, &DiallerSessionEndRow::end),
                  "DiallerSessionEndRows must hold one row per SessionEnd, in enumerator order");

    /// How one `SendDrop` is counted and logged.
    struct SendDropRow
    {
        SendDrop drop;                 ///< The drop this row describes.
        IMetricsSink::Counter counter; ///< The series it moves.
        std::string_view says;         ///< What was seen, completing the Debug line.
    };

    /// One row per `SendDrop`, in enumerator order: what `Send` observed, and where it is counted.
    constexpr EnumTable<SendDrop, SendDropRow> SendDropRows { {
        { .drop = SendDrop::NoSession,
          .counter = IMetricsSink::Counter::RaftSendsDroppedNoSession,
          .says = "it dials in and no session of its is attached -- a learner that is offline, or has not dialled yet" },
        { .drop = SendDrop::UnknownPeer,
          .counter = IMetricsSink::Counter::RaftSendsDroppedUnknownPeer,
          .says = "this node neither dials it nor was told it dials in, so nothing here can reach it" },
    } };

    static_assert(RowsInEnumeratorOrder(SendDropRows, &SendDropRow::drop),
                  "SendDropRows must hold one row per SendDrop, in enumerator order");

    /// The largest single read a dialler's connection makes, by the shape of session it dials.
    ///
    /// A one-way dialler reads the handshake and nothing after it. A two-way one reads a session
    /// too, through the SAME reader, so its cap covers a session frame and its tag -- the cap the
    /// acceptor's reader has. Only a cap: each handshake frame's declared length is still checked
    /// against its own ceiling before anything of that size is asked for.
    /// @param direction The shape of session dialled.
    /// @return The reader's payload cap.
    [[nodiscard]] std::size_t DiallerReadCap(RaftWire::SessionDirection direction) noexcept
    {
        return AcceptorWrites(direction)
                   ? std::max(SessionReadLimits {}.maxFrameBytes + RaftWire::TagSize, RaftWire::MaxHandshakePayload)
                   : RaftWire::MaxHandshakePayload;
    }

} // namespace

std::chrono::milliseconds NextBackoff(RaftWire::SessionDirection direction, std::chrono::milliseconds previous) noexcept
{
    auto const& row = DialBackoffOf(direction);
    return std::clamp(previous * 2, row.initial, row.cap);
}

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

    /// What one attempt at serving a peer amounted to, for the backoff after it.
    struct Served
    {
        Outcome outcome { Outcome::Retry }; ///< How it ended.

        /// Whether the session carried at least one frame EACH way: the peer is up, so the next
        /// wait starts again from the table's initial one. Never true for a one-way session,
        /// whose row does not grow anyway.
        bool carriedBothWays { false };
    };

    /// One proven session's two halves and what they share.
    ///
    /// Lives in `ServeOnce`'s frame, which awaits both halves before it ends, so the pointers the
    /// halves hold into it never outlive it. Reactor-thread only, like the socket, so nothing here
    /// is synchronised: the halves take turns on one thread.
    struct SessionHalves
    {
        RaftPeerTransport* self { nullptr };       ///< The transport.
        RaftPeerTransport::Peer* peer { nullptr }; ///< The peer; its socket is the session's.
        PeerEndpoint where;                        ///< Where it was dialled, for the log lines.
        ProvenAcceptor proven;                     ///< The key the acceptor proved, and the session's keys.

        Outcome outcome { Outcome::Retry }; ///< How the WRITER ended: only it sees a stop.
        bool readerEnded { false };         ///< The reader has returned; a zero-length frame is its.
        bool writerEnded { false };         ///< The writer has returned; the reader need not wake it.
        std::size_t written { 0 };          ///< Frames this end wrote.
        std::size_t delivered { 0 };        ///< Frames the acceptor wrote that were delivered.

        /// Whether this session's withdrawn key has been counted, by EITHER half.
        ///
        /// The one ending both halves can observe, and in either order: the reader first when the
        /// acceptor's frame arrives ahead of a `Send`, the writer first when its wake is queued ahead
        /// of a read completion that had already settled -- which on IOCP is only the port's order.
        /// A property of the SESSION rather than a check per order, so the next ordering is covered
        /// too: `NoteWithdrawal` is the only thing that counts it.
        bool withdrawalCounted { false };
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
    static core::async::Task<Served> ServeOnce(RaftPeerTransport* self, RaftPeerTransport::Peer* peer);

    /// Prove this node's id to the acceptor at `peer->socket`, and check its verdict, within
    /// the handshake bound.
    /// @param self The transport.
    /// @param peer The peer being dialled; its socket is connected.
    /// @param reader The connection's one reader, which a two-way session goes on reading
    ///        through: the acceptor's first frame may already sit in it behind the verdict.
    /// @param where Where it was dialled, for the log line.
    /// @return The key the acceptor proved itself with and the session's key, when it
    ///         accepted this node as the member it dialled; nothing otherwise, the refusal
    ///         already counted.
    static core::async::Task<std::optional<ProvenAcceptor>> Handshake(RaftPeerTransport* self,
                                                                      RaftPeerTransport::Peer* peer,
                                                                      ByteReader* reader,
                                                                      PeerEndpoint where);

    /// Write the peer's outbox on the session until the outbox closes, the acceptor's key is
    /// withdrawn, a write fails, or the reader has ended; then close the socket.
    /// @param halves The session.
    static core::async::Task<void> WriteSession(SessionHalves* halves);

    /// Read, check and deliver what the acceptor writes on a two-way session until it ends; then
    /// close the socket and wake the writer with a zero-length frame, unless it has ended already.
    /// @param halves The session.
    /// @param reader The handshake's reader, carried on.
    static core::async::Task<void> ReadSession(SessionHalves* halves, ByteReader* reader);

    /// Closes and releases a peer's socket when one connection's life ends, on every way out of
    /// `ServeOnce`, a throw included.
    ///
    /// Declared BEFORE anything that reads the socket, so it is destroyed AFTER them: the
    /// connection's `ByteReader` holds a reference to the socket and must not outlive it, which it
    /// did while the release was a statement before each `co_return`.
    class SocketRelease
    {
      public:
        /// @param peer The peer whose socket this connection is; it has one.
        explicit SocketRelease(RaftPeerTransport::Peer* peer) noexcept:
            _peer { peer }
        {
        }

        SocketRelease(SocketRelease const&) = delete;
        SocketRelease(SocketRelease&&) = delete;
        SocketRelease& operator=(SocketRelease const&) = delete;
        SocketRelease& operator=(SocketRelease&&) = delete;

        ~SocketRelease()
        {
            ReleaseSocket(_peer);
        }

      private:
        RaftPeerTransport::Peer* _peer;
    };

    /// Ends a two-way session's reader on every way out of `ReadSession`, a throw included.
    ///
    /// `whenAll` waits for BOTH halves, and the writer ends only when told: a reader that threw --
    /// a sink that threw, an allocation that failed -- and skipped its ending would leave the writer
    /// parked on the outbox for good, and the session counted as connected with nothing reading it.
    ///
    /// **Its premise: a session's frames are RESUMED to their end, never destroyed while parked.** A
    /// destructor also runs when a suspended frame is destroyed, and this one's work is only sound on
    /// the way out of a frame that ran: the wake it pushes posts the writer's handle to the reactor,
    /// and the writer's runner frame is destroyed right after this one inside `whenAll`, which would
    /// leave the reactor holding a handle to freed memory. It holds today because nothing destroys
    /// them: `RunSender`'s task is owned by `Peer::sender`, which is never erased, and `Stop()` at its
    /// ceiling RELEASES a stuck sender's frame rather than destroying it. An owner that starts
    /// destroying a parked sender -- to forget a peer, say -- must first stop this guard doing its
    /// work, or resume the frame to its end instead. `SocketRelease` stands on the same premise.
    class ReaderEnding
    {
      public:
        /// @param halves The session.
        explicit ReaderEnding(SessionHalves* halves) noexcept:
            _halves { halves }
        {
        }

        ReaderEnding(ReaderEnding const&) = delete;
        ReaderEnding(ReaderEnding&&) = delete;
        ReaderEnding& operator=(ReaderEnding const&) = delete;
        ReaderEnding& operator=(ReaderEnding&&) = delete;

        ~ReaderEnding()
        {
            EndReader(_halves);
        }

      private:
        SessionHalves* _halves;
    };

    /// Close and release @p peer's socket, if it still has one. Reactor thread only.
    /// @param peer The peer.
    static void ReleaseSocket(RaftPeerTransport::Peer* peer) noexcept;

    /// Mark the reader ended, close the socket, and wake a writer still parked on the outbox.
    /// @param halves The session.
    static void EndReader(SessionHalves* halves) noexcept;

    /// What a session's sender does with one entry it popped off its outbox.
    ///
    /// **Private: never transmitted or persisted**, so the enumerators carry no values.
    enum class SenderStep : std::uint8_t
    {
        Write,    ///< A message, on a session whose key still proves: sealed, then written.
        StepOver, ///< A wake, on a session whose key still proves: nothing is sealed or written.
        Withdraw, ///< The key no longer proves: the session ends here.
    };

    /// Decide what a sender does with one entry popped off its outbox -- asked by BOTH senders,
    /// the dialled one and the attached one, so a wake means the same thing on either side.
    ///
    /// The roster first, whatever was popped: a WAKE -- a zero-length entry, which no message is,
    /// every one having a header -- exists precisely to make a sender with nothing to write ask
    /// it (`RaftPeerTransport::RecheckProofs`, and a dialled session's reader ending). Only a
    /// MESSAGE then reaches the seal. A wake that reached it would spend a sequence number the
    /// peer never sees, and the next real frame's tag would fail at the other end
    /// (`Core/SessionSeal.hpp`): an idle session closed by the act of asking whether it may stay
    /// open. A message popped into a withdrawal is counted as dropped with the session; a wake is
    /// not a message, so it is not.
    /// @param self The transport, whose roster is asked and whose drop count moves.
    /// @param peer The member the session proved.
    /// @param key The key it proved itself with.
    /// @param popped What was popped.
    /// @return The step.
    [[nodiscard]] static SenderStep StepFor(RaftPeerTransport* self,
                                            NodeId const& peer,
                                            Ed25519PublicKey const& key,
                                            std::vector<std::byte> const& popped);

    /// Count this session's withdrawn key, once, whichever half noticed it.
    /// @param halves The session.
    /// @param detail What was seen, for the log line.
    static void NoteWithdrawal(SessionHalves* halves, std::string_view detail);

    /// Count and log how a session this node dialled ended, by `DiallerSessionEndRows`.
    /// @param halves The session.
    /// @param ending Why its reader ended.
    static void NoteSessionEnd(SessionHalves* halves, SessionEnding const& ending);

    /// Say that a frame on a session this node dialled was stepped over, naming the peer.
    /// @param halves The session.
    /// @param error What the decode reported.
    static void NoteSkipped(SessionHalves const* halves, ConsensusError const& error);

    /// One peer's whole life. Ends only on a stop.
    static core::async::Task<void> RunSender(RaftPeerTransport* self, RaftPeerTransport::Peer* peer);

    /// Write an attached session's outbox to its socket until the outbox closes, the roster
    /// withdraws the key the session was proved with, or a write fails.
    ///
    /// Detached rather than a `Task` the entry owns: see the class comment. Runs to its first
    /// park on the calling thread, which is the reactor's, and holds its own share of the entry
    /// -- the outbox it parks on and the socket it writes -- until it ends.
    /// @param self The transport; outlives this by `Stop()`'s drain.
    /// @param inbound The session and its outbox.
    static core::async::DetachedTask RunInboundSender(RaftPeerTransport* self,
                                                      std::shared_ptr<RaftPeerTransport::InboundPeer> inbound);

    /// Close live peer sockets, on the reactor's thread.
    ///
    /// One peer or all of them: a shutdown ends every session, and a peer that has
    /// moved needs exactly its own dropped so its sender redials the new address.
    /// One function rather than two, because what makes this correct is the thread
    /// it runs on and that argument is the same either way.
    /// @param self The transport.
    /// @param only Which peer, or nullopt for every one.
    static core::async::DetachedTask CloseSockets(RaftPeerTransport* self, std::optional<NodeId> only);
};

namespace
{
    /// A two-way session's idle bound (`SessionIdleTable`): armed as the reader starts, re-armed by
    /// every frame it reads, and closing the socket when it runs out -- recording that it did, so
    /// the reader's ending is named `SessionEnd::Silent` rather than read as the peer closing.
    ///
    /// A `DeadlineTimer` re-armed per frame rather than a coroutine sleeping beside the reader: a
    /// sleep nothing can cancel would hold the session's end -- and `Stop()` -- until it woke, while
    /// a timer is disarmed by its destructor. It lives in the reader's frame, declared after its
    /// `ReaderEnding`, so it is disarmed before that closes the socket.
    class SessionSilence
    {
      public:
        /// @param loop The session's loop, whose clock the bound is measured on.
        /// @param socket The session's socket, closed when the bound runs out; outlives this.
        /// @param bound How long the session may read nothing; non-positive arms nothing.
        SessionSilence(core::net::EventLoop& loop, core::net::ISocket* socket, std::chrono::milliseconds bound):
            _loop { loop },
            _target { .socket = socket },
            _bound { bound }
        {
            Rearm();
        }

        SessionSilence(SessionSilence const&) = delete;
        SessionSilence(SessionSilence&&) = delete;
        SessionSilence& operator=(SessionSilence const&) = delete;
        SessionSilence& operator=(SessionSilence&&) = delete;
        ~SessionSilence() = default;

        /// Start the bound again from now: a frame was read.
        void Rearm()
        {
            _timer.reset();
            if (_bound > std::chrono::milliseconds::zero())
                _timer.emplace(_loop, _loop.clock().now() + _bound, &Expire, &_target);
        }

        /// @return Whether the bound ran out and closed the socket.
        [[nodiscard]] bool Expired() const noexcept
        {
            return _target.expired;
        }

        /// @return The bound, for the log line.
        [[nodiscard]] std::chrono::milliseconds Bound() const noexcept
        {
            return _bound;
        }

      private:
        /// The timer's callback: recorded BEFORE the close, as `armSocketDeadline` records it, so the
        /// reader the close resumes never sees a socket that shut without the reason attached.
        /// @param state The `SocketDeadlineTarget`.
        static void Expire(void* state)
        {
            auto& target = *static_cast<core::net::SocketDeadlineTarget*>(state);
            target.expired = true;
            target.socket->close();
        }

        core::net::EventLoop& _loop;
        core::net::SocketDeadlineTarget _target;
        std::chrono::milliseconds _bound;
        std::optional<core::net::DeadlineTimer> _timer; ///< Declared last, so it is disarmed first.
    };

    /// Feeds a two-way session's per-frame progress back to the session and the log, so
    /// `ReadProvenSession` itself never has to know either exists -- the dialling end's
    /// counterpart of the server's `SessionObserver`.
    class DiallerSessionObserver final: public IProvenSessionObserver
    {
      public:
        /// @param halves The session; outlives this, which lives in its reader's frame.
        /// @param silence The session's idle bound, re-armed by every frame read; outlives this.
        DiallerSessionObserver(PeerSenderAccess::SessionHalves* halves, SessionSilence* silence) noexcept:
            _halves { halves },
            _silence { silence }
        {
        }

        void OnDelivered() override
        {
            ++_halves->delivered;
            _silence->Rearm();
        }

        void OnSkipped(ConsensusError const& error) override
        {
            _silence->Rearm();
            PeerSenderAccess::NoteSkipped(_halves, error);
        }

      private:
        PeerSenderAccess::SessionHalves* _halves;
        SessionSilence* _silence;
    };
} // namespace

core::async::Task<PeerSenderAccess::Served> PeerSenderAccess::ServeOnce(RaftPeerTransport* self,
                                                                        RaftPeerTransport::Peer* peer)
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

    auto dialed = co_await self->_connector.connect(
        where.host, where.port, core::net::DialOptions { .connectTimeout = self->_options.dialTimeout });
    if (!dialed.has_value())
    {
        // Debug, not Warn. A peer being down is the ordinary condition Raft is
        // built for, and one line per backoff interval per peer at Warn would
        // bury the messages that do need reading.
        self->_logger.Log(
            LogLevel::Debug,
            std::format(
                "raft: could not reach peer {} at {}:{}: {}", where.id, where.host, where.port, dialed.error().context));
        co_return Served { .outcome = Outcome::Retry, .carriedBothWays = false };
    }

    // A stop that arrived while the dial was in flight. The dial cannot be
    // abandoned -- destroying a suspended task frees a frame the reactor still
    // points into -- so it is always awaited to completion and the socket it
    // produced is closed here.
    if (self->_stop.get_token().stop_requested())
    {
        dialed.value()->close();
        co_return Served { .outcome = Outcome::Stop, .carriedBothWays = false };
    }

    peer->socket = std::move(dialed.value());
    SocketRelease const release { peer };

    // A re-address that landed while this dial was in flight closed a socket that
    // did not exist yet, so it closed nothing -- and this connection is to the
    // address the peer has just stopped answering on. Nothing else would notice:
    // the session below is long-lived and never re-reads the address, so the peer
    // would be served at its old address until that connection happened to break.
    if (auto const current = self->AddressOf(*peer); current.host != where.host || current.port != where.port)
        co_return Served { .outcome = Outcome::Retry, .carriedBothWays = false };

    // ONE reader for the connection's whole life, the handshake's and a two-way session's. The
    // acceptor writes its first session frame right behind its verdict, so both can arrive in one
    // read -- and a reader built for the handshake alone would take that frame with it when it
    // was dropped, which no later read could ever return.
    ByteReader reader { *peer->socket, /*maxLineBytes=*/1, DiallerReadCap(self->_options.direction) };

    // Nothing is sent to an address until what answers there has proved the member's id and
    // accepted this node as the member it dialled -- and until then the peer is not
    // CONNECTED either, so `ConnectedPeers()` counts authenticated sessions only.
    auto proven = co_await Handshake(self, peer, &reader, where);
    if (!proven.has_value())
        co_return Served { .outcome = Outcome::Retry, .carriedBothWays = false };

    Session const session { self, where };
    SessionHalves halves { .self = self, .peer = peer, .where = where, .proven = *std::move(proven) };

    // Both halves, or the writer alone: whether the acceptor writes here is the direction's
    // column, the one the acceptor read out of this node's signed proof. The halves share the
    // socket and end together, and BOTH have returned before this line is passed, so neither
    // outlives the socket `release` lets go of once the reader above it is gone.
    if (AcceptorWrites(self->_options.direction))
        co_await core::async::whenAll(ReadSession(&halves, &reader), WriteSession(&halves));
    else
        co_await WriteSession(&halves);

    co_return Served { .outcome = halves.outcome, .carriedBothWays = halves.written != 0 && halves.delivered != 0 };
}

core::async::Task<void> PeerSenderAccess::WriteSession(SessionHalves* halves)
{
    auto* const self = halves->self;
    auto* const peer = halves->peer;
    FrameSealer sealer { std::move(halves->proven.session.diallerToAcceptor) };

    while (true)
    {
        auto frame = co_await peer->outbox.pop();
        if (!frame.has_value())
        {
            halves->outcome = Outcome::Stop; // the outbox was closed
            break;
        }

        // Asked FIRST, before the frame is looked at: once the reader has ended, the session is over
        // whatever the outbox still holds. A real frame popped now -- one `Send` queued in the same
        // turn the reader ended -- is dropped with the session rather than judged: asking the roster
        // about it would count a withdrawn key the reader had already counted, and sealing it would
        // write to a socket the reader had closed.
        if (halves->readerEnded)
        {
            if (!frame->empty())
                self->_dropped.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // The roster, asked again before every frame and every wake: a key revoked since the
        // handshake ends the session here, and the redial is judged against the roster as it is
        // now. A message is dropped with the session, which Raft already survives for any of
        // them. A wake -- with this session's reader alive, one an EARLIER session's reader pushed
        // while its writer was parked in a write rather than here -- is stepped over.
        auto const step = StepFor(self, halves->where.id, halves->proven.provenKey, *frame);
        if (step == SenderStep::Withdraw)
        {
            NoteWithdrawal(halves, "");
            break;
        }
        if (step == SenderStep::StepOver)
            continue;

        SealInPlace(sealer, *frame);

        // Written from a local of this frame, never from a queue element:
        // `core::net::ISocket::Write` requires the buffer to stay at a stable address until
        // the awaitable resumes, and a deque's element addresses do not.
        if (!co_await WriteFrame(peer->socket.get(), *frame))
        {
            self->_dropped.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        ++halves->written;
    }

    // Marked before the close, which is what completes a reader parked on this socket: that
    // reader then finds the writer gone and leaves the outbox alone.
    halves->writerEnded = true;
    peer->socket->close();
}

core::async::Task<void> PeerSenderAccess::ReadSession(SessionHalves* halves, ByteReader* reader)
{
    // First, so it runs last and on every way out, a throw included.
    ReaderEnding const ended { halves };

    auto* const self = halves->self;
    FrameOpener opener { std::move(halves->proven.session.acceptorToDialler) };
    auto const who = ProvenSessionPeer { .id = halves->where.id, .key = halves->proven.provenKey };

    // This end is the PASSIVE one: it writes only what the leader asks of it, so a session whose
    // other end went away while this machine slept would park here forever. The bound is the
    // direction's column -- one-way arms none -- measured from the last frame read.
    SessionSilence silence { self->_reactor,
                             halves->peer->socket.get(),
                             SessionIdleBound(self->_options.direction, self->_options.heartbeatInterval) };
    DiallerSessionObserver observer { halves, &silence };

    // The acceptor's own loop, the one `RaftPeerServer` runs: the tag first, then `StillProves`,
    // then the sender -- so a forget closes this session at the acceptor's next frame, as it
    // does at this node's next one in the writer.
    auto ending =
        co_await ReadProvenSession(reader, &opener, &who, &self->_identity, &self->_sink, SessionReadLimits {}, &observer);
    // Whatever the read reported, a close the bound made is the bound's: only its timer knows.
    if (silence.Expired())
        ending =
            SessionEnding { .end = SessionEnd::Silent, .detail = std::format("nothing for {} ms", silence.Bound().count()) };
    NoteSessionEnd(halves, ending);
}

void PeerSenderAccess::ReleaseSocket(RaftPeerTransport::Peer* peer) noexcept
{
    if (peer->socket == nullptr)
        return;
    peer->socket->close();
    peer->socket.reset();
}

void PeerSenderAccess::EndReader(SessionHalves* halves) noexcept
{
    halves->readerEnded = true;

    // A writer still running is parked on the outbox or in a write. The close completes the
    // write; the zero-length frame wakes the pop, which nothing else would until a message came.
    // Asked AFTER the close, which may itself have finished a parked writer: a frame pushed for a
    // writer that has gone would be left for the next session to step over.
    halves->peer->socket->close();
    if (!halves->writerEnded)
    {
        auto const pushed = halves->peer->outbox.push(std::vector<std::byte> {});
        halves->self->_dropped.fetch_add(pushed.displaced, std::memory_order_relaxed);
    }
}

PeerSenderAccess::SenderStep PeerSenderAccess::StepFor(RaftPeerTransport* self,
                                                       NodeId const& peer,
                                                       Ed25519PublicKey const& key,
                                                       std::vector<std::byte> const& popped)
{
    if (!self->_identity.StillProves(peer, key))
    {
        if (!popped.empty())
            self->_dropped.fetch_add(1, std::memory_order_relaxed);
        return SenderStep::Withdraw;
    }
    return popped.empty() ? SenderStep::StepOver : SenderStep::Write;
}

void PeerSenderAccess::NoteWithdrawal(SessionHalves* halves, std::string_view detail)
{
    if (std::exchange(halves->withdrawalCounted, true))
        return;
    halves->self->NoteDialRefusal(*halves->peer, halves->where, DiallerRefusal::KeyWithdrawn, detail);
}

void PeerSenderAccess::NoteSessionEnd(SessionHalves* halves, SessionEnding const& ending)
{
    auto const& row = DiallerSessionEndRows[static_cast<std::size_t>(ending.end)];
    auto* const self = halves->self;
    auto const& where = halves->where;

    // The withdrawal is the one ending the writer can count as well, so it goes through the
    // session's own once-only count; every other ending only the reader can see.
    if (row.refusal == DiallerRefusal::KeyWithdrawn)
    {
        NoteWithdrawal(halves, ending.detail);
        return;
    }

    if (row.refusal.has_value())
    {
        self->NoteDialRefusal(*halves->peer, where, *row.refusal, ending.detail);
        return;
    }

    // PeerClosed: silent BY ROW, as the acceptor's is. The session's own line says it ended.
    if (!row.level.has_value())
        return;

    self->_logger.Log(*row.level,
                      std::format("raft: peer {} at {}:{} {}{}{}; closing",
                                  where.id,
                                  where.host,
                                  where.port,
                                  row.sentence,
                                  row.sentence.empty() || ending.detail.empty() ? "" : ": ",
                                  ending.detail));
}

void PeerSenderAccess::NoteSkipped(SessionHalves const* halves, ConsensusError const& error)
{
    halves->self->_logger.Log(LogLevel::Debug,
                              std::format("raft: skipped a frame from peer {} on the session this node dialled: {}",
                                          halves->where.id,
                                          error.context));
}

core::async::Task<std::optional<ProvenAcceptor>> PeerSenderAccess::Handshake(RaftPeerTransport* self,
                                                                             RaftPeerTransport::Peer* peer,
                                                                             ByteReader* reader,
                                                                             PeerEndpoint where)
{
    using ReadOutcome = HandshakeFrameRead::Outcome;
    auto* const socket = peer->socket.get();

    // Armed before the first read, so the bound covers the whole exchange: an acceptor
    // that never challenges -- a build from before the handshake, which only ever reads --
    // and one that never answers the proof are both a connection this sender would
    // otherwise sit on forever, with nothing queued for it reaching anybody.
    core::net::SocketDeadlineTarget expiry { .socket = socket };
    auto deadline = core::net::armSocketDeadline(&self->_reactor, self->_options.handshakeBound, &expiry);

    auto const challengeRead = co_await ReadHandshakeFrame<RaftWire::MessageType::Challenge>(reader);
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
    // The direction is SIGNED into the proof: one-way, this node writes and the peer answers over
    // its own dial, which is how every voter reaches every other; two-way, the acceptor writes
    // back here, which is how a learner nobody dials is reached.
    auto begun = DiallerHandshake::Create(self->_identity, where.id, self->_options.direction, self->_random);
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

    auto const verdictRead = co_await ReadHandshakeFrame<RaftWire::MessageType::Verdict>(reader);
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
            // SIGNED by a member that proved its id, so it is a fact about this node rather than
            // an address's say-so -- and the one way a learner that was offline through its forget
            // hears of it. Told on this thread; the observer records and returns.
            self->NoteDialRefusal(*peer, where, DiallerRefusal::OwnKeyRevoked, "");
            if (self->_onOwnKeyRevoked)
                self->_onOwnKeyRevoked(conclusion.acceptor);
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

core::async::Task<void> PeerSenderAccess::RunSender(RaftPeerTransport* self, RaftPeerTransport::Peer* peer)
{
    self->_reactorThread.store(std::this_thread::get_id(), std::memory_order_relaxed);

    auto const token = self->_stop.get_token();
    auto const direction = self->_options.direction;
    auto delay = DialBackoffOf(direction).initial;
    while (!token.stop_requested())
    {
        auto served = Served {};
        try
        {
            served = co_await ServeOnce(self, peer);
        }
        catch (...)
        {
            // The hazard inverts rather than disappearing. With a `core::async::DetachedTask`
            // an escaped exception is std::terminate -- a node killed over one
            // peer. With an unawaited `core::async::Task<void>` it is stored in the promise
            // and never read, so the sender would simply end, forever, with
            // nothing logged anywhere: "three nodes sat at undecided with no
            // error" spelled for one peer. So it is caught and reported.
            self->NoteSenderThrew(peer->endpoint.id);
        }

        if (served.outcome == Outcome::Stop || token.stop_requested())
            break;

        // A session that carried a frame each way reached a peer that is up, so the wait starts
        // again from the row's first; one that did not has waited longer each time, to the cap.
        // Doubling a one-way row changes nothing, its cap being its first wait. A pinned wait
        // overrides the table.
        if (served.carriedBothWays)
            delay = DialBackoffOf(direction).initial;
        auto const wait =
            self->_options.reconnectBackoff > std::chrono::milliseconds::zero() ? self->_options.reconnectBackoff : delay;
        delay = NextBackoff(direction, delay);

        // Backed off after EVERY session, not only a failed dial. The thread
        // version redialled immediately after a dropped connection, so a peer
        // that accepts and instantly resets produced a tight
        // connect/write-fail/connect loop with no sleep in it. On a private
        // thread that burned one core; on the shared reactor it starves the
        // election timers, which is the "nine role changes in twelve seconds"
        // failure this repository already has a name for.
        //
        // One park, which the stop cancels (#1596): no `stopWakeBound` steps any more.
        auto backoff = core::net::interruptibleSleepUntil(&self->_reactor, token, self->_reactor.clock().now() + wait);
        if (co_await std::move(backoff) == core::net::WakeReason::Cancelled)
            break;
    }

    peer->socket.reset();
    self->NoteSenderFinished();
    co_return;
}

core::async::DetachedTask PeerSenderAccess::RunInboundSender(RaftPeerTransport* self,
                                                             std::shared_ptr<RaftPeerTransport::InboundPeer> inbound)
{
    self->_reactorThread.store(std::this_thread::get_id(), std::memory_order_relaxed);

    auto& link = *inbound->link;
    while (true)
    {
        auto frame = co_await inbound->outbox->pop();
        if (!frame.has_value())
            break; // detached, replaced by a newer session, or stopping

        // The roster, asked again before every frame and every wake, by the step the dialling
        // sender asks it through. A key withdrawn since the proof ends the session here: closing
        // the socket ends the server's reader too, which detaches this link. Counted on the
        // ACCEPTOR's row, since this is the acceptor's session -- whichever of its reader and this
        // sender noticed first, and the other then sees only a closed socket. A wake is what
        // `RecheckProofs` pushes a session nothing is being sent on, so that it is asked at all.
        auto const step = StepFor(self, link.Peer(), link.ProvenKey(), *frame);
        if (step == SenderStep::Withdraw)
        {
            auto const& row = RowFor(AcceptorRefusal::KeyWithdrawn);
            self->_metrics.Increment(row.counter);
            self->_logger.Log(
                LogLevel::Warn,
                std::format("raft: closed the session peer {} dialled in on, because {}", link.Peer(), row.says));
            link.Socket().close();
            break;
        }
        if (step == SenderStep::StepOver)
            continue;

        SealInPlace(link.Sealer(), *frame);

        // A write that fails is a session that has ended, whatever the reader has noticed yet;
        // closing it makes the reader notice now, which is what detaches the link.
        if (!co_await WriteFrame(&link.Socket(), *frame))
        {
            self->_dropped.fetch_add(1, std::memory_order_relaxed);
            link.Socket().close();
            break;
        }
    }

    // Closed by the sender ITSELF, whichever way it ended: after a failed write or a withdrawn
    // key the entry stays attached until the server's reader notices the close, and until then a
    // push would be accepted into a queue nobody pops -- a message lost and never counted. Closed,
    // it refuses the push, which `Send` counts as a drop for want of a session.
    inbound->outbox->close();

    // The share is let go BEFORE the count, because the count is what lets `Stop()` return
    // and the transport be destroyed: nothing of this frame may be left to release after it.
    inbound.reset();
    self->NoteInboundSenderFinished();
}

core::async::DetachedTask PeerSenderAccess::CloseSockets(RaftPeerTransport* self, std::optional<NodeId> only)
{
    // The hop is the point. `core::net::ISocket::close` retires the socket's registration
    // with the loop, which only the loop's thread may touch, and the sender it wakes then
    // resumes in that loop's drain, on the reactor, never inside `close()` (core-cpp 0.2.1,
    // guarantee G2). Closing from the thread calling Stop() would race the reactor on the
    // first and, before 0.2.1, ran the sender on the wrong thread on epoll and kqueue.
    co_await core::async::ResumeOn { self->_reactor };

    // Collected under the lock, closed outside it. The woken sender resumes in the loop's
    // next drain rather than inside `close()` (G2), so the lock would no longer be held
    // across sender code -- but a close is still no place to hold a lock that `Send`, and
    // therefore the driver's mutex, can wait behind. `Peer::socket` is reactor-thread-only,
    // so nothing but the map lookup needs the lock at all.
    //
    // An attached session is closed on a stop as well: a sender parked in a write to a learner
    // that stopped reading completes only when its socket closes. Held by its link rather than
    // by pointer, so the socket outlives this loop whoever detaches it meanwhile.
    auto closing = std::vector<core::net::ISocket*> {};
    auto attached = std::vector<std::shared_ptr<RaftSessionLink>> {};
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
            attached.reserve(self->_inbound.size());
            for (auto const& [id, inbound]: self->_inbound)
                attached.push_back(inbound->link);
        }
    }

    for (auto* const socket: closing)
        if (socket != nullptr)
            socket->close();
    for (auto const& link: attached)
        link->Socket().close();
    co_return;
}

RaftPeerTransport::RaftPeerTransport(std::vector<PeerEndpoint> peers,
                                     core::net::EventLoop& reactor,
                                     core::net::IConnector& connector,
                                     IRaftMessageSink& inbound,
                                     ILogger& logger,
                                     IMetricsSink& metrics,
                                     IRaftPeerIdentity const& identity,
                                     ISecureRandom& random,
                                     PeerTransportOptions options):
    _identity { identity },
    _self { identity.Self() },
    _reactor { reactor },
    _connector { connector },
    _sink { inbound },
    _logger { logger },
    _metrics { metrics },
    _random { random },
    _options { options }
{
    for (auto& endpoint: peers)
        Learn(std::move(endpoint));
}

void RaftPeerTransport::RecheckProofs()
{
    // The outboxes of the sessions that no longer prove, collected under the lock and woken outside
    // it, as `Send` pushes. Only the OUTBOX is shared off the reactor -- never the link, which owns
    // the socket (`InboundPeer`).
    auto withdrawn = std::vector<std::shared_ptr<InboundOutbox>> {};
    {
        auto const guard = std::shared_lock { _peersMutex };
        for (auto const& [id, inbound]: _inbound)
            if (!_identity.StillProves(inbound->link->Peer(), inbound->link->ProvenKey()))
                withdrawn.push_back(inbound->outbox);
    }
    // A full outbox gives up its oldest entry for the wake, as it does for `Send`'s message and for
    // `EndReader`'s wake, and that loss is counted like theirs.
    for (auto const& outbox: withdrawn)
    {
        auto const pushed = outbox->push(std::vector<std::byte> {});
        _dropped.fetch_add(pushed.displaced, std::memory_order_relaxed);
    }
}

void RaftPeerTransport::ObserveOwnKeyRevoked(OwnKeyRevokedObserver observer)
{
    _onOwnKeyRevoked = std::move(observer);
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
            auto const inserted = _peers.emplace(id, std::make_unique<Peer>(std::move(where), _reactor, OutboxOptions()));
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
        // the test and the submission. Safe because `core::async::ResumeOn` never resumes inline
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

core::async::AsyncQueueOptions RaftPeerTransport::OutboxOptions() const noexcept
{
    return core::async::AsyncQueueOptions { .capacity = _options.maxQueuedPerPeer,
                                            .overflow = core::async::AsyncQueueOverflow::DropOldest };
}

void RaftPeerTransport::Attach(std::shared_ptr<RaftSessionLink> link)
{
    auto const peer = link->Peer();
    auto inbound = std::make_shared<InboundPeer>(std::move(link), _reactor, OutboxOptions());
    auto replaced = std::shared_ptr<InboundPeer> {};
    {
        // The same lock and the same value `RequestStop` moves: a session attached after it
        // would start a sender `Stop()` has already stopped waiting for.
        auto const guard = std::unique_lock { _peersMutex };
        if (_lifecycle == Lifecycle::Stopping)
            return;
        replaced = std::exchange(_inbound[peer], inbound);
        _inboundSendersRunning.fetch_add(1, std::memory_order_acq_rel);
    }

    // Outside the lock, as every close and every start here is. The same id proving a newer
    // session supersedes the older one (the map is keyed by id), so it is ENDED rather than left open: its sender may be
    // parked in a write to a learner that stopped reading, which only a close completes -- and
    // once out of `_inbound`, a stop would never close it. Closing it ends the server's reader,
    // whose detach then finds the newer link and leaves it. On the reactor's thread, as `Attach`
    // always is.
    //
    // Counted, because the one fault this names cannot be told apart by any single event: a
    // roaming learner supersedes once per reconnect, and two machines holding ONE identity key take
    // the session from each other for as long as both run. Only the RATE separates them, so the
    // count is what an operator reads -- and no condition claims a clone from it.
    if (replaced != nullptr)
    {
        replaced->outbox->close();
        replaced->link->Socket().close();
        _metrics.Increment(IMetricsSink::Counter::RaftInboundSessionsSuperseded);
        _logger.Log(LogLevel::Info,
                    std::format("raft: peer {} proved a second two-way session; its earlier one is closed", peer));
    }

    _logger.Log(LogLevel::Info,
                std::format("raft: peer {} dialled in two-way; this node writes to it on that session", peer));
    PeerSenderAccess::RunInboundSender(this, std::move(inbound));
}

void RaftPeerTransport::Detach(RaftSessionLink const& link) noexcept
{
    auto removed = std::shared_ptr<InboundPeer> {};
    {
        auto const guard = std::unique_lock { _peersMutex };
        if (auto const found = _inbound.find(link.Peer()); found != _inbound.end() && found->second->link.get() == &link)
        {
            removed = std::move(found->second);
            _inbound.erase(found);
        }
    }

    if (removed != nullptr)
    {
        removed->outbox->close();
        _logger.Log(LogLevel::Info, std::format("raft: peer {}'s two-way session ended", link.Peer()));
    }
}

std::size_t RaftPeerTransport::InboundLinks() const noexcept
{
    auto const guard = std::shared_lock { _peersMutex };
    return _inbound.size();
}

std::size_t RaftPeerTransport::InboundSendersRunning() const noexcept
{
    return _inboundSendersRunning.load(std::memory_order_acquire);
}

void RaftPeerTransport::NoteInboundSenderFinished() noexcept
{
    _inboundSendersRunning.fetch_sub(1, std::memory_order_acq_rel);
}

void RaftPeerTransport::NoteSendDrop(NodeId const& to, SendDrop drop) noexcept
{
    auto const& row = SendDropRows[static_cast<std::size_t>(drop)];
    _dropped.fetch_add(1, std::memory_order_relaxed);
    _sendDrops[static_cast<std::size_t>(drop)].fetch_add(1, std::memory_order_relaxed);
    _metrics.Increment(row.counter);
    _logger.Log(LogLevel::Debug, std::format("raft: dropped a message for peer {}: {}", to, row.says));
}

void RaftPeerTransport::LearnDialsIn(std::vector<NodeId> peers)
{
    auto placed = std::set<NodeId>(std::make_move_iterator(peers.begin()), std::make_move_iterator(peers.end()));
    auto const guard = std::unique_lock { _peersMutex };
    _dialsIn = std::move(placed);
}

std::size_t RaftPeerTransport::PeerCount() const noexcept
{
    auto const guard = std::shared_lock { _peersMutex };
    return _peers.size();
}

std::vector<NodeId> RaftPeerTransport::DialTargets() const
{
    auto const guard = std::shared_lock { _peersMutex };
    auto ids = std::vector<NodeId> {};
    ids.reserve(_peers.size());
    std::ranges::copy(_peers | std::views::keys, std::back_inserter(ids)); // a `std::map`: in id order
    return ids;
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
    _reactor.submit(peer.sender.handle());
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
    _stop.request_stop();

    // `Stopping` and the outbox closures under one lock, so a `Learn` racing this
    // either finishes first -- and has its outbox closed by the loop below -- or
    // sees `Stopping` and refuses. Closing an outbox takes only that queue's own
    // lock, which nothing here holds in the other order.
    {
        auto const guard = std::unique_lock { _peersMutex };
        _lifecycle = Lifecycle::Stopping;
        for (auto& [id, peer]: _peers)
            peer->outbox.close();
        for (auto const& [id, inbound]: _inbound)
            inbound->outbox->close();
    }

    if (AnySenderRunning())
        PeerSenderAccess::CloseSockets(this, std::nullopt);
}

void RaftPeerTransport::Stop() noexcept
{
    RequestStop();

    // Nothing to wait for means nothing to deadlock on, so this is safe from any
    // thread -- which is what lets a single-threaded test drive the reactor
    // itself, drain the senders with `RequestStop()`, and then let the destructor
    // call this without tripping the check below.
    if (!AnySenderRunning())
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
    auto const outcome = DrainWithin([this] { return AnySenderRunning(); });

    // Only the dialled senders' frames are released below: an attached session's sender is
    // detached, so its frame is nobody's to release -- it is named in the count all the same.
    //
    // THE HAZARD, and it is not only theirs: a sender still running past this ceiling, of either
    // kind, holds `this`. Leaking its frame keeps its OWN memory valid, but the transport is
    // destroyed once `Stop()` returns, so a sender resumed after that dereferences freed memory.
    // Accepted for parity with the dialled senders rather than restructured here: reaching the
    // ceiling at all means a socket close did not complete a parked write, which is the defect
    // to chase, and the Error line below is what names it.
    if (auto const stuck =
            _sendersRunning.load(std::memory_order_acquire) + _inboundSendersRunning.load(std::memory_order_acquire);
        outcome == DrainResult::Ceiling && stuck != 0)
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
            std::ignore = peer->sender.release();
    }
}

bool RaftPeerTransport::AnySenderRunning() const noexcept
{
    return _sendersRunning.load(std::memory_order_acquire) != 0
           || _inboundSendersRunning.load(std::memory_order_acquire) != 0;
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
    auto const now = _reactor.clock().now();
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

    auto const now = _reactor.clock().now();
    if (now < peer.nextRefusalReport)
        return;
    peer.nextRefusalReport = now + RefusalReportInterval;

    _logger.Log(row.level,
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
    if (_stop.get_token().stop_requested())
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
    auto attachedOutbox = std::shared_ptr<InboundOutbox> {};
    auto unreached = SendDrop::UnknownPeer;
    {
        // The lookup is under the lock and the push below is not. A dialled peer is only
        // ever added or re-addressed, never erased, so the pointer stays valid for
        // this transport's life -- which is what lets the queue's own lock be taken
        // outside this one rather than nested inside it. An attached session CAN be
        // erased, so its OUTBOX is held by a share of its own -- never the entry, which
        // owns the link and so the socket: this thread is not the reactor's, and the
        // last share of a socket must not be let go here (`InboundPeer`).
        //
        // A dialled peer first: a member reached both ways is one this node was told to
        // dial. Never a dial of a peer it was not given.
        auto const guard = std::shared_lock { _peersMutex };
        if (auto const found = _peers.find(to); found != _peers.end())
            target = found->second.get();
        else if (auto const attached = _inbound.find(to); attached != _inbound.end())
            attachedOutbox = attached->second->outbox;
        else if (_dialsIn.contains(to))
            unreached = SendDrop::NoSession;
    }

    if (target == nullptr && attachedOutbox == nullptr)
    {
        NoteSendDrop(to, unreached);
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

    // Never resumes the sender inline -- see `core::async::AsyncQueue`. This matters here
    // specifically because `Send` is reached from `RaftDriver::Deliver`, which
    // holds the driver's mutex.
    auto& outbox = target != nullptr ? target->outbox : *attachedOutbox;
    auto const pushed = outbox.push(std::move(frame));
    _dropped.fetch_add(pushed.displaced, std::memory_order_relaxed);

    // An attached session detached between the lookup and the push: its outbox is closed, so
    // the message reached no session. The peer had dialled in -- that is what attached it --
    // so it is the no-session row whatever `LearnDialsIn` last said.
    if (attachedOutbox != nullptr && pushed.admission == core::async::AsyncQueueAdmission::Refused && pushed.displaced == 0)
        NoteSendDrop(to, SendDrop::NoSession);
}

} // namespace FastCache::Consensus

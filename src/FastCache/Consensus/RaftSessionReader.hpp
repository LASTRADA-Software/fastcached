// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftMessageSink.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <core/async/Task.hpp>

namespace FastCache::Consensus
{

/// Why a proven Raft peer session's read loop ended.
///
/// **Private: never transmitted or persisted.** Shared by both ends of a session:
/// `RaftPeerServer` reads the session it accepted, and the dialling end (#1596's
/// distributed-node tasks) reads the session it opened, off the socket it dialled --
/// so a fix to how a frame is checked reaches both through this one enum rather than
/// through two copies that can drift apart.
enum class SessionEnd : std::uint8_t
{
    PeerClosed,   ///< The peer closed, or a frame arrived truncated: an ordinary end.
    BadMagic,     ///< A frame's header did not decode: nothing to resynchronize to.
    OverCap,      ///< A frame declared more payload than this end will buffer.
    BadTag,       ///< A frame's tag did not verify.
    KeyWithdrawn, ///< The roster no longer names the key this session was proved with.
    WrongSender,  ///< A verified message named a sender other than the proven peer.
    Unreadable,   ///< The payload decoded to nothing this build can read.

    /// Nothing was read for the session's idle bound, so this end closed it. Never produced by
    /// `ReadProvenSession` itself: the end that ARMED the bound -- a two-way dialler
    /// (`SessionIdleTable`) -- names it, since only its timer knows the close was its own.
    Silent,

    Last, ///< Not an ending, and has no row.
};

/// Why a session ended, and what a caller's log line says about it.
struct SessionEnding
{
    SessionEnd end { SessionEnd::PeerClosed }; ///< Which ending.

    /// What was seen, for a caller's log line to append to its own sentence for `end`.
    /// Empty where the ending needs nothing beyond its own name.
    std::string detail;
};

/// How one END of a session counts and logs a `SessionEnding`: a row of that end's own table.
///
/// One shape for both ends, and a table per end, because what differs is exactly the refusal
/// enum a row moves and the sentence it says: an acceptor's row counts an `AcceptorRefusal` and
/// speaks of the peer that dialled it, a dialler's counts a `DiallerRefusal` and speaks of the
/// peer it dialled. `ReadProvenSession` hands back only the FACTS an ending carries, so neither
/// end inherits a sentence written for the other.
/// @tparam Refusal The refusal enum this end counts under.
template <typename Refusal>
struct SessionEndRow
{
    /// The ending this row describes. `Last` until a row names one, which no table's order check
    /// accepts, so a row left unnamed cannot pass for a real ending.
    SessionEnd end { SessionEnd::Last };

    std::optional<Refusal> refusal; ///< The counter it moves, when it moves one.

    /// The level a log-only ending is reported at, or `std::nullopt` for one that is silent BY
    /// DESIGN -- a row fact, never inferred from an empty sentence and an empty detail, which a
    /// future ending with a sentence but no detail would satisfy by accident.
    std::optional<LogLevel> level;

    std::string_view sentence; ///< What happened, before `SessionEnding::detail` is appended.
};

/// Who a proven session belongs to, and what its frames may claim.
struct ProvenSessionPeer
{
    NodeId id;               ///< The member this session proved.
    Ed25519PublicKey key {}; ///< The key it proved that with; re-checked on every frame.
};

/// Limits one session's reader is held to.
struct SessionReadLimits
{
    /// Largest frame payload this end will buffer from a peer that has proved its id.
    ///
    /// The wire's length field is a u32, so a peer -- or something that is not a peer at
    /// all -- can declare four gigabytes. Without a cap the declared length *is* the
    /// allocation, which makes a single frame a memory-exhaustion vector.
    std::size_t maxFrameBytes { 8U * 1024U * 1024U };
};

/// Told about a proven session's ordinary per-frame progress: what `ReadProvenSession` decoded.
///
/// The counter and the log line a real connection wants for a delivery, and for a skipped
/// frame, both come from the same one fact each -- so they share one seam here rather than
/// `ReadProvenSession` reaching for a counter AND a logger of its own. That is also what lets
/// the dialling end (#1596's distributed-node tasks) produce the identical diagnostic for the
/// identical frame, through its own implementation of this rather than a second copy of the
/// log line's wording.
class IProvenSessionObserver
{
  public:
    IProvenSessionObserver() = default;
    IProvenSessionObserver(IProvenSessionObserver const&) = delete;
    IProvenSessionObserver(IProvenSessionObserver&&) = delete;
    IProvenSessionObserver& operator=(IProvenSessionObserver const&) = delete;
    IProvenSessionObserver& operator=(IProvenSessionObserver&&) = delete;
    virtual ~IProvenSessionObserver() = default;

    /// A message was decoded and handed to the sink.
    virtual void OnDelivered() = 0;

    /// A frame's type was not one this build knows, so it was stepped over rather than ending
    /// the session -- the ordinary condition during a rolling upgrade.
    /// @param error What the decode reported; carries the frame's own description.
    virtual void OnSkipped(ConsensusError const& error) = 0;
};

/// Read, open, re-check and deliver every frame of one proven Raft peer session, until it ends.
///
/// The whole of what either end of a session does once a handshake has produced a
/// `ProvenSessionPeer` and a `FrameOpener` over its key: read a header, cap its
/// declared length before the payload is buffered, read the payload and its tag,
/// open it, ask the roster whether it still names `peer`'s key as its own, decode
/// the message, and check that it names the peer that proved this session -- in
/// that order, because a check made before the tag verifies is a check on a claim
/// nothing has backed, and every check after it is a check on bytes this session's
/// own key produced.
///
/// A frame whose type this build does not know is stepped over once its tag has
/// verified, rather than ending the session: a peer running a newer build is the
/// ordinary condition during a rolling upgrade.
///
/// Every pointer must outlive the coroutine; pointers, never references, because a
/// reference parameter of a coroutine is bound before the first suspension and would
/// outlive nothing across one. That is also why no label (a peer's address, say) is taken
/// here as a `std::string_view`: a view borrowed by a coroutine parameter is bound before the
/// first suspension exactly as a reference is, and a caller that wants one in its own log line
/// carries it on its own `IProvenSessionObserver`, on its own side of this call.
/// @param reader Reads the session's frames.
/// @param opener Opens each frame's tag against the session's key.
/// @param peer Who this session was proved as, and under which key.
/// @param identity Asked, on every frame, whether the roster still names `peer`'s key as
///        its own -- how an applied forget closes a session its revoked key proved, at its
///        next frame.
/// @param sink Where a delivered message goes.
/// @param limits Frame limits to enforce.
/// @param observer Told about each delivery and each skip.
/// @return Why the session ended; the caller maps it onto its own refusal counters and log lines.
[[nodiscard]] core::async::Task<SessionEnding> ReadProvenSession(ByteReader* reader,
                                                                 FrameOpener* opener,
                                                                 ProvenSessionPeer const* peer,
                                                                 IRaftPeerIdentity const* identity,
                                                                 IRaftMessageSink* sink,
                                                                 SessionReadLimits limits,
                                                                 IProvenSessionObserver* observer);

} // namespace FastCache::Consensus

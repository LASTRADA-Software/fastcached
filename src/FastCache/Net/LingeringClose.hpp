// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace FastCache
{

/// How long, and how much, a closing server listens to a peer it has stopped answering.
///
/// Time is bounded two ways because two sockets wait two ways: a reactor socket's reads
/// SUSPEND, so the whole drain is one deadline; a blocking socket's reads BLOCK, so each
/// one carries `total / reads` and the read count caps the sum, which makes the worst case
/// `total` on both. The byte cap is the third bound and the one a fast peer meets: never
/// more than serving the request would have read.
struct LingerBounds
{
    /// The whole drain, from the half-close to the close. Non-positive arms nothing, for a
    /// surface whose own sweep already bounds a socket it has refused.
    std::chrono::milliseconds total;

    /// Bytes discarded at most. Past them the close is a reset after all -- the price of a
    /// bound, and never a promise to read an unbounded upload in order to refuse it.
    std::size_t maxBytes;

    /// Reads at most, which is what bounds a blocking socket's total.
    std::size_t reads;
};

/// How a lingering close ended. Private to its callers and their tests: never stored or
/// sent, so its order is free to change.
enum class LingerEnd : std::uint8_t
{
    AlreadyClosed, ///< Nothing to listen to: the socket was closed before the linger began.
    PeerFinished,  ///< The peer closed or half-closed: EOF, the ordinary ending.
    PeerFailed,    ///< A read failed on its own: a reset, or the socket gone under the read.
    Expired,       ///< The bound ran out before the peer finished.
    ReadCap,       ///< The reads ran out first, so a peer still sending meets a reset.
    ByteCap,       ///< The bytes ran out first, so a peer still sending meets a reset.
};

/// How a lingering close ended, and after how many reads.
struct LingerOutcome
{
    LingerEnd end;     ///< How it ended.
    std::size_t reads; ///< The reads it made, the last one included.
};

/// Half-close, listen to the peer until it closes too or a bound runs out, then close.
///
/// **A close with the peer's bytes still unread is an RST, not a FIN**, and an RST
/// destroys what the peer had not read yet. Measured on loopback (#1553): Windows drops
/// every byte already buffered for the reader and reports the reset at once; Linux and
/// macOS hand those bytes over first. So a server that answers a request it did not
/// finish reading -- a `431` over a head past its cap, a `-ERR Protocol error` over a
/// value past its limit, a `408` to a head that dribbled -- and then closes, delivers
/// that answer to a Linux client and a reset to a Windows one. The refusal is the whole
/// point of answering at all, and it was lost exactly where it was aimed.
///
/// So the close lingers, the way web servers have for decades: half-close first, so the
/// answer is followed by a FIN and a well-behaved peer closes on reading it; discard what
/// the peer is still sending; close once it has closed, or once the bounds say stop. A
/// peer that closed first costs ONE read, which reports EOF or the reset at once -- so an
/// ordinary goodbye is never turned into a wait, and the outcome says so. A peer
/// that keeps sending past the bounds meets the reset anyway -- bounded best effort, and
/// never a promise to read an unbounded upload in order to refuse it.
///
/// A socket that is already closed is only closed again, which is a no-op.
/// @param socket The socket to close; must outlive the task.
/// @param reactor Where a reactor socket's deadline is armed, or nullptr for a blocking
///        or in-memory one.
/// @param bounds How long and how much to listen.
/// @return A task that completes once the socket is closed, with how the linger ended.
[[nodiscard]] Task<LingerOutcome> CloseLingering(ISocket* socket, IReactor* reactor, LingerBounds bounds);

} // namespace FastCache

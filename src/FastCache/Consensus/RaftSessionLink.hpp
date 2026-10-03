// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/SessionSeal.hpp>

#include <memory>
#include <utility>

#include <core/net/ISocket.hpp>

namespace FastCache::Consensus
{

/// What a session's direction means to the end that ACCEPTED it.
struct SessionDirectionRow
{
    RaftWire::SessionDirection direction; ///< The direction this row describes.

    /// Whether the acceptor writes on the connection: attaches it to its transport as the way
    /// to reach the dialler. Only a peer nobody dials asks for that, because a voter's replies
    /// ride the other voter's own dial and an acceptor writing there as well would be a second
    /// writer nobody asked for.
    bool acceptorWrites;
};

/// One row per `RaftWire::SessionDirection`, in enumerator order.
inline constexpr EnumTable<RaftWire::SessionDirection, SessionDirectionRow> SessionDirectionRows { {
    { .direction = RaftWire::SessionDirection::OneWay, .acceptorWrites = false },
    { .direction = RaftWire::SessionDirection::TwoWay, .acceptorWrites = true },
} };

static_assert(RowsInEnumeratorOrder(SessionDirectionRows, &SessionDirectionRow::direction),
              "SessionDirectionRows must hold one row per SessionDirection, in enumerator order");

/// Whether an acceptor writes on a session the dialler proved with @p direction.
/// @param direction The direction the dialler's signed proof asked for.
/// @return True when the acceptor attaches the session and writes to the dialler on it.
[[nodiscard]] constexpr bool AcceptorWrites(RaftWire::SessionDirection direction) noexcept
{
    return SessionDirectionRows[static_cast<std::size_t>(direction)].acceptorWrites;
}

/// One accepted TWO-WAY session the acceptor may write on. Shared by the server's read loop and
/// the transport's sender for it; the socket lives as long as either still holds the link.
///
/// ## One reader and one writer, never more
///
/// A socket has one read operation and one write operation, and core-cpp ends the process for a
/// second of either (`contract::claimReadSlot`, `claimWriteSlot`). So the roles are fixed: the
/// server's loop only READS this socket once the handshake is over, and only the transport's
/// sender for this link WRITES on it -- after the server has finished writing its verdict, which
/// is before the link exists at all. The `FrameSealer` is that sender's alone for the same reason:
/// its position is the count of frames written, and one writer is what keeps it true.
///
/// ## Destroyed on the reactor
///
/// Every owner of a link lets go of it on the one reactor the server and the transport are built
/// on: the server's connection coroutine, and on the transport's side its map entry, the link's
/// sender and a stop's snapshot of the links. So the socket is destroyed on that reactor's thread
/// whichever of them lets go last. `RaftPeerTransport::Send`, which runs on any thread, must
/// never hold one -- it holds only the link's outbox, for exactly this reason.
class RaftSessionLink
{
  public:
    /// @param peer The member the dialler proved.
    /// @param provenKey The key it proved that with.
    /// @param socket The accepted connection.
    /// @param sealer Seals what the acceptor writes: the session's `acceptorToDialler` key.
    RaftSessionLink(NodeId peer,
                    Ed25519PublicKey provenKey,
                    std::shared_ptr<core::net::ISocket> socket,
                    FrameSealer sealer) noexcept:
        _peer { std::move(peer) },
        _provenKey { provenKey },
        _socket { std::move(socket) },
        _sealer { std::move(sealer) }
    {
    }

    RaftSessionLink(RaftSessionLink const&) = delete;
    RaftSessionLink(RaftSessionLink&&) = delete;
    RaftSessionLink& operator=(RaftSessionLink const&) = delete;
    RaftSessionLink& operator=(RaftSessionLink&&) = delete;
    ~RaftSessionLink() = default;

    /// @return The member the dialler proved.
    [[nodiscard]] NodeId const& Peer() const noexcept
    {
        return _peer;
    }

    /// @return The key it proved that with -- what `StillProves` is re-asked about.
    [[nodiscard]] Ed25519PublicKey const& ProvenKey() const noexcept
    {
        return _provenKey;
    }

    /// @return The accepted connection.
    [[nodiscard]] core::net::ISocket& Socket() const noexcept
    {
        return *_socket;
    }

    /// @return The sealer for what the acceptor writes. Written only by the one sender coroutine.
    [[nodiscard]] FrameSealer& Sealer() noexcept
    {
        return _sealer;
    }

  private:
    NodeId _peer;
    Ed25519PublicKey _provenKey;
    std::shared_ptr<core::net::ISocket> _socket;
    FrameSealer _sealer;
};

} // namespace FastCache::Consensus

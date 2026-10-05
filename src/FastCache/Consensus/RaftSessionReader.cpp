// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftSessionReader.hpp>
#include <FastCache/Consensus/RaftWire.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

namespace FastCache::Consensus
{

namespace
{
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

core::async::Task<SessionEnding> ReadProvenSession(ByteReader* reader,
                                                   FrameOpener* opener,
                                                   ProvenSessionPeer const* peer,
                                                   IRaftPeerIdentity const* identity,
                                                   IRaftMessageSink* sink,
                                                   SessionReadLimits limits,
                                                   IProvenSessionObserver* observer)
{
    while (true)
    {
        auto const headerBytes = co_await reader->ReadExactly(RaftWire::HeaderSize);
        if (!headerBytes.has_value())
            co_return SessionEnding { .end = SessionEnd::PeerClosed, .detail = {} }; // EOF, or the peer went away.

        auto const header = RaftWire::DecodeHeader(*headerBytes);
        if (!header.has_value())
        {
            // A wrong magic is the one condition under which the reader cannot find
            // where this frame ends. There is nothing to resynchronize to, so every
            // later byte would be a guess.
            co_return SessionEnding { .end = SessionEnd::BadMagic, .detail = {} };
        }

        if (header->payloadLength > limits.maxFrameBytes)
        {
            // Refused BEFORE the payload is buffered, exactly as the compile-cache
            // handler's cap is: checking afterwards would let a peer force the very
            // allocation the cap exists to deny, once per frame. `detail` carries only the
            // two numbers -- the phrasing around them is the caller's, so a dialler's log
            // line is not phrased as an acceptor's.
            co_return SessionEnding { .end = SessionEnd::OverCap,
                                      .detail = std::format(
                                          "{} bytes, cap {} bytes", header->payloadLength, limits.maxFrameBytes) };
        }

        auto const body = co_await reader->ReadExactly(std::size_t { header->payloadLength } + RaftWire::TagSize);
        if (!body.has_value())
            co_return SessionEnding { .end = SessionEnd::PeerClosed, .detail = {} };

        auto const bytes = std::span<std::byte const> { *body };
        auto const payload = bytes.first(header->payloadLength);

        // The tag BEFORE anything the frame says is acted on -- before its type decides
        // whether it is stepped over, and before its message reaches the node.
        if (!opener->Open(*headerBytes, payload, TagOf(bytes.last(RaftWire::TagSize))))
            co_return SessionEnding { .end = SessionEnd::BadTag, .detail = {} };

        // The roster, asked again for every frame: a key revoked since the handshake ends the
        // session here, at the first frame after the decision, rather than whenever the
        // connection happens to break. Asked after the tag, so a frame nobody sealed is counted
        // as what it is.
        if (!identity->StillProves(peer->id, peer->key))
            co_return SessionEnding { .end = SessionEnd::KeyWithdrawn, .detail = {} };

        auto decoded = RaftWire::DecodeMessage(*header, payload);
        if (decoded.has_value())
        {
            // The member the session proved is the one that speaks on it. A message
            // naming another sender is refused rather than delivered under that name:
            // everything the node decides about a sender -- whom it votes for, whose
            // entries it takes -- is then a fact about the key, not about a field.
            if (auto const& sender = SenderOf(*decoded); sender != peer->id)
                co_return SessionEnding { .end = SessionEnd::WrongSender,
                                          .detail = std::format("the message named {}", sender) };

            sink->Deliver(*std::move(decoded));
            observer->OnDelivered();
            continue;
        }

        // The payload and its tag have been consumed and verified, so the reader is still
        // in sync -- which is the whole point of the declared length.
        if (decoded.error().code == ConsensusErrorCode::UnknownMessageType)
        {
            // Stepped over, not fatal. A peer running a newer build is the ordinary
            // condition during a rolling upgrade, and ending the session here would
            // partition this node from every peer ahead of it.
            observer->OnSkipped(decoded.error());
            continue;
        }

        // A malformed payload, a handshake frame out of place, or a version other than
        // the one the handshake settled: this reader and that sender disagree about the
        // bytes, so the session is no longer trustworthy even though this frame was
        // consumed cleanly. `detail` is the decoder's own description, not a sentence built
        // around it -- see the `OverCap` comment above.
        co_return SessionEnding { .end = SessionEnd::Unreadable, .detail = decoded.error().context };
    }
}

} // namespace FastCache::Consensus

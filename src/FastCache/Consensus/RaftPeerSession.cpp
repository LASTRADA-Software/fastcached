// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace FastCache::Consensus
{

namespace
{
    /// The fields a dialler's proof MACs, in wire order.
    ///
    /// One function per construction rather than one per end, for
    /// `DiscoveryWire::Detail::ProofFields`' reason: a signer and a verifier that each
    /// spell the list are a signer and a verifier that will one day spell it
    /// differently, which presents as every node failing to prove a key they all hold.
    ///
    /// The spans BORROW from every argument, so the result is consumed inside the full
    /// expression that built it -- the shape `.agent/rules/wire-and-protocol.md` records
    /// as having been a use-after-free twice.
    /// @param nonces Both ends' nonces.
    /// @param dialler Who is dialling.
    /// @param target Whom it dialled.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 4> ProofFields(SessionNonces const& nonces,
                                                                        std::string_view dialler,
                                                                        std::string_view target) noexcept
    {
        return { std::span<std::byte const> { nonces.acceptor },
                 std::span<std::byte const> { nonces.dialler },
                 WireFields::AsBytes(dialler),
                 WireFields::AsBytes(target) };
    }

    /// The fields an acceptor's verdict MACs, in wire order. Borrows, as `ProofFields`.
    /// @param nonces Both ends' nonces.
    /// @param dialler Who dialled.
    /// @param acceptor Which member answered.
    /// @param verdict The verdict's one byte.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 5> VerdictFields(SessionNonces const& nonces,
                                                                          std::string_view dialler,
                                                                          std::string_view acceptor,
                                                                          std::array<std::byte, 1> const& verdict) noexcept
    {
        return { std::span<std::byte const> { nonces.acceptor },
                 std::span<std::byte const> { nonces.dialler },
                 WireFields::AsBytes(dialler),
                 WireFields::AsBytes(acceptor),
                 std::span<std::byte const> { verdict } };
    }

    /// The fields a session frame MACs, in wire order. Borrows, as `ProofFields`.
    /// @param nonces The session's nonces.
    /// @param position The frame's big-endian position in its session.
    /// @param header The frame's header.
    /// @param payload The frame's payload.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 5> FrameFields(
        SessionNonces const& nonces,
        std::array<std::byte, sizeof(std::uint64_t)> const& position,
        std::span<std::byte const> header,
        std::span<std::byte const> payload) noexcept
    {
        return { std::span<std::byte const> { nonces.acceptor },
                 std::span<std::byte const> { nonces.dialler },
                 std::span<std::byte const> { position },
                 header,
                 payload };
    }

    /// The one byte a verdict travels as.
    /// @param verdict The verdict.
    /// @return Its byte, as `RaftWire::EncodeVerdict` writes it.
    [[nodiscard]] std::array<std::byte, 1> VerdictByte(RaftWire::HandshakeVerdict verdict) noexcept
    {
        return RaftWire::Detail::EnumField(verdict);
    }

    /// What an acceptor decides about a proof whose MAC has ALREADY verified.
    ///
    /// Own id before wrong target, because a dialler claiming this node's id is the more
    /// specific diagnosis whatever it dialled: two machines answering to one identity.
    /// @param proof The proof.
    /// @param self This node's id.
    /// @return The decision; never `Refused`, which is the MAC's.
    [[nodiscard]] ProofOutcome DecideProvenProof(RaftWire::ProofFrame const& proof, NodeId const& self) noexcept
    {
        if (proof.dialler == self)
            return ProofOutcome::OwnId;
        if (proof.target != self)
            return ProofOutcome::WrongTarget;
        return ProofOutcome::Accepted;
    }

    /// The verdict that travels for a decision about a proven proof.
    /// @param outcome The decision; `Refused` has no verdict and is never passed.
    /// @return The verdict.
    [[nodiscard]] RaftWire::HandshakeVerdict VerdictFor(ProofOutcome outcome) noexcept
    {
        switch (outcome)
        {
            case ProofOutcome::OwnId:
                return RaftWire::HandshakeVerdict::OwnId;
            case ProofOutcome::WrongTarget:
                return RaftWire::HandshakeVerdict::WrongTarget;
            case ProofOutcome::Accepted:
                return RaftWire::HandshakeVerdict::Accepted;
            case ProofOutcome::Refused:
                // No verdict is sent for a refusal; this arm exists so a new outcome is a
                // compile error rather than a silent acceptance, and it answers with a
                // refusal if it is ever reached.
                break;
        }
        return RaftWire::HandshakeVerdict::WrongTarget;
    }

    /// Whether @p id could be carried by a handshake at all.
    /// @param id The id.
    /// @return True when non-empty, within `MaxHandshakeIdBytes`, and text.
    [[nodiscard]] bool CarriableId(std::string_view id) noexcept
    {
        return !id.empty() && id.size() <= RaftWire::MaxHandshakeIdBytes && IsValidUtf8(id);
    }
} // namespace

AcceptorHandshake::AcceptorHandshake(IRaftPeerCredential const& credential, NodeId self, IRandomSource& random):
    _credential { credential },
    _self { std::move(self) },
    _challenge { .nonce = DrawNonce(random) }
{
}

RaftWire::ChallengeFrame const& AcceptorHandshake::Challenge() const noexcept
{
    return _challenge;
}

AcceptorHandshake::Judgement AcceptorHandshake::Judge(RaftWire::ProofFrame const& proof)
{
    auto const nonces = SessionNonces { .acceptor = _challenge.nonce, .dialler = proof.nonce };

    // Spent before anything else, whatever happens next: a nonce that could answer twice
    // is a nonce that can be replayed.
    if (std::exchange(_spent, true))
        return Judgement { .outcome = ProofOutcome::Refused, .verdict = std::nullopt, .nonces = nonces, .dialler = {} };

    // The MAC FIRST, and nothing else is read before it: the two refusals below name
    // what the proof claimed, and a claim is unauthenticated until its tag checks out --
    // so asking them first would make each one an answer to somebody holding no key.
    if (!_credential.Verify(RaftPeerMac::DiallerProof, ProofFields(nonces, proof.dialler, proof.target), proof.tag))
        return Judgement { .outcome = ProofOutcome::Refused, .verdict = std::nullopt, .nonces = nonces, .dialler = {} };

    auto const outcome = DecideProvenProof(proof, _self);
    auto const verdict = VerdictFor(outcome);

    auto const decided = VerdictByte(verdict);
    return Judgement {
        .outcome = outcome,
        .verdict = RaftWire::VerdictFrame { .verdict = verdict,
                                            .acceptor = _self,
                                            .tag = _credential.Sign(RaftPeerMac::AcceptorVerdict,
                                                                    VerdictFields(nonces, proof.dialler, _self, decided)) },
        .nonces = nonces,
        .dialler = proof.dialler,
    };
}

DiallerHandshake::DiallerHandshake(IRaftPeerCredential const& credential, NodeId self, NodeId target, IRandomSource& random):
    _credential { credential },
    _self { std::move(self) },
    _target { std::move(target) },
    _nonce { DrawNonce(random) }
{
}

std::expected<RaftWire::ProofFrame, std::string> DiallerHandshake::Answer(RaftWire::ChallengeFrame const& challenge)
{
    // Refused HERE rather than sent: an acceptor refuses such an id as a malformed proof
    // and closes, and this node would then read that close as a wrong key -- every
    // connection, every peer, forever, naming the one cause it is not.
    if (!CarriableId(_self))
        return std::unexpected { std::format(
            "this node's id is {} bytes or not text, which a Raft handshake cannot carry (at most {} bytes of UTF-8)",
            _self.size(),
            RaftWire::MaxHandshakeIdBytes) };
    if (!CarriableId(_target))
        return std::unexpected { std::format(
            "the peer id this node dials is {} bytes or not text, which a Raft handshake cannot carry (at most {} bytes "
            "of UTF-8)",
            _target.size(),
            RaftWire::MaxHandshakeIdBytes) };

    _answered = challenge.nonce;
    auto const nonces = SessionNonces { .acceptor = challenge.nonce, .dialler = _nonce };
    return RaftWire::ProofFrame { .dialler = _self,
                                  .target = _target,
                                  .nonce = _nonce,
                                  .tag = _credential.Sign(RaftPeerMac::DiallerProof, ProofFields(nonces, _self, _target)) };
}

DiallerHandshake::Conclusion DiallerHandshake::Conclude(RaftWire::VerdictFrame const& verdict) const
{
    // A verdict with no challenge answered answers nothing this node asked.
    if (!_answered.has_value())
        return Conclusion { .outcome = VerdictOutcome::Forged, .acceptor = {}, .nonces = {} };

    auto const nonces = SessionNonces { .acceptor = *_answered, .dialler = _nonce };
    auto const decided = VerdictByte(verdict.verdict);

    // The MAC first, for the acceptor's reason: what the verdict says is a claim until then.
    if (!_credential.Verify(
            RaftPeerMac::AcceptorVerdict, VerdictFields(nonces, _self, verdict.acceptor, decided), verdict.tag))
        return Conclusion { .outcome = VerdictOutcome::Forged, .acceptor = {}, .nonces = nonces };

    auto outcome = VerdictOutcome::Accepted;
    switch (verdict.verdict)
    {
        case RaftWire::HandshakeVerdict::WrongTarget:
            outcome = VerdictOutcome::WrongTarget;
            break;
        case RaftWire::HandshakeVerdict::OwnId:
            outcome = VerdictOutcome::OwnId;
            break;
        case RaftWire::HandshakeVerdict::Accepted:
            // An acceptance naming some other member is not one this node can act on: its
            // frames would go to a member it did not mean. No acceptor of this build sends
            // one -- it accepts only as itself, and only when it is the target -- so it is
            // read as the wrong member having answered, which is what it says.
            outcome = verdict.acceptor == _target ? VerdictOutcome::Accepted : VerdictOutcome::WrongTarget;
            break;
        case RaftWire::HandshakeVerdict::Last:
            // Never decoded (`DecodeWireEnum`), and named rather than defaulted so a new
            // verdict is a compile error here.
            outcome = VerdictOutcome::Forged;
            break;
    }
    return Conclusion { .outcome = outcome, .acceptor = verdict.acceptor, .nonces = nonces };
}

FrameSealer::FrameSealer(IRaftPeerCredential const& credential, SessionNonces nonces) noexcept:
    _credential { credential },
    _nonces { nonces }
{
}

Sha256::Digest FrameSealer::Seal(std::span<std::byte const> frame)
{
    // Every frame `RaftWire::Encode` produces carries a header; a shorter span is a
    // programmer error, and sealing it as an empty header keeps it one the opener refuses.
    auto const headerSize = std::min(frame.size(), RaftWire::HeaderSize);
    auto const position = WireFields::ToBigEndian<std::uint64_t>(_next++);
    return _credential.Sign(RaftPeerMac::Frame,
                            FrameFields(_nonces, position, frame.first(headerSize), frame.subspan(headerSize)));
}

FrameOpener::FrameOpener(IRaftPeerCredential const& credential, SessionNonces nonces) noexcept:
    _credential { credential },
    _nonces { nonces }
{
}

bool FrameOpener::Open(std::span<std::byte const> header, std::span<std::byte const> payload, Sha256::Digest const& tag)
{
    auto const position = WireFields::ToBigEndian<std::uint64_t>(_next);
    if (!_credential.Verify(RaftPeerMac::Frame, FrameFields(_nonces, position, header, payload), tag))
        return false;
    ++_next;
    return true;
}

} // namespace FastCache::Consensus

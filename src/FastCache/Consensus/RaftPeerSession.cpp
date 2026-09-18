// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace FastCache::Consensus
{

namespace
{
    /// What binds a session key to this wire and this construction. Versioned for the reason the
    /// signature labels are: changing it retires every session in flight, which is a stated act.
    constexpr std::string_view SessionKeyLabel = "fastcache-raft-session-v2";

    /// The fields a dialler's proof signs, in wire order: everything the handshake has carried
    /// up to the proof's own signature.
    ///
    /// One function per construction rather than one per end, for
    /// `DiscoveryWire::Detail::ProofFields`' reason: a signer and a verifier that each spell the
    /// list are a signer and a verifier that will one day spell it differently, which presents as
    /// every node failing to prove an id they all hold keys for.
    ///
    /// The spans BORROW from every argument, so the result is consumed inside the full
    /// expression that built it -- the shape `.agent/rules/wire-and-protocol.md` records as
    /// having been a use-after-free twice.
    /// @param challenge The acceptor's challenge.
    /// @param proof The dialler's proof; its signature is not among the fields.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 6> ProofTranscript(RaftWire::ChallengeFrame const& challenge,
                                                                            RaftWire::ProofFrame const& proof) noexcept
    {
        return { std::span<std::byte const> { challenge.nonce },
                 std::span<std::byte const> { challenge.ephemeral },
                 WireFields::AsBytes(proof.dialler),
                 WireFields::AsBytes(proof.target),
                 std::span<std::byte const> { proof.nonce },
                 std::span<std::byte const> { proof.ephemeral } };
    }

    /// The fields an acceptor's verdict signs, in wire order: the whole proof transcript, the
    /// proof's own signature, then the verdict. Borrows, as `ProofTranscript`.
    /// @param challenge The acceptor's challenge.
    /// @param proof The dialler's proof, signature included.
    /// @param verdict The verdict's one byte.
    /// @param acceptor Which member answered.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 9> VerdictTranscript(RaftWire::ChallengeFrame const& challenge,
                                                                              RaftWire::ProofFrame const& proof,
                                                                              std::array<std::byte, 1> const& verdict,
                                                                              std::string_view acceptor) noexcept
    {
        auto const head = ProofTranscript(challenge, proof);
        return { head[0],
                 head[1],
                 head[2],
                 head[3],
                 head[4],
                 head[5],
                 std::span<std::byte const> { proof.signature },
                 std::span<std::byte const> { verdict },
                 WireFields::AsBytes(acceptor) };
    }

    /// The one byte a verdict travels as.
    /// @param verdict The verdict.
    /// @return Its byte, as `RaftWire::EncodeVerdict` writes it.
    [[nodiscard]] std::array<std::byte, 1> VerdictByte(RaftWire::HandshakeVerdict verdict) noexcept
    {
        return RaftWire::Detail::EnumField(verdict);
    }

    /// The session key both ends derive, or nothing when no secret could be agreed.
    ///
    /// Salted with both nonces and bound to both ephemeral keys AND both ids, so a key agreed
    /// in one exchange is the key of no other -- even one that somehow reused an ephemeral key.
    /// @param ownSecret This end's ephemeral secret.
    /// @param peerEphemeral The other end's ephemeral public key.
    /// @param challenge The acceptor's challenge.
    /// @param proof The dialler's proof.
    /// @return The key, or nothing: a low-order peer key fixes the "shared" secret to a value
    ///         everybody knows, and `X25519SharedSecret` refuses it.
    [[nodiscard]] std::optional<SessionKey> AgreeSessionKey(SecureByteBuffer const& ownSecret,
                                                            X25519PublicKey const& peerEphemeral,
                                                            RaftWire::ChallengeFrame const& challenge,
                                                            RaftWire::ProofFrame const& proof)
    {
        auto const shared = X25519SharedSecret(ownSecret, peerEphemeral);
        if (!shared.has_value())
            return std::nullopt;

        std::array<std::byte, 2 * NonceBytes> salt {};
        std::ranges::copy(challenge.nonce, salt.begin());
        std::ranges::copy(proof.nonce, salt.begin() + NonceBytes);

        auto const info = WireFields::Encode({ WireFields::AsBytes(SessionKeyLabel),
                                               std::span<std::byte const> { challenge.ephemeral },
                                               std::span<std::byte const> { proof.ephemeral },
                                               WireFields::AsBytes(proof.dialler),
                                               WireFields::AsBytes(proof.target) });
        auto key = DeriveSessionKey(*shared, salt, info);
        if (!key.has_value())
            return std::nullopt;
        return *std::move(key);
    }

    /// A fresh ephemeral key pair.
    /// @param random Where the secret comes from.
    /// @return The secret and its public half, or why nothing could be drawn.
    [[nodiscard]] std::expected<std::pair<SecureByteBuffer, X25519PublicKey>, SecureRandomError> DrawEphemeral(
        ISecureRandom& random)
    {
        SecureByteBuffer secret(X25519KeyBytes);
        if (auto drawn = random.Fill(secret); !drawn.has_value())
            return std::unexpected { drawn.error() };

        // Cannot fail: any 32 bytes are an X25519 secret, and these are 32. Were it ever to, the
        // all-zero public key stands in, which the other end's `X25519SharedSecret` refuses as
        // low-order -- so a failure here is a handshake that fails, never a session with a known
        // key.
        auto const ephemeral = X25519PublicKeyFrom(secret).value_or(X25519PublicKey {});
        return std::pair { std::move(secret), ephemeral };
    }

    /// What an acceptor decides about a proof whose signature has ALREADY verified.
    ///
    /// Own id before wrong target, because a dialler that proved this node's id is the more
    /// specific diagnosis whatever it dialled: two machines holding one private key.
    /// @param proof The proof.
    /// @param self This node's id.
    /// @return The decision; never one of the signature's.
    [[nodiscard]] ProofOutcome DecideProvenProof(RaftWire::ProofFrame const& proof, NodeId const& self) noexcept
    {
        if (proof.dialler == self)
            return ProofOutcome::OwnId;
        if (proof.target != self)
            return ProofOutcome::WrongTarget;
        return ProofOutcome::Accepted;
    }

    /// The verdict that travels for a decision that owes one.
    /// @param outcome The decision; never one answered with nothing.
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
            case ProofOutcome::RevokedKey:
                return RaftWire::HandshakeVerdict::KeyRevoked;
            case ProofOutcome::Forged:
            case ProofOutcome::UnknownKey:
                // No verdict is sent for these; the arms exist so a new outcome is a compile
                // error rather than a silent acceptance, and they answer with a refusal if they
                // are ever reached.
                break;
        }
        return RaftWire::HandshakeVerdict::WrongTarget;
    }

    /// The outcome a signature check that did NOT prove the id stands for.
    /// @param check What the check concluded; never `Verified`.
    /// @return The refusal.
    [[nodiscard]] ProofOutcome RefusalFor(SignerCheck check) noexcept
    {
        switch (check)
        {
            case SignerCheck::Unknown:
                return ProofOutcome::UnknownKey;
            case SignerCheck::Revoked:
                return ProofOutcome::RevokedKey;
            case SignerCheck::Verified:
            case SignerCheck::Forged:
                break;
        }
        return ProofOutcome::Forged;
    }

    /// Whether @p id could be carried by a handshake at all.
    /// @param id The id.
    /// @return True when non-empty, within `MaxHandshakeIdBytes`, and text.
    [[nodiscard]] bool CarriableId(std::string_view id) noexcept
    {
        return !id.empty() && id.size() <= RaftWire::MaxHandshakeIdBytes && IsValidUtf8(id);
    }
} // namespace

std::expected<AcceptorHandshake, SecureRandomError> AcceptorHandshake::Create(IRaftPeerIdentity const& identity,
                                                                              ISecureRandom& random)
{
    auto nonce = DrawNonce(random);
    if (!nonce.has_value())
        return std::unexpected { nonce.error() };
    auto ephemeral = DrawEphemeral(random);
    if (!ephemeral.has_value())
        return std::unexpected { ephemeral.error() };
    return AcceptorHandshake { identity,
                               RaftWire::ChallengeFrame { .nonce = *nonce, .ephemeral = ephemeral->second },
                               std::move(ephemeral->first) };
}

AcceptorHandshake::AcceptorHandshake(IRaftPeerIdentity const& identity,
                                     RaftWire::ChallengeFrame const& challenge,
                                     SecureByteBuffer ephemeralSecret):
    _identity { identity },
    _challenge { challenge },
    _ephemeralSecret { std::move(ephemeralSecret) }
{
}

RaftWire::ChallengeFrame const& AcceptorHandshake::Challenge() const noexcept
{
    return _challenge;
}

AcceptorHandshake::Judgement AcceptorHandshake::Judge(RaftWire::ProofFrame const& proof)
{
    auto const refused = [](ProofOutcome outcome) {
        return Judgement { .outcome = outcome, .verdict = std::nullopt, .dialler = {}, .provenKey = {}, .session = {} };
    };

    // Spent before anything else, whatever happens next: a challenge that could answer twice is
    // a challenge that can be replayed.
    if (std::exchange(_spent, true))
        return refused(ProofOutcome::Forged);

    // The signature FIRST, and nothing the proof claims is reported on before it: the refusals
    // below name what the proof claimed, and a claim is unauthenticated until its signature
    // checks out. The claimed id does choose WHICH key to check under -- there is no other way
    // to ask -- and so `UnknownKey` is answered with nothing rather than signed.
    auto const signer = _identity.Verify(
        RaftPeerSignature::DiallerProof, proof.dialler, ProofTranscript(_challenge, proof), proof.signature);
    if (signer.check != SignerCheck::Verified && signer.check != SignerCheck::Revoked)
        return refused(RefusalFor(signer.check));

    auto const& self = _identity.Self();
    auto const outcome = signer.check == SignerCheck::Revoked ? ProofOutcome::RevokedKey : DecideProvenProof(proof, self);

    // Agreed BEFORE the verdict is signed, so an Accepted verdict is never sent for a session
    // this end cannot open. A proven member whose ephemeral key is low-order sent something no
    // honest dialler does; it is refused as a proof that did not hold up.
    auto session = std::optional<SessionKey> {};
    if (outcome == ProofOutcome::Accepted)
    {
        session = AgreeSessionKey(_ephemeralSecret, proof.ephemeral, _challenge, proof);
        if (!session.has_value())
            return refused(ProofOutcome::Forged);
    }

    auto const verdict = VerdictFor(outcome);
    auto const decided = VerdictByte(verdict);
    return Judgement {
        .outcome = outcome,
        .verdict =
            RaftWire::VerdictFrame { .verdict = verdict,
                                     .acceptor = self,
                                     .signature = _identity.Sign(RaftPeerSignature::AcceptorVerdict,
                                                                 VerdictTranscript(_challenge, proof, decided, self)) },
        .dialler = outcome == ProofOutcome::RevokedKey ? NodeId {} : proof.dialler,
        .provenKey =
            outcome == ProofOutcome::Accepted || outcome == ProofOutcome::RevokedKey ? signer.key : Ed25519PublicKey {},
        .session = std::move(session),
    };
}

std::expected<DiallerHandshake, SecureRandomError> DiallerHandshake::Create(IRaftPeerIdentity const& identity,
                                                                            NodeId target,
                                                                            ISecureRandom& random)
{
    auto nonce = DrawNonce(random);
    if (!nonce.has_value())
        return std::unexpected { nonce.error() };
    auto ephemeral = DrawEphemeral(random);
    if (!ephemeral.has_value())
        return std::unexpected { ephemeral.error() };
    return DiallerHandshake { identity, std::move(target), *nonce, std::move(ephemeral->first), ephemeral->second };
}

DiallerHandshake::DiallerHandshake(IRaftPeerIdentity const& identity,
                                   NodeId target,
                                   Nonce const& nonce,
                                   SecureByteBuffer ephemeralSecret,
                                   X25519PublicKey const& ephemeral):
    _identity { identity },
    _target { std::move(target) },
    _nonce { nonce },
    _ephemeralSecret { std::move(ephemeralSecret) },
    _ephemeral { ephemeral }
{
}

std::expected<RaftWire::ProofFrame, std::string> DiallerHandshake::Answer(RaftWire::ChallengeFrame const& challenge)
{
    auto const& self = _identity.Self();

    // Refused HERE rather than sent: an acceptor refuses such an id as a malformed proof and
    // closes, and this node would then read that close as its key being unknown -- every
    // connection, every peer, forever, naming the one cause it is not.
    if (!CarriableId(self))
        return std::unexpected { std::format(
            "this node's id is {} bytes or not text, which a Raft handshake cannot carry (at most {} bytes of UTF-8)",
            self.size(),
            RaftWire::MaxHandshakeIdBytes) };
    if (!CarriableId(_target))
        return std::unexpected { std::format(
            "the peer id this node dials is {} bytes or not text, which a Raft handshake cannot carry (at most {} bytes "
            "of UTF-8)",
            _target.size(),
            RaftWire::MaxHandshakeIdBytes) };

    auto proof = RaftWire::ProofFrame {
        .dialler = self, .target = _target, .nonce = _nonce, .ephemeral = _ephemeral, .signature = {}
    };
    proof.signature = _identity.Sign(RaftPeerSignature::DiallerProof, ProofTranscript(challenge, proof));

    _challenge = challenge;
    _proof = proof;
    return proof;
}

DiallerHandshake::Conclusion DiallerHandshake::Conclude(RaftWire::VerdictFrame const& verdict) const
{
    auto const forged = [](VerdictOutcome outcome) {
        return Conclusion { .outcome = outcome, .acceptor = {}, .provenKey = {}, .session = {} };
    };

    // A verdict with no challenge answered answers nothing this node asked.
    if (!_challenge.has_value() || !_proof.has_value())
        return forged(VerdictOutcome::Forged);

    auto const decided = VerdictByte(verdict.verdict);

    // The signature first, for the acceptor's reason: what the verdict says is a claim until
    // then. The id it names chooses the key, as the proof's did.
    auto const signer = _identity.Verify(RaftPeerSignature::AcceptorVerdict,
                                         verdict.acceptor,
                                         VerdictTranscript(*_challenge, *_proof, decided, verdict.acceptor),
                                         verdict.signature);
    switch (signer.check)
    {
        case SignerCheck::Verified:
            break;
        case SignerCheck::Forged:
            return forged(VerdictOutcome::Forged);
        case SignerCheck::Unknown:
            return forged(VerdictOutcome::AcceptorKeyUnknown);
        case SignerCheck::Revoked:
            return forged(VerdictOutcome::AcceptorKeyRevoked);
    }

    auto const answered = [&](VerdictOutcome outcome) {
        return Conclusion { .outcome = outcome, .acceptor = verdict.acceptor, .provenKey = {}, .session = {} };
    };

    switch (verdict.verdict)
    {
        case RaftWire::HandshakeVerdict::WrongTarget:
            return answered(VerdictOutcome::WrongTarget);
        case RaftWire::HandshakeVerdict::OwnId:
            return answered(VerdictOutcome::OwnId);
        case RaftWire::HandshakeVerdict::KeyRevoked:
            return answered(VerdictOutcome::OwnKeyRevoked);
        case RaftWire::HandshakeVerdict::Accepted:
            break;
        case RaftWire::HandshakeVerdict::Last:
            // Never decoded (`DecodeWireEnum`), and named rather than defaulted so a new verdict
            // is a compile error here.
            return forged(VerdictOutcome::Forged);
    }

    // An acceptance naming some other member is not one this node can act on: its frames would
    // go to a member it did not mean. No acceptor of this build sends one -- it accepts only as
    // itself, and only when it is the target -- so it is read as the wrong member having
    // answered, which is what it says.
    if (verdict.acceptor != _target)
        return answered(VerdictOutcome::WrongTarget);

    auto session = AgreeSessionKey(_ephemeralSecret, _challenge->ephemeral, *_challenge, *_proof);
    if (!session.has_value())
        return forged(VerdictOutcome::Forged);

    return Conclusion { .outcome = VerdictOutcome::Accepted,
                        .acceptor = verdict.acceptor,
                        .provenKey = signer.key,
                        .session = std::move(session) };
}

} // namespace FastCache::Consensus

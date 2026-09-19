// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Distributed/NodeProof.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What binds a session key to this wire and this construction. Versioned for the reason the
    /// signature labels are: changing it retires every session in flight, which is a stated act.
    constexpr std::string_view SessionKeyLabel = "fastcache-node-session-v1";

    /// Which direction a session key seals, as HKDF's info spells it. Distinct, so the two keys of
    /// one connection are never the same key.
    constexpr std::string_view CallerToServerLabel = "caller-to-server";
    constexpr std::string_view ServerToCallerLabel = "server-to-caller";

    /// The fields the server's signature covers, in wire order: the caller's half, then the
    /// server's up to its signature.
    ///
    /// One function per construction rather than one per end, for the Raft handshake's reason: a
    /// signer and a verifier that each spell the list are a signer and a verifier that will one day
    /// spell it differently. The spans BORROW from every argument, so the result is consumed inside
    /// the full expression that built it.
    /// @param request The caller's opening.
    /// @param reply The server's answer; its signature is not among the fields.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 6> ChallengeTranscript(
        Wire::NodeChallengeRequest const& request, Wire::NodeChallengeReply const& reply) noexcept
    {
        return { std::span<std::byte const> { request.nonce }, std::span<std::byte const> { request.ephemeral },
                 WireFields::AsBytes(reply.serverId),          std::span<std::byte const> { reply.serverKey },
                 std::span<std::byte const> { reply.nonce },   std::span<std::byte const> { reply.ephemeral } };
    }

    /// The fields the caller's signature covers: the whole challenge transcript, the server's
    /// signature, then the caller's id and key. Borrows, as `ChallengeTranscript`.
    /// @param request The caller's opening.
    /// @param reply The server's answer, signature included.
    /// @param clientId The id the caller claims.
    /// @param clientKey The key it signs with.
    /// @return The fields.
    [[nodiscard]] std::array<std::span<std::byte const>, 9> ProofTranscript(Wire::NodeChallengeRequest const& request,
                                                                            Wire::NodeChallengeReply const& reply,
                                                                            std::string_view clientId,
                                                                            Ed25519PublicKey const& clientKey) noexcept
    {
        auto const head = ChallengeTranscript(request, reply);
        return { head[0],
                 head[1],
                 head[2],
                 head[3],
                 head[4],
                 head[5],
                 std::span<std::byte const> { reply.signature },
                 WireFields::AsBytes(clientId),
                 std::span<std::byte const> { clientKey } };
    }

    /// What is signed for @p purpose: its label, then the transcript, as one encoded message.
    /// @param purpose Which signature.
    /// @param transcript The handshake's fields, in wire order.
    /// @return The message.
    [[nodiscard]] std::vector<std::byte> SignedMessage(NodeProofSignature purpose,
                                                       std::span<std::span<std::byte const> const> transcript)
    {
        std::vector<std::span<std::byte const>> labelled;
        labelled.reserve(transcript.size() + 1);
        labelled.push_back(WireFields::AsBytes(NodeProofSignatureLabels[static_cast<std::size_t>(purpose)].label));
        labelled.insert(labelled.end(), transcript.begin(), transcript.end());
        return WireFields::Encode(WireFields::FieldList { labelled });
    }

    /// One session key for one direction.
    /// @param shared The X25519 output.
    /// @param salt Both nonces.
    /// @param direction Which way it seals.
    /// @param request The caller's opening.
    /// @param reply The server's answer.
    /// @param clientId The id the caller proved.
    /// @return The key, or nothing when HKDF refused -- which a caller treats as a failed handshake.
    [[nodiscard]] std::optional<SessionKey> DirectionKey(std::span<std::byte const> shared,
                                                         std::span<std::byte const> salt,
                                                         std::string_view direction,
                                                         Wire::NodeChallengeRequest const& request,
                                                         Wire::NodeChallengeReply const& reply,
                                                         std::string_view clientId)
    {
        auto const info = WireFields::Encode({ WireFields::AsBytes(SessionKeyLabel),
                                               WireFields::AsBytes(direction),
                                               std::span<std::byte const> { request.ephemeral },
                                               std::span<std::byte const> { reply.ephemeral },
                                               WireFields::AsBytes(reply.serverId),
                                               WireFields::AsBytes(clientId) });
        auto key = DeriveSessionKey(shared, salt, info);
        if (!key.has_value())
            return std::nullopt;
        return *std::move(key);
    }
} // namespace

std::expected<NodeEphemeral, SecureRandomError> DrawNodeEphemeral(ISecureRandom& random)
{
    SecureByteBuffer secret(X25519KeyBytes);
    if (auto drawn = random.Fill(secret); !drawn.has_value())
        return std::unexpected { drawn.error() };

    // Cannot fail: any 32 bytes are an X25519 secret, and these are 32. Were it ever to, the all-zero
    // public key stands in, which the other end's `X25519SharedSecret` refuses as low-order -- so a
    // failure here is a handshake that fails, never a session with a known key.
    auto const publicKey = X25519PublicKeyFrom(secret).value_or(X25519PublicKey {});
    return NodeEphemeral { .secret = std::move(secret), .publicKey = publicKey };
}

Wire::NodeChallengeReply AnswerNodeChallenge(Ed25519KeyPair const& identity,
                                             Wire::NodeChallengeRequest const& request,
                                             ServerHello const& hello)
{
    auto reply = Wire::NodeChallengeReply {
        .serverId = std::string { hello.serverId }, .serverKey = {}, .nonce = {}, .ephemeral = {}, .signature = {}
    };
    std::ranges::copy(identity.PublicKey(), reply.serverKey.begin());
    std::ranges::copy(hello.nonce, reply.nonce.begin());
    std::ranges::copy(hello.ephemeral, reply.ephemeral.begin());
    auto const transcript = ChallengeTranscript(request, reply);
    auto const signature = identity.Sign(SignedMessage(NodeProofSignature::ServerChallenge, transcript));
    std::ranges::copy(signature, reply.signature.begin());
    return reply;
}

bool VerifyNodeChallengeReply(Wire::NodeChallengeRequest const& request, Wire::NodeChallengeReply const& reply)
{
    auto const transcript = ChallengeTranscript(request, reply);
    auto key = Ed25519PublicKey {};
    std::ranges::copy(reply.serverKey, key.begin());
    auto signature = Ed25519Signature {};
    std::ranges::copy(reply.signature, signature.begin());
    return Ed25519Verify(key, SignedMessage(NodeProofSignature::ServerChallenge, transcript), signature);
}

Wire::ProveNodeRequest MintNodeProof(Ed25519KeyPair const& identity,
                                     std::string_view clientId,
                                     Wire::NodeChallengeRequest const& request,
                                     Wire::NodeChallengeReply const& reply)
{
    auto proof = Wire::ProveNodeRequest { .nodeId = std::string { clientId }, .publicKey = {}, .signature = {} };
    std::ranges::copy(identity.PublicKey(), proof.publicKey.begin());
    auto const transcript = ProofTranscript(request, reply, clientId, identity.PublicKey());
    auto const signature = identity.Sign(SignedMessage(NodeProofSignature::NodeProof, transcript));
    std::ranges::copy(signature, proof.signature.begin());
    return proof;
}

bool VerifyNodeProof(Wire::NodeChallengeRequest const& request,
                     Wire::NodeChallengeReply const& reply,
                     Wire::ProveNodeRequest const& proof)
{
    auto key = Ed25519PublicKey {};
    std::ranges::copy(proof.publicKey, key.begin());
    auto signature = Ed25519Signature {};
    std::ranges::copy(proof.signature, signature.begin());
    auto const transcript = ProofTranscript(request, reply, proof.nodeId, key);
    return Ed25519Verify(key, SignedMessage(NodeProofSignature::NodeProof, transcript), signature);
}

std::optional<NodeSessionKeys> DeriveNodeSessionKeys(SecureByteBuffer const& ownSecret,
                                                     Wire::NodeChallengeRequest const& request,
                                                     Wire::NodeChallengeReply const& reply,
                                                     std::string_view clientId,
                                                     bool callerSide)
{
    auto peer = X25519PublicKey {};
    std::ranges::copy(callerSide ? std::span<std::byte const> { reply.ephemeral }
                                 : std::span<std::byte const> { request.ephemeral },
                      peer.begin());
    auto const shared = X25519SharedSecret(ownSecret, peer);
    if (!shared.has_value())
        return std::nullopt;

    std::array<std::byte, 2 * NonceBytes> salt {};
    std::ranges::copy(request.nonce, salt.begin());
    std::ranges::copy(reply.nonce, salt.begin() + NonceBytes);

    auto callerToServer = DirectionKey(*shared, salt, CallerToServerLabel, request, reply, clientId);
    auto serverToCaller = DirectionKey(*shared, salt, ServerToCallerLabel, request, reply, clientId);
    if (!callerToServer.has_value() || !serverToCaller.has_value())
        return std::nullopt;
    return NodeSessionKeys { .callerToServer = *std::move(callerToServer), .serverToCaller = *std::move(serverToCaller) };
}

} // namespace FastCache::Distributed

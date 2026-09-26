// SPDX-License-Identifier: Apache-2.0
#include "../fastcache-cc/CacheProtocol.hpp"
#include "NodeProofClient.hpp"

#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <format>
#include <utility>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Whether @p code says the peer serves no proof at all: a node running no consensus, which is
    /// no scheduler of this fleet, or a build that does not know the verbs.
    /// @param code The refusal's code.
    /// @return True when the peer offers no proof, rather than refusing this one.
    [[nodiscard]] bool SaysNoProofHere(Wire::ErrorCode code) noexcept
    {
        return code == Wire::ErrorCode::UnknownOpcode || code == Wire::ErrorCode::NoCluster;
    }

    /// The attempt a refused exchange amounts to.
    /// @param outcome What the exchange answered.
    /// @return `NotOffered` for a peer that serves no proof, `Refused` otherwise, with its words.
    [[nodiscard]] NodeProofAttempt Unproved(Cc::CacheOutcome const& outcome)
    {
        auto const offered = !(outcome.kind == Cc::CacheOutcomeKind::Rejected && SaysNoProofHere(outcome.code));
        return NodeProofAttempt { .result = offered ? NodeProofResult::Refused : NodeProofResult::NotOffered,
                                  .reason = Cc::DescribeOutcome(outcome) };
    }

    /// What this machine says about a server it will not prove itself to.
    /// @param standing Why.
    /// @param serverId Who the server said it is.
    /// @return The sentence.
    [[nodiscard]] std::string DescribeUntrusted(ServerStanding standing, std::string_view serverId)
    {
        if (standing == ServerStanding::Revoked)
            return std::format("{} signed with a key this cluster REVOKED: it is a machine the cluster forgot, and this "
                               "node proves nothing to it. Drop it from --scheduler",
                               serverId);
        return std::format("{} signed with a key that is no voter's in the roster this node holds, so this node proves "
                           "nothing to it: a --scheduler that names a machine which is not, or no longer, a voter",
                           serverId);
    }
} // namespace

NodeProofAttempt NodeProofClient::Prove(SealedFrameSocket& peer,
                                        Cc::CredentialNotice& notice,
                                        Cc::Credential const& credential) const
{
    auto const nonce = DrawNonce(_random);
    auto ephemeral = Distributed::DrawNodeEphemeral(_random);
    if (!nonce.has_value() || !ephemeral.has_value())
        return NodeProofAttempt {
            .result = NodeProofResult::Refused,
            .reason = std::format("this node cannot draw a handshake from its random source: {}",
                                  (nonce.has_value() ? ephemeral.error() : nonce.error()).ToString()),
        };

    auto request = Wire::NodeChallengeRequest {};
    std::ranges::copy(*nonce, request.nonce.begin());
    std::ranges::copy(ephemeral->publicKey, request.ephemeral.begin());

    auto const challenged =
        core::async::syncRun(Cc::ExchangeFramed(&peer, &notice, Wire::EncodeNodeChallenge(request), credential));
    if (!challenged.IsHit())
        return Unproved(challenged);

    // Exact widths are the decoder's rule: a field silently truncated would have both ends sign
    // different inputs, and the refusal would read as a forgery for a version mismatch.
    auto const reply = Wire::DecodeNodeChallengeReply(challenged.value);
    if (!reply.has_value())
        return NodeProofAttempt { .result = NodeProofResult::Refused,
                                  .reason = "the server answered a challenge this build cannot read" };

    // The server's signature under the key it named FIRST, and whether that key is one to trust
    // second -- so a server that cannot sign is told apart from one this node will not talk to.
    if (!Distributed::VerifyNodeChallengeReply(request, *reply))
        return NodeProofAttempt { .result = NodeProofResult::Untrusted,
                                  .reason = std::format("{}'s signature does not verify under the key it named",
                                                        reply->serverId) };

    auto serverKey = Ed25519PublicKey {};
    std::ranges::copy(reply->serverKey, serverKey.begin());
    if (auto const standing = _trust.StandingOf(reply->serverId, serverKey);
        standing == ServerStanding::NotVoter || standing == ServerStanding::Revoked)
        return NodeProofAttempt { .result = NodeProofResult::Untrusted,
                                  .reason = DescribeUntrusted(standing, reply->serverId) };

    auto keys = Distributed::DeriveNodeSessionKeys(ephemeral->secret, request, *reply, _nodeId, /*callerSide=*/true);
    if (!keys.has_value())
        return NodeProofAttempt { .result = NodeProofResult::Untrusted,
                                  .reason = std::format("{}'s ephemeral key agrees no session key", reply->serverId) };

    // The server answers the proof SEALED, whatever it decides, so the receiving seal is engaged
    // before the proof leaves; the sending one only once the answer has verified, since the proof
    // itself travels in the clear.
    peer.SealReceiving(std::move(keys->serverToCaller));
    auto const proved = core::async::syncRun(Cc::ExchangeFramed(
        &peer, &notice, Wire::EncodeProveNode(Distributed::MintNodeProof(_key, _nodeId, request, *reply)), credential));
    if (!proved.IsHit())
        return Unproved(proved);

    peer.SealSending(std::move(keys->callerToServer));
    return NodeProofAttempt { .result = NodeProofResult::Proved, .reason = {} };
}

} // namespace FastCache::Node

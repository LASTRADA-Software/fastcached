// SPDX-License-Identifier: Apache-2.0
#include "../fastcache-cc/CacheProtocol.hpp"
#include "NodeProofClient.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <span>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Whether a refusal means *this peer serves no proof* rather than *your key is wrong*.
    ///
    /// Two codes, and they are the two shapes a mixed fleet produces: a build that predates the
    /// verbs answers `UnknownOpcode` by the step-over rule, and a current build holding no
    /// `--cluster-key-file` answers `NoCluster` from its unserved-family row. Neither is an event
    /// and neither is this machine's fault, so a caller carries on by address.
    ///
    /// Read from the CODE and never from the message: both refusals have prose an operator may
    /// reword, and a client keying on wording stops recognising them the day either is edited.
    /// @param code The refusal the peer sent.
    /// @return True when the peer offers no proof at all.
    [[nodiscard]] bool SaysNoProofHere(Wire::ErrorCode code) noexcept
    {
        return code == Wire::ErrorCode::UnknownOpcode || code == Wire::ErrorCode::NoCluster;
    }

} // namespace

NodeProofAttempt ProveNodeOver(ISocket& peer,
                               Cc::CredentialNotice& notice,
                               IClusterKeySource const& key,
                               std::string_view nodeId,
                               Cc::Credential const& credential)
{
    // Read where it is PRESENTED, never captured: the credential rule, and here it also means a
    // rotated key file reaches a running node's next heartbeat round rather than its next
    // restart. A node that cannot read its own key presents nothing and goes on registering by
    // address, which is exactly what it did before this verb existed.
    auto const clusterKey = key.ClusterKey();
    if (!clusterKey.has_value())
        return NodeProofAttempt { .result = NodeProofResult::NothingToPresent, .reason = clusterKey.error() };

    auto const challenged = SyncRun(Cc::ExchangeFramed(&peer, &notice, Wire::EncodeNodeChallenge(), credential));
    if (!challenged.IsHit())
    {
        if (challenged.kind == Cc::CacheOutcomeKind::Rejected && SaysNoProofHere(challenged.code))
            return NodeProofAttempt { .result = NodeProofResult::NotOffered, .reason = Cc::DescribeOutcome(challenged) };
        return NodeProofAttempt { .result = NodeProofResult::Refused, .reason = Cc::DescribeOutcome(challenged) };
    }

    // The LENGTH is the whole validation and it is exact: a peer whose nonce is wider than this
    // build's would otherwise have its challenge silently truncated, both ends would sign
    // different inputs, and the refusal would come back as a wrong key -- a confident wrong
    // diagnosis for a version mismatch. `DecodeNodeChallengeReply` owns that rule.
    auto const challenge = Wire::DecodeNodeChallengeReply(challenged.value);
    if (!challenge.has_value())
        return NodeProofAttempt { .result = NodeProofResult::Refused,
                                  .reason = "the peer answered a challenge this build cannot read: it is not "
                                            "exactly 32 bytes, so the two ends would sign different inputs" };

    auto const tag = Distributed::MintNodeProof(std::span<std::byte const> { *clusterKey }, *challenge, nodeId);
    auto const proved = SyncRun(Cc::ExchangeFramed(&peer, &notice, Wire::EncodeProveNode(nodeId, tag), credential));
    if (!proved.IsHit())
    {
        if (proved.kind == Cc::CacheOutcomeKind::Rejected && SaysNoProofHere(proved.code))
            // Reachable although the challenge was served: a peer may be reconfigured, or
            // reloaded, between two frames of one connection. Reported as *not offered* rather
            // than as a refusal, because what an operator would act on is unchanged.
            return NodeProofAttempt { .result = NodeProofResult::NotOffered, .reason = Cc::DescribeOutcome(proved) };
        return NodeProofAttempt { .result = NodeProofResult::Refused, .reason = Cc::DescribeOutcome(proved) };
    }

    return NodeProofAttempt { .result = NodeProofResult::Proved, .reason = {} };
}

} // namespace FastCache::Node

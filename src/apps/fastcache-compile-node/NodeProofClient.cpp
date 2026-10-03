// SPDX-License-Identifier: Apache-2.0
#include "../fastcache-cc/CacheProtocol.hpp"
#include "../fastcache-cc/WorkerProtocol.hpp"
#include "NodeProofClient.hpp"

#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <utility>
#include <vector>

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
    /// @param standing What the trust said about the server, when it was asked before the exchange.
    /// @return `NotOffered` for a peer that serves no proof, `Refused` otherwise, with its words.
    [[nodiscard]] NodeProofAttempt Unproved(Cc::CacheOutcome const& outcome,
                                            std::optional<ServerStanding> standing = std::nullopt)
    {
        auto result = NodeProofResult::Refused;
        if (outcome.kind == Cc::CacheOutcomeKind::Rejected && SaysNoProofHere(outcome.code))
            result = NodeProofResult::NotOffered;
        else if (outcome.kind == Cc::CacheOutcomeKind::Rejected && outcome.code == Wire::ErrorCode::RosterNotYetApplied)
            result = NodeProofResult::Deferred;
        return NodeProofAttempt { .result = result, .reason = Cc::DescribeOutcome(outcome), .standing = standing };
    }

    /// Who is proving, borrowed from the client for one handshake.
    ///
    /// Pointers rather than references, as for every coroutine parameter here: this travels into
    /// `Challenge` by value, and a reference member would be the borrow the reference-parameter rule
    /// refuses, passed in by another route.
    struct Prover
    {
        std::string_view nodeId;   ///< The id it proves.
        Ed25519KeyPair const* key; ///< Its identity key pair; not owned.
        ISecureRandom* random;     ///< Where the handshake's nonce and ephemeral key come from; not owned.
    };

    /// What the challenge settled that the proof exchange needs, and nothing else.
    struct ProofStep
    {
        std::vector<std::byte> frame;      ///< The proof, framed.
        Distributed::NodeSessionKeys keys; ///< What seals the connection in each direction.
        ServerStanding standing;           ///< What the trust said about the server.
    };

    /// Open the handshake, read the server's signed challenge, and decide whether to answer it.
    ///
    /// Its own coroutine so that the opening -- the nonce, the ephemeral secret, the request and the
    /// server's reply -- ends here, and only the proof, the keys and the standing cross the proof
    /// exchange that follows.
    /// @param prover Who is proving; what it borrows must outlive the coroutine.
    /// @param peer The connection; not owned.
    /// @param trust Whom the prover may prove itself to; not owned.
    /// @return The step, or the attempt the handshake ended as.
    [[nodiscard]] core::async::Task<std::expected<ProofStep, NodeProofAttempt>> Challenge(Prover prover,
                                                                                          SealedFrameSocket* peer,
                                                                                          IServerTrust const* trust)
    {
        // `nonce` and `ephemeral`, and `request` built from them, are held across the challenge's
        // `co_await`, and the first two are call results: the shape MSVC 19.44 for ARM64 miscompiled
        // in #1545, keeping such a result on the resume function's stack. The rulebook's remedy is to
        // compute the value after the await, and it cannot apply here: the nonce and the ephemeral
        // key are what the challenge SENDS, and the reply is checked and the session key agreed
        // against them afterwards, so neither can be drawn later or drawn again. The pin is the
        // `[proof]` cases on the `windows-11-arm` `cl-release` leg, which runs `ctest`: a draw read
        // back from a later activation's stack fails the signature check or the seal there. That leg
        // is NON-BINDING -- a failure on it stops no merge -- so the pin is a signal someone must read,
        // not a gate.
        auto const nonce = DrawNonce(*prover.random);
        auto ephemeral = Distributed::DrawNodeEphemeral(*prover.random);
        if (!nonce.has_value() || !ephemeral.has_value())
            co_return std::unexpected { NodeProofAttempt {
                .result = NodeProofResult::Refused,
                .reason = std::format("this node cannot draw a handshake from its random source: {}",
                                      (nonce.has_value() ? ephemeral.error() : nonce.error()).ToString()),
                .standing = std::nullopt,
            } };

        auto request = Wire::NodeChallengeRequest {};
        std::ranges::copy(*nonce, request.nonce.begin());
        std::ranges::copy(ephemeral->publicKey, request.ephemeral.begin());

        auto const challenged = co_await Cc::ExchangeWithSchedulerAsync(peer, Wire::EncodeNodeChallenge(request));
        if (!challenged.IsHit())
            co_return std::unexpected { Unproved(challenged) };

        // Exact widths are the decoder's rule: a field silently truncated would have both ends sign
        // different inputs, and the refusal would read as a forgery for a version mismatch.
        auto const reply = Wire::DecodeNodeChallengeReply(challenged.value);
        if (!reply.has_value())
            co_return std::unexpected { NodeProofAttempt {
                .result = NodeProofResult::Refused,
                .reason = "the server answered a challenge this build cannot read",
                .standing = std::nullopt,
            } };

        // The server's signature under the key it named FIRST, and whether that key is one to trust
        // second -- so a server that cannot sign is told apart from one this node will not talk to.
        if (!Distributed::VerifyNodeChallengeReply(request, *reply))
            co_return std::unexpected { NodeProofAttempt {
                .result = NodeProofResult::Untrusted,
                .reason = std::format("{}'s signature does not verify under the key it named", reply->serverId),
                .standing = std::nullopt,
            } };

        auto serverKey = Ed25519PublicKey {};
        std::ranges::copy(reply->serverKey, serverKey.begin());
        auto const standing = trust->StandingOf(reply->serverId, serverKey);
        auto const expected = trust->Expected();
        if (auto const& row = ServerStandingTable[static_cast<std::size_t>(standing)]; !row.provesTo)
            co_return std::unexpected { NodeProofAttempt {
                .result = NodeProofResult::Untrusted,
                .reason = std::vformat(row.refusal, std::make_format_args(reply->serverId, expected)),
                .standing = standing,
            } };

        auto keys =
            Distributed::DeriveNodeSessionKeys(ephemeral->secret, request, *reply, prover.nodeId, /*callerSide=*/true);
        if (!keys.has_value())
            co_return std::unexpected { NodeProofAttempt {
                .result = NodeProofResult::Untrusted,
                .reason = std::format("{}'s ephemeral key agrees no session key", reply->serverId),
                .standing = standing,
            } };

        co_return ProofStep {
            .frame = Wire::EncodeProveNode(Distributed::MintNodeProof(*prover.key, prover.nodeId, request, *reply)),
            .keys = *std::move(keys),
            .standing = standing,
        };
    }
} // namespace

std::string ReplacedNodeKeyDiagnosis(std::string_view id)
{
    return std::format("this cluster records {} under another identity key: that machine's node-key was replaced while "
                       "the id it minted survived. Restore the node-key it was recorded with, or --cluster-forget={} on "
                       "a member and admit the new key (it asks to enroll and --enroll-approve admits it, or "
                       "--cluster-admit={}=<host>:<port>@<key> on a member, with what --print-identity prints)",
                       id,
                       id,
                       id);
}

NodeProofClient::NodeProofClient(std::string nodeId,
                                 Ed25519KeyPair const& key,
                                 IServerTrust const& trust,
                                 Distributed::IMembershipOracle const* ownCluster,
                                 NodeConditions* conditions,
                                 ISecureRandom& random):
    _nodeId { std::move(nodeId) },
    _key { key },
    _trust { trust },
    _ownCluster { ownCluster },
    _conditions { conditions },
    _random { random }
{
    // Evaluated from the start: a consensus node's row is this client's, and one left undecided is
    // a wiring defect `NodeConditions::Settle` names. Nothing has been held yet.
    if (_ownCluster != nullptr && _conditions != nullptr)
        _conditions->Clear(NodeCondition::OwnRecordAwaited);
}

OwnRecord NodeProofClient::OwnRecordNow() const
{
    if (_ownCluster == nullptr)
        return OwnRecord::NotAsked;
    // As the node-proof surface asks it (`NodeProofResponder`): the key, proven by a session proof.
    auto const standing = _ownCluster->ExplainKey(ProvenIdentity { .id = _nodeId, .key = _key.PublicKey() },
                                                  Distributed::KeyEvidence::SessionProof);
    if (Distributed::RestsOnProvenIdentity(standing)
        || standing.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone))
        return OwnRecord::Recorded;
    // No opinion of this key, and yet the id may be recorded -- under a key this machine no longer
    // holds. That is not a wait, and saying "not recorded yet" about it is a confident wrong signal.
    if (auto const recorded = _ownCluster->LiveKeyOf(_nodeId); recorded.has_value() && *recorded != _key.PublicKey())
        return OwnRecord::OtherKey;
    return OwnRecord::Awaited;
}

bool NodeProofClient::HoldUntilRecorded(ILogger& logger) const
{
    auto const record = OwnRecordNow();
    std::scoped_lock const lock { _holdMutex };
    if (record == OwnRecord::NotAsked || record == OwnRecord::Recorded)
    {
        if (_heldAsks > 0)
        {
            logger.Log(LogLevel::Info, "this node's own cluster has recorded it; announcing to the fleet");
            if (_conditions != nullptr)
                _conditions->Clear(NodeCondition::OwnRecordAwaited);
        }
        _heldAsks = 0;
        _heldFor = record;
        return false;
    }

    // A hold for a different reason is a new hold, said afresh; leaving a replaced key behind ends
    // what the raised row said about it.
    if (record != _heldFor)
    {
        if (_heldFor == OwnRecord::OtherKey && _conditions != nullptr)
            _conditions->Clear(NodeCondition::OwnRecordAwaited);
        _heldAsks = 0;
        _heldFor = record;
    }
    ++_heldAsks;
    NarrateHold(record, logger);
    return true;
}

void NodeProofClient::NarrateHold(OwnRecord record, ILogger& logger) const
{
    if (record == OwnRecord::OtherKey)
    {
        if (_heldAsks != 1)
            return;
        auto const diagnosis = ReplacedNodeKeyDiagnosis(_nodeId);
        logger.Logf(LogLevel::Warn, "not announcing to the fleet: {}", diagnosis);
        if (_conditions != nullptr)
            _conditions->Raise(NodeCondition::OwnRecordAwaited, diagnosis);
        return;
    }

    if (_heldAsks == 1)
        logger.Logf(LogLevel::Info,
                    "not announcing to the fleet yet: this node's own cluster has not recorded this node yet, so every "
                    "scheduler of that cluster would refuse {} as unknown. A member the cluster was started with is "
                    "recorded once the cluster has elected a leader, a moment after start; a machine joining a "
                    "cluster that already runs is recorded once it is admitted. Announcing as soon as it is recorded",
                    _nodeId);
    else if (_heldAsks == OwnRecordPatience)
    {
        logger.Logf(LogLevel::Warn,
                    "still not announcing to the fleet: after {} asks this node's own cluster has not recorded {}. "
                    "Either that cluster cannot elect a leader -- a majority of its voters must answer -- or this "
                    "machine joined a cluster that never admitted it: it asks to enroll and --enroll-approve admits "
                    "it, or "
                    "--cluster-admit=<id>=<host>:<port>@<key> on a member, with the id and key this machine's "
                    "--print-identity prints",
                    OwnRecordPatience,
                    _nodeId);
        if (_conditions != nullptr)
            _conditions->Raise(
                NodeCondition::OwnRecordAwaited,
                std::format("after {} asks this node's own cluster has not recorded {}", OwnRecordPatience, _nodeId));
    }
}

core::async::Task<NodeProofAttempt> NodeProofClient::ProveAsync(SealedFrameSocket* peer, IServerTrust const* trust) const
{
    // Both exchanges present NO credential, by the seam's signature rather than by an argument left
    // out: the proof IS this machine's credential (`Cc::ExchangeWithSchedulerAsync`).
    auto step = co_await Challenge(Prover { .nodeId = _nodeId, .key = &_key, .random = &_random }, peer, trust);
    if (!step.has_value())
        co_return Noted(std::move(step).error());

    // The server answers the proof SEALED, whatever it decides, so the receiving seal is engaged
    // before the proof leaves; the sending one only once the answer has verified, since the proof
    // itself travels in the clear.
    peer->SealReceiving(std::move(step->keys.serverToCaller));
    auto const proved = co_await Cc::ExchangeWithSchedulerAsync(peer, std::move(step->frame));
    if (!proved.IsHit())
        co_return Noted(Unproved(proved, step->standing));

    peer->SealSending(std::move(step->keys.callerToServer));
    co_return Noted(NodeProofAttempt { .result = NodeProofResult::Proved, .reason = {}, .standing = step->standing });
}

NodeProofAttempt NodeProofClient::Noted(NodeProofAttempt attempt) const noexcept
{
    if (attempt.result == NodeProofResult::Deferred)
        _deferrals.fetch_add(1, std::memory_order_relaxed);
    else
        _deferrals.store(0, std::memory_order_relaxed);
    return attempt;
}

NodeProofAttempt NodeProofClient::Prove(SealedFrameSocket& peer) const
{
    // The presence round runs on a thread that may block, over a blocking socket: `syncRun` is sound
    // there and nowhere else. The shared-cache leg runs on the reactor and awaits `ProveAsync`.
    return core::async::syncRun(ProveAsync(&peer, &_trust));
}

} // namespace FastCache::Node

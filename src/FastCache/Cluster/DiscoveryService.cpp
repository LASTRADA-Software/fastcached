// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Cluster
{

DiscoveryService::DiscoveryService(IDatagramSocket& socket,
                                   IClock& clock,
                                   ISecureRandom& random,
                                   PeerDirectory& directory,
                                   DiscoveryConfig config,
                                   Consensus::IRaftPeerKeys const& keys,
                                   IMetricsSink& metrics,
                                   ILogger& logger):
    _socket { socket },
    _clock { clock },
    _random { random },
    _directory { directory },
    _config { std::move(config) },
    _keys { keys },
    _metrics { metrics },
    _logger { logger }
{
}

bool DiscoveryService::SendBeacon()
{
    auto const datagram = DiscoveryWire::EncodeBeacon(
        { .clusterId = _config.clusterId, .nodeId = _config.nodeId, .raftEndpoint = _config.raftEndpoint });
    return _socket.send(datagram, _config.beaconAddress).has_value();
}

bool DiscoveryService::IssueChallenge(DiscoveryWire::Beacon const& peer, DatagramAddress const& replyTo)
{
    // Drawn through the randomness seam rather than a local engine: a nonce this
    // node chose is the only thing making a proof unreplayable, so a test has to be
    // able to fix it and a production build has to be able to trust it. `DrawNonce`
    // rather than a draw here, because the Raft peer handshake draws its nonces the
    // same way and the size and the source are one decision (#1308).
    auto const nonce = DrawNonce(_random);
    if (!nonce.has_value())
    {
        // Withheld rather than issued with a weak nonce (#1527), and said. The line names
        // no peer: nothing about the peer is wrong, and what it claimed is unproved.
        if (auto const now = _clock.now(); now >= _nextNoNonceReport)
        {
            _nextNoNonceReport = now + NoNonceReportInterval;
            _logger.Logf(LogLevel::Error,
                         "discovery: withheld a challenge, because this node cannot draw a nonce: {}. No peer can "
                         "prove the key to this node until it can",
                         nonce.error().ToString());
        }
        return false;
    }

    DiscoveryWire::Challenge const challenge { .clusterId = _config.clusterId, .nonce = *nonce };

    // Replaces any earlier challenge to this node rather than adding to a list:
    // a beacon is unauthenticated, so anything on the segment can send one, and a
    // table that grew per datagram would be a memory-exhaustion hole reachable
    // without holding the key.
    _pending[peer.nodeId] = Pending { .challenge = challenge, .endpoint = peer.raftEndpoint, .issuedAt = _clock.now() };

    // Unicast to where the datagram actually came from, not to what it claimed.
    // A beacon that lies about its endpoint should not be able to aim this
    // node's challenges at a third party.
    (void) _socket.send(DiscoveryWire::EncodeChallenge(challenge), replyTo);
    return true;
}

DiscoveryEvent DiscoveryService::PumpOnce(std::chrono::milliseconds timeout)
{
    auto const received = _socket.receive(timeout);
    if (!received.has_value())
        return received.error() == DatagramWait::Closed ? DiscoveryEvent::Closed : DiscoveryEvent::Nothing;

    auto const kind = DiscoveryWire::ClassifyDatagram(received->payload);
    if (!kind.has_value())
        return DiscoveryEvent::Ignored;

    switch (*kind)
    {
        case DiscoveryWire::Kind::Beacon: {
            auto const beacon = DiscoveryWire::DecodeBeacon(received->payload);
            if (!beacon.has_value())
                return DiscoveryEvent::Ignored;

            // The directory decides whether this beacon is even ours -- wrong
            // cluster, our own, or an identity nothing could record. Only then is a
            // challenge worth the datagram, and a peer it declined is one no
            // membership proposal can ever be generated for.
            // A `switch` without a `default`, so a fifth outcome is a build failure
            // rather than one nobody reports: "deliberately silent" and "somebody
            // forgot" are otherwise the same state, and three of the four here are
            // deliberately silent.
            switch (_directory.NoteBeacon(beacon->clusterId, beacon->nodeId, beacon->raftEndpoint))
            {
                case BeaconOutcome::Recorded:
                    break;

                // Both ordinary. This node's own beacon comes back on every
                // broadcast, and another fleet's is what a shared segment carries,
                // so reporting either would bury the one that is a fault.
                case BeaconOutcome::OtherCluster:
                case BeaconOutcome::Self:
                    return DiscoveryEvent::Ignored;

                case BeaconOutcome::Unnameable:
                    // Throttled, because one unauthenticated datagram provokes this
                    // and nothing on the segment has to hold the key to send it --
                    // see `UnnameableReportInterval`.
                    //
                    // Reported by the address it came FROM rather than by what it
                    // claimed, for two reasons that happen to agree. The claim is
                    // the thing that is not text, so it is the one part of such a
                    // beacon that cannot be printed at all -- and the address is
                    // what says which machine to go and look at, which is where the
                    // identity was typed.
                    if (auto const now = _clock.now(); now >= _nextUnnameableReport)
                    {
                        _nextUnnameableReport = now + UnnameableReportInterval;
                        _logger.Logf(LogLevel::Warn,
                                     "discovery: the beacon from {} names an id or endpoint this node cannot record "
                                     "as a member -- empty, or not valid UTF-8; ignoring",
                                     FormatHostPort(received->from.host, received->from.port));
                    }
                    return DiscoveryEvent::Ignored;
            }

            return IssueChallenge(*beacon, received->from) ? DiscoveryEvent::PeerSeen : DiscoveryEvent::ChallengeWithheld;
        }

        case DiscoveryWire::Kind::Challenge: {
            auto const challenge = DiscoveryWire::DecodeChallenge(received->payload);
            if (!challenge.has_value())
                return DiscoveryEvent::Ignored;

            // Answering a challenge for another cluster would announce this node, signed, to a
            // fleet that is not ours.
            if (challenge->clusterId != _config.clusterId)
                return DiscoveryEvent::Ignored;

            // Signed with this node's OWN key (#178), over a nonce somebody else chose -- so the
            // signature is fresh, names this node's endpoint, and proves nothing to anybody but
            // the node that asked.
            auto proof = DiscoveryWire::Proof { .nodeId = _config.nodeId,
                                                .raftEndpoint = _config.raftEndpoint,
                                                .publicKey = _keys.OwnPublicKey(),
                                                .signature = {} };
            proof.signature =
                _keys.SignAsSelf(DiscoveryWire::ProofMessage(*challenge, proof.nodeId, proof.raftEndpoint, proof.publicKey));
            (void) _socket.send(DiscoveryWire::EncodeProof(proof), received->from);
            return DiscoveryEvent::ChallengeAnswered;
        }

        case DiscoveryWire::Kind::Proof: {
            auto const proof = DiscoveryWire::DecodeProof(received->payload);
            if (!proof.has_value())
                return DiscoveryEvent::Ignored;

            // Refused before it is COMPARED, and the ordering is the whole of it:
            // the mismatch below names what the proof claimed, and a proof is
            // unauthenticated until its signature checks out -- so anything on the
            // segment could otherwise put arbitrary bytes into this node's log
            // without ever holding a key.
            //
            // It could never have matched anyway. The endpoint it would have to
            // equal is one `PeerDirectory::NoteBeacon` recorded, and that refuses
            // an endpoint which is not text. The node id needs no such check: it
            // has to be a key of `_pending`, and those come from the same door.
            if (!IsValidUtf8(proof->raftEndpoint))
                return DiscoveryEvent::ProofRejected;

            // A proof is only ever an answer to a challenge THIS node issued. An
            // unsolicited one carries a nonce nobody here chose, so there is
            // nothing it could be replayed against -- and accepting one would
            // make the nonce pointless.
            auto pending = _pending.find(proof->nodeId);
            if (pending == _pending.end())
                return DiscoveryEvent::ProofRejected;

            // The endpoint must be the one that was challenged. Both are inside
            // the signature, so a mismatch cannot verify anyway -- this rejects it
            // before doing the work, and says so.
            if (pending->second.endpoint != proof->raftEndpoint)
            {
                _logger.Logf(LogLevel::Warn,
                             "discovery: {} answered for {} but was challenged at {}; ignoring",
                             proof->nodeId,
                             proof->raftEndpoint,
                             pending->second.endpoint);
                return DiscoveryEvent::ProofRejected;
            }

            // Spent, whatever the verdict: a nonce that could answer twice is a nonce that can
            // be replayed, and a proof that failed must not get a second try at it.
            auto const challenge = pending->second.challenge;
            _pending.erase(pending);
            return JudgeProof(*proof, challenge, received->from);
        }

        case DiscoveryWire::Kind::Invalid:
            break;
    }

    return DiscoveryEvent::Ignored;
}

DiscoveryEvent DiscoveryService::JudgeProof(DiscoveryWire::Proof const& proof,
                                            DiscoveryWire::Challenge const& challenge,
                                            DatagramAddress const& from)
{
    // The SIGNATURE first, under the key the proof carries, before anything the proof claims
    // is looked up or reported: until it verifies, the key is a claim too, and naming it would
    // be naming bytes anybody could have sent.
    if (!DiscoveryWire::VerifyProofSignature(challenge, proof))
    {
        _metrics.Increment(IMetricsSink::Counter::DiscoveryProofsRefusedForged);
        _logger.Logf(LogLevel::Warn,
                     "discovery: the proof from {} is not signed by the key it carries; ignoring it",
                     FormatHostPort(from.host, from.port));
        return DiscoveryEvent::ProofRejected;
    }

    // Then the roster: whose key is this? Only the key the roster holds for this id proves the
    // id. A revoked key is recognised whatever id it claims, because the list is not narrowed to
    // the claim -- the question is who SIGNED, which only the signature answers (`PeerKeys`).
    auto const keys = _keys.KeysOf(proof.nodeId);
    if (keys.live != std::optional { proof.publicKey })
    {
        // Two increments rather than one over a conditional, so each counter is written by a
        // statement that names it -- which is what `counter-attribution` reads to find a writer.
        auto const revoked = std::ranges::contains(keys.revoked, proof.publicKey);
        if (revoked)
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofsRefusedRevokedKey);
        else
            _metrics.Increment(IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey);
        ReportUnacceptedKey(revoked, proof, from);
        return revoked ? DiscoveryEvent::PeerRevokedKey : DiscoveryEvent::PeerUnknownKey;
    }

    if (!_directory.MarkAuthenticated(proof.nodeId, proof.raftEndpoint, proof.publicKey))
        return DiscoveryEvent::ProofRejected;

    _logger.Logf(
        LogLevel::Info, "discovery: {} at {} proved the key the cluster holds for it", proof.nodeId, proof.raftEndpoint);
    return DiscoveryEvent::PeerAuthenticated;
}

void DiscoveryService::ReportUnacceptedKey(bool revoked, DiscoveryWire::Proof const& proof, DatagramAddress const& from)
{
    ++_unacceptedSinceReport;
    auto const now = _clock.now();
    if (now < _nextUnacceptedKeyReport)
        return;
    _nextUnacceptedKeyReport = now + UnacceptedKeyReportInterval;

    // The key WHOLE, because it verified: only its holder could have signed this, so naming it
    // is no oracle, and it is exactly what an operator types after `@` to admit the machine --
    // or recognises as the one they removed. The two remedies are opposite, so each is said.
    auto const key = FormatEd25519PublicKey(proof.publicKey);
    auto const remedy = revoked ? std::string { "that machine was removed, and this key is never admitted again" }
                                : std::format("enroll it, or admit it with --cluster-admit={}={}@{}, if it belongs",
                                              proof.nodeId,
                                              proof.raftEndpoint,
                                              key);
    _logger.Logf(LogLevel::Warn,
                 "discovery: {} at {} (from {}) proved the key {}, which the cluster {}. Not desiring it: {}.{}",
                 proof.nodeId,
                 proof.raftEndpoint,
                 FormatHostPort(from.host, from.port),
                 key,
                 revoked ? "has REVOKED" : "does not hold for that id",
                 remedy,
                 _unacceptedSinceReport > 1
                     ? std::format(" ({} such proof(s) since the last report)", _unacceptedSinceReport)
                     : std::string {});
    _unacceptedSinceReport = 0;
}

void DiscoveryService::Maintain()
{
    _directory.ExpireStale();

    auto const now = _clock.now();
    std::erase_if(_pending,
                  [this, now](auto const& entry) { return now - entry.second.issuedAt >= _config.challengeLifetime; });
}

} // namespace FastCache::Cluster

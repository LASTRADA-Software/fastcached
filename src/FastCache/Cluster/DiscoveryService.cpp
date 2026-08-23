// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace FastCache::Cluster
{

DiscoveryService::DiscoveryService(IDatagramSocket& socket,
                                   IClock& clock,
                                   IRandomSource& random,
                                   PeerDirectory& directory,
                                   DiscoveryConfig config,
                                   ILogger& logger):
    _socket { socket },
    _clock { clock },
    _random { random },
    _directory { directory },
    _config { std::move(config) },
    _logger { logger }
{
}

bool DiscoveryService::SendBeacon()
{
    auto const datagram = DiscoveryWire::EncodeBeacon(
        { .clusterId = _config.clusterId, .nodeId = _config.nodeId, .raftEndpoint = _config.raftEndpoint });
    return _socket.Send(datagram, _config.beaconAddress).has_value();
}

void DiscoveryService::IssueChallenge(DiscoveryWire::Beacon const& peer, std::string_view replyTo)
{
    DiscoveryWire::Challenge challenge { .clusterId = _config.clusterId, .nonce = {} };

    // Drawn through the randomness seam rather than a local engine, for the
    // reason `RaftNode`'s election timeouts are: a nonce this node chose is the
    // only thing making a proof unreplayable, so a test has to be able to fix it
    // and a production build has to be able to trust it.
    for (auto offset = std::size_t { 0 }; offset < challenge.nonce.size(); offset += sizeof(std::uint64_t))
    {
        auto const draw = _random.UniformInRange(0, std::numeric_limits<std::uint64_t>::max());
        WriteBigEndian<std::uint64_t>(std::span { challenge.nonce }.subspan(offset, sizeof(std::uint64_t)), draw);
    }

    // Replaces any earlier challenge to this node rather than adding to a list:
    // a beacon is unauthenticated, so anything on the segment can send one, and a
    // table that grew per datagram would be a memory-exhaustion hole reachable
    // without holding the key.
    _pending[peer.nodeId] = Pending { .challenge = challenge, .endpoint = peer.raftEndpoint, .issuedAt = _clock.Now() };

    // Unicast to where the datagram actually came from, not to what it claimed.
    // A beacon that lies about its endpoint should not be able to aim this
    // node's challenges at a third party.
    (void) _socket.Send(DiscoveryWire::EncodeChallenge(challenge), replyTo);
}

DiscoveryEvent DiscoveryService::PumpOnce(std::chrono::milliseconds timeout)
{
    auto const received = _socket.Receive(timeout);
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
            // cluster, our own, or nothing to reach. Only then is a challenge
            // worth the datagram.
            if (!_directory.NoteBeacon(beacon->clusterId, beacon->nodeId, beacon->raftEndpoint))
                return DiscoveryEvent::Ignored;

            IssueChallenge(*beacon, received->from);
            return DiscoveryEvent::PeerSeen;
        }

        case DiscoveryWire::Kind::Challenge: {
            auto const challenge = DiscoveryWire::DecodeChallenge(received->payload);
            if (!challenge.has_value())
                return DiscoveryEvent::Ignored;

            // Answering a challenge for another cluster would prove possession
            // of this key to a fleet that is not ours.
            if (challenge->clusterId != _config.clusterId)
                return DiscoveryEvent::Ignored;

            auto const tag =
                DiscoveryWire::ExpectedProofTag(_config.presharedKey, *challenge, _config.nodeId, _config.raftEndpoint);
            (void) _socket.Send(
                DiscoveryWire::EncodeProof({ .nodeId = _config.nodeId, .raftEndpoint = _config.raftEndpoint, .tag = tag }),
                received->from);
            return DiscoveryEvent::ChallengeAnswered;
        }

        case DiscoveryWire::Kind::Proof: {
            auto const proof = DiscoveryWire::DecodeProof(received->payload);
            if (!proof.has_value())
                return DiscoveryEvent::Ignored;

            // A proof is only ever an answer to a challenge THIS node issued. An
            // unsolicited one carries a nonce nobody here chose, so there is
            // nothing it could be replayed against -- and accepting one would
            // make the nonce pointless.
            auto pending = _pending.find(proof->nodeId);
            if (pending == _pending.end())
                return DiscoveryEvent::ProofRejected;

            // The endpoint must be the one that was challenged. Both are inside
            // the MAC, so a mismatch cannot produce a valid tag anyway -- this
            // rejects it before doing the work, and says so.
            if (pending->second.endpoint != proof->raftEndpoint)
            {
                _logger.Logf(LogLevel::Warn,
                             "discovery: {} answered for {} but was challenged at {}; ignoring",
                             proof->nodeId,
                             proof->raftEndpoint,
                             pending->second.endpoint);
                return DiscoveryEvent::ProofRejected;
            }

            auto const expected = DiscoveryWire::ExpectedProofTag(
                _config.presharedKey, pending->second.challenge, proof->nodeId, proof->raftEndpoint);
            if (!ConstantTimeEquals(expected, proof->tag))
            {
                // Warn rather than debug: on a healthy segment this does not
                // happen, and when it does it means either a misconfigured key or
                // somebody trying to join a fleet they do not belong to. Both are
                // things an operator wants to see.
                _logger.Logf(LogLevel::Warn,
                             "discovery: {} at {} failed to prove the cluster key",
                             proof->nodeId,
                             proof->raftEndpoint);
                _pending.erase(pending);
                return DiscoveryEvent::ProofRejected;
            }

            // Spent, whatever happens next: a nonce that could answer twice is a
            // nonce that can be replayed.
            _pending.erase(pending);

            if (!_directory.MarkAuthenticated(proof->nodeId, proof->raftEndpoint))
                return DiscoveryEvent::ProofRejected;

            _logger.Logf(LogLevel::Info,
                         "discovery: {} at {} proved the cluster key and may be admitted",
                         proof->nodeId,
                         proof->raftEndpoint);
            return DiscoveryEvent::PeerAuthenticated;
        }

        case DiscoveryWire::Kind::Invalid:
            break;
    }

    return DiscoveryEvent::Ignored;
}

void DiscoveryService::Maintain()
{
    _directory.ExpireStale();

    auto const now = _clock.Now();
    std::erase_if(_pending,
                  [this, now](auto const& entry) { return now - entry.second.issuedAt >= _config.challengeLifetime; });
}

} // namespace FastCache::Cluster

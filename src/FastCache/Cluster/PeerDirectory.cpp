// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/PeerDirectory.hpp>

#include <algorithm>
#include <utility>

namespace FastCache::Cluster
{

PeerDirectory::PeerDirectory(IClock& clock, std::string clusterId, std::string selfNodeId, std::chrono::seconds expiry):
    _clock { clock },
    _clusterId { std::move(clusterId) },
    _selfNodeId { std::move(selfNodeId) },
    _expiry { expiry }
{
}

bool PeerDirectory::NoteBeacon(std::string_view clusterId, std::string_view nodeId, std::string_view raftEndpoint)
{
    // Two unrelated fleets can share a segment, and neither should disturb the
    // other. This is a routing decision and not a security one -- the cluster id
    // is plain text in every beacon -- so it is a cheap equality rather than
    // anything that pretends to authenticate.
    if (clusterId != _clusterId)
        return false;

    // Own beacons come back on a broadcast or multicast address -- the sender is
    // on the segment too -- so this is the ordinary case rather than a fault.
    // Recording it would make a lone node believe it has a peer, and propose a
    // membership change to admit itself.
    if (nodeId == _selfNodeId)
        return false;

    // A node that names neither an id nor an endpoint cannot be reached or
    // named in a membership entry, so there is nothing to remember.
    if (nodeId.empty() || raftEndpoint.empty())
        return false;

    auto const now = _clock.Now();
    auto const key = std::string { nodeId };

    if (auto found = _peers.find(key); found != _peers.end())
    {
        // A peer that now advertises a different endpoint loses its
        // authenticated bit: the proof covered the OLD endpoint, so carrying the
        // bit across would admit an address nobody ever proved. Re-proving is
        // one handshake, and the alternative is the hole the MAC's endpoint
        // field exists to close.
        if (found->second.raftEndpoint != raftEndpoint)
        {
            found->second.raftEndpoint = std::string { raftEndpoint };
            found->second.authenticated = false;
        }
        found->second.lastSeen = now;
        return true;
    }

    _peers.emplace(
        key,
        KnownPeer { .nodeId = key, .raftEndpoint = std::string { raftEndpoint }, .authenticated = false, .lastSeen = now });
    return true;
}

bool PeerDirectory::MarkAuthenticated(std::string_view nodeId, std::string_view raftEndpoint)
{
    auto found = _peers.find(std::string { nodeId });
    if (found == _peers.end())
        return false;

    // The endpoint must be the one currently advertised. A proof authenticates a
    // (node, endpoint) pair -- both are inside the MAC -- so accepting it against
    // whatever the directory happens to hold now would let a beacon sent between
    // the challenge and the proof redirect an authenticated peer.
    if (found->second.raftEndpoint != raftEndpoint)
        return false;

    found->second.authenticated = true;
    return true;
}

std::size_t PeerDirectory::ExpireStale()
{
    auto const now = _clock.Now();
    return std::erase_if(_peers, [this, now](auto const& entry) { return now - entry.second.lastSeen >= _expiry; });
}

std::vector<KnownPeer> PeerDirectory::Peers() const
{
    std::vector<KnownPeer> out;
    out.reserve(_peers.size());
    for (auto const& [id, peer]: _peers)
        out.push_back(peer);

    // Ordered because the container is not: an unordered_map's iteration order
    // varies between runs and standard libraries, and a caller that proposed a
    // membership change from it would produce a different proposal on each node.
    std::ranges::sort(out, {}, &KnownPeer::nodeId);
    return out;
}

std::vector<KnownPeer> PeerDirectory::AuthenticatedPeers() const
{
    auto out = Peers();
    std::erase_if(out, [](KnownPeer const& peer) { return !peer.authenticated; });
    return out;
}

} // namespace FastCache::Cluster

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// The rule `BeaconOutcome::Unnameable` names, which carries the reasoning.
    ///
    /// One question rather than two, because a member is an `(id, endpoint)` pair: a
    /// record with no id is not one, and neither is a record carrying a half that is not
    /// text. An empty ENDPOINT is a learner's, which answers nothing and so names nowhere to
    /// dial -- a member all the same.
    /// @param nodeId The identity the beacon claims.
    /// @param raftEndpoint The consensus endpoint it claims.
    /// @return True when both halves could survive being recorded.
    [[nodiscard]] bool CanBeNamed(std::string_view nodeId, std::string_view raftEndpoint) noexcept
    {
        return !nodeId.empty() && IsValidUtf8(nodeId) && IsValidUtf8(raftEndpoint);
    }
} // namespace

PeerDirectory::PeerDirectory(core::platform::IClock& clock,
                             IFleetSummarySource const& self,
                             Consensus::IRaftPeerKeys const& keys,
                             std::chrono::seconds expiry):
    _clock { clock },
    _self { self },
    _keys { keys },
    _expiry { expiry }
{
}

BeaconOutcome PeerDirectory::NoteBeacon(CompileCacheWire::FleetSummary const& summary)
{
    auto const& clusterId = summary.clusterId;
    auto const& nodeId = summary.nodeId;
    auto const& raftEndpoint = summary.raftEndpoint;

    // Read once per beacon, so the cluster and the id this beacon is judged against are one
    // reading of one source.
    auto const self = _self.Current();
    auto const ours = clusterId == self.clusterId;

    // Own beacons come back on a broadcast or multicast address -- the sender is
    // on the segment too -- so this is the ordinary case rather than a fault.
    // Recording it would make a lone node believe it has a peer, and propose a
    // membership change to admit itself. Asked of THIS cluster's beacons only: an id is
    // operator-typed, so another fleet may well hold a node named as this one is, and that
    // node is another fleet's -- challenged and judged as one, never dropped as this node.
    if (ours && nodeId == self.nodeId)
        return BeaconOutcome::Self;

    // Before anything is remembered, and that ordering is the whole of #159: what
    // this directory holds is what `DiscoveryTier` publishes, what `ConsensusTier`
    // desires, and what a leader eventually proposes as a member -- and a proposal
    // is refused at the proposer only by a check that also governs REMOVAL, which
    // is how a member nobody can name becomes one nobody can forget. Asked of
    // another fleet's node too: its proof is logged and handed to formation, and
    // both read these fields back out as text.
    if (!CanBeNamed(nodeId, raftEndpoint))
        return BeaconOutcome::Unnameable;

    auto const now = _clock.now();

    // Two fleets can share a segment, and neither's node is ever the other's peer.
    // This is a routing decision and not a security one -- the cluster id is plain
    // text in every beacon -- so it is a cheap equality rather than anything that
    // pretends to authenticate. Another fleet is still worth HEARING: a solitary
    // node yields to one, and an established one says it can see one. So it is kept
    // apart, bounded, and challenged; what its proof says goes to formation.
    //
    // A comparison, which is why the "can this be named" rule above does not reach
    // it: a fleet whose own id is not UTF-8 goes on working, and a filter here would
    // take every peer away from it without saying so -- both sides would simply
    // agree to ignore each other.
    if (!ours)
    {
        if (auto found = _foreign.find(clusterId); found != _foreign.end())
        {
            found->second.lastSeen = now;
            return BeaconOutcome::Foreign;
        }
        auto const inserted = InsertForeign(clusterId, /*proven=*/false);
        if (!inserted.has_value())
            return BeaconOutcome::ForeignTableFull;
        return *inserted ? BeaconOutcome::ForeignDisplacing : BeaconOutcome::Foreign;
    }

    auto const key = std::string { nodeId };

    // Asked at every beacon rather than once: the roster changes under a running node, and a peer
    // an operator admitted a minute ago is bounded by the roster from its next beacon on.
    auto const rostered = _keys.KeysOf(key).live.has_value();

    if (auto found = _peers.find(key); found != _peers.end())
    {
        // A peer that now advertises a different endpoint loses its
        // authenticated bit: the proof covered the OLD endpoint, so carrying the
        // bit across would admit an address nobody ever proved. Re-proving is
        // one handshake, and the alternative is the hole the signed endpoint
        // field exists to close.
        if (found->second.raftEndpoint != raftEndpoint)
        {
            found->second.raftEndpoint = std::string { raftEndpoint };
            found->second.authenticated = false;
            found->second.provenKey.reset();
        }
        found->second.lastSeen = now;
        found->second.rostered = rostered;
        return BeaconOutcome::Recorded;
    }

    // At the bound, the least recently heard peer the roster holds no key for makes room: it is
    // the one most likely to be gone, and nothing but its own beacons vouches for it. A rostered
    // newcomer is bounded by the roster and takes no slot from anybody.
    auto displaced = false;
    if (!rostered && UnrosteredPeers() >= MaxUnrosteredPeers)
    {
        auto unrostered = _peers | std::views::filter([](auto const& entry) { return !entry.second.rostered; });
        auto const oldest = std::ranges::min_element(
            unrostered, {}, [](auto const& entry) { return std::tie(entry.second.lastSeen, entry.first); });
        _peers.erase(oldest.base());
        displaced = true;
    }

    _peers.emplace(key,
                   KnownPeer { .nodeId = key,
                               .raftEndpoint = std::string { raftEndpoint },
                               .authenticated = false,
                               .provenKey = std::nullopt,
                               .lastSeen = now,
                               .rostered = rostered });
    return displaced ? BeaconOutcome::RecordedDisplacing : BeaconOutcome::Recorded;
}

std::optional<bool> PeerDirectory::InsertForeign(std::string const& clusterId, bool proven)
{
    auto const now = _clock.now();
    auto displaced = false;
    if (_foreign.size() >= MaxForeignFleets)
    {
        // The OLDEST slot nothing has proven -- by when it was taken, not when it was last heard, so
        // a spoofer re-sending its ids does not keep them young. A tie goes to the lower id, so
        // which slot goes does not depend on how a standard library orders a hash table.
        auto unproven = _foreign | std::views::filter([](auto const& entry) { return !entry.second.proven; });
        auto const oldest = std::ranges::min_element(
            unproven, {}, [](auto const& entry) { return std::tie(entry.second.recordedAt, entry.first); });
        if (oldest == unproven.end())
            return std::nullopt;
        _foreign.erase(oldest.base());
        displaced = true;
    }
    _foreign.emplace(clusterId, ForeignFleet { .recordedAt = now, .lastSeen = now, .proven = proven });
    return displaced;
}

bool PeerDirectory::MarkForeignProven(std::string_view clusterId)
{
    auto const key = std::string { clusterId };
    if (auto found = _foreign.find(key); found != _foreign.end())
    {
        found->second.proven = true;
        return true;
    }
    return InsertForeign(key, /*proven=*/true).has_value();
}

std::size_t PeerDirectory::UnrosteredPeers() const noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(_peers, [](auto const& entry) { return !entry.second.rostered; }));
}

bool PeerDirectory::MarkAuthenticated(std::string_view nodeId, std::string_view raftEndpoint, Ed25519PublicKey const& key)
{
    auto found = _peers.find(std::string { nodeId });
    if (found == _peers.end())
        return false;

    // The endpoint must be the one currently advertised. A proof authenticates a
    // (node, endpoint) pair -- both are inside the signature -- so accepting it against
    // whatever the directory happens to hold now would let a beacon sent between
    // the challenge and the proof redirect an authenticated peer.
    if (found->second.raftEndpoint != raftEndpoint)
        return false;

    found->second.authenticated = true;
    found->second.provenKey = key;
    return true;
}

std::size_t PeerDirectory::ExpireStale()
{
    auto const now = _clock.now();
    auto const peers =
        std::erase_if(_peers, [this, now](auto const& entry) { return now - entry.second.lastSeen >= _expiry; });
    auto const fleets =
        std::erase_if(_foreign, [this, now](auto const& entry) { return now - entry.second.lastSeen >= _expiry; });
    return peers + fleets;
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

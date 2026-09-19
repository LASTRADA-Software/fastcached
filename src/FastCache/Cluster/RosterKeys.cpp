// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/RosterKeys.hpp>

#include <algorithm>
#include <mutex>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// The keys a command line typed, by id.
    /// @param bootstrap The members it named.
    /// @return Each member that carried a key, by id.
    [[nodiscard]] std::map<Consensus::NodeId, Ed25519PublicKey> TypedKeys(std::span<ClusterMember const> bootstrap)
    {
        std::map<Consensus::NodeId, Ed25519PublicKey> typed;
        for (auto const& member: bootstrap)
            if (member.publicKey.has_value())
                typed.emplace(member.id, *member.publicKey);
        return typed;
    }
} // namespace

RosterKeys::RosterKeys(Ed25519KeyPair own, std::span<ClusterMember const> bootstrap):
    _own { std::move(own) },
    _bootstrap { TypedKeys(bootstrap) }
{
}

void RosterKeys::Adopt(ClusterState const& state)
{
    std::map<Consensus::NodeId, Ed25519PublicKey> stated;
    for (auto const& member: state.members)
        if (member.publicKey.has_value())
            stated.emplace(member.id, *member.publicKey);

    std::unique_lock const lock { _lock };
    _stated = std::move(stated);
    _revoked = state.revokedKeys;
}

void RosterKeys::AdoptConfiguration(Consensus::Configuration const& configuration)
{
    auto counted = configuration.voters;
    counted.insert(counted.end(), configuration.learners.begin(), configuration.learners.end());

    std::unique_lock const lock { _lock };
    _counted = std::move(counted);
}

Ed25519PublicKey RosterKeys::OwnPublicKey() const
{
    return _own.PublicKey();
}

Ed25519Signature RosterKeys::SignAsSelf(std::span<std::byte const> message) const
{
    return _own.Sign(message);
}

Consensus::PeerKeys RosterKeys::KeysOf(Consensus::NodeId const& peer) const
{
    std::shared_lock const lock { _lock };

    // Every revoked key, whichever id the revocation named: the removed machine may claim any id,
    // so filtering on the one it was forgotten under would report it as a stranger whenever the
    // two differ. The one exception is that id itself while the configuration still counts it
    // (#1555): its own key stays live FOR IT until the configuration drops it.
    auto const counted = std::ranges::contains(_counted, peer);
    auto graced = std::vector<Ed25519PublicKey> {};
    auto keys = Consensus::PeerKeys {};
    keys.revoked.reserve(_revoked.size());
    for (auto const& revoked: _revoked)
    {
        if (counted && revoked.id == peer)
            graced.push_back(revoked.publicKey);
        else
            keys.revoked.push_back(revoked.publicKey);
    }

    auto const isRevoked = [&keys](Ed25519PublicKey const& key) {
        return std::ranges::contains(keys.revoked, key);
    };

    // The state first, because it is what the cluster agreed; a key it states is never one it
    // revoked, since the `Forget` that revokes a key removes the record holding it. Then the
    // command line's, and last the key a counted member was forgotten under -- only when it is
    // one, since two would leave nothing to say which of them the member proves with, and the
    // refusing answer is the safe one.
    if (auto const stated = _stated.find(peer); stated != _stated.end())
        keys.live = stated->second;
    else if (auto const typed = _bootstrap.find(peer); typed != _bootstrap.end() && !isRevoked(typed->second))
        keys.live = typed->second;
    else if (graced.size() == 1)
        keys.live = graced.front();

    return keys;
}

} // namespace FastCache::Cluster

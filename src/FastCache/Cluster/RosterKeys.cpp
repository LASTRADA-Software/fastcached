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

    // Every revoked key, whichever id the revocation named: that id is a LABEL (`RevokedKey`),
    // so filtering on it would report the removed machine as a stranger whenever the label and
    // the id it now claims differ.
    auto keys = Consensus::PeerKeys {};
    keys.revoked.reserve(_revoked.size());
    for (auto const& revoked: _revoked)
        keys.revoked.push_back(revoked.publicKey);

    auto const isRevoked = [&keys](Ed25519PublicKey const& key) {
        return std::ranges::contains(keys.revoked, key);
    };

    // The state first, because it is what the cluster agreed; a key it states is never one it
    // revoked, since `RevokeKey` clears the key it records.
    if (auto const stated = _stated.find(peer); stated != _stated.end())
        keys.live = stated->second;
    else if (auto const typed = _bootstrap.find(peer); typed != _bootstrap.end() && !isRevoked(typed->second))
        keys.live = typed->second;

    return keys;
}

} // namespace FastCache::Cluster

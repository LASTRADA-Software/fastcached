// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>

#include <algorithm>

/// @file SelfForgotten.hpp
/// #1539's reading of FORGOTTEN, public: what a leader asks before proposing a forgotten member's
/// removal, and what a node asks of the state it applies before it gives up the cluster it is in.
///
/// One reading, asked by both. A node that read "forgotten" differently from its leader would
/// either leave a cluster that still counts it or stay in one that has removed it.
namespace FastCache::Cluster
{

/// Whether the cluster FORGOT @p id rather than never recording it: a key revoked under that id,
/// which only `Forget` writes (#1555).
///
/// The one forget fact that outlives the record it removed, so it can be read about a member the
/// state never recorded -- every bootstrap member the configuration counts. A forget always writes
/// it: one that would revoke nothing is refused before it is proposed (`PrepareForget`). The caller
/// asks whether the id is recorded NOW: a machine admitted again under a new key is a member,
/// whatever its old key's entry says.
/// @param state The replicated state.
/// @param id A member id.
/// @return True when `revokedKeys` names it.
[[nodiscard]] inline bool IsForgottenById(ClusterState const& state, Consensus::NodeId const& id)
{
    return std::ranges::contains(state.revokedKeys, id, &RevokedKey::id);
}

/// Whether @p self has been forgotten: its record GONE, and the fact only `Forget` writes beside
/// that -- a key revoked under its id (#1539, #1555).
///
/// The record alone is not a forget: a fresh cluster's state has recorded nothing yet, and reading
/// that as forgotten would take every new cluster's only voter out of it. No host decides it: an
/// address is not an identity, and a machine is forgotten by its key wherever it dials from.
/// @param state The replicated state.
/// @param self This node's id.
/// @return True when the state says this node was forgotten.
[[nodiscard]] inline bool IsSelfForgotten(ClusterState const& state, Consensus::NodeId const& self)
{
    if (std::ranges::contains(state.members, self, &ClusterMember::id))
        return false;
    return IsForgottenById(state, self);
}

} // namespace FastCache::Cluster

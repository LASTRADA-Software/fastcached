// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Cluster
{

/// A member this node believes should be present, and how sure it is of each part.
///
/// Not a `ClusterMember`, and the difference is one field's type. A record here is
/// what somebody *knows*, assembled from a source that may not know all of it:
/// discovery proves a peer's consensus endpoint and learns nothing about the port
/// clients speak to, because nobody dials that port to find out.
struct DesiredMember
{
    Consensus::NodeId id;     ///< Stable identity; what consensus counts.
    std::string raftEndpoint; ///< Where its consensus port answers.

    /// Where clients reach it while it leads; absent when this node has no opinion.
    ///
    /// **Absent is not empty**, and that distinction is what keeps discovery from
    /// undoing a leader's own announcement. `AddMember` applies wholesale, so a
    /// proposal carrying an empty scheduler endpoint *clears* what was recorded --
    /// correct when the proposer knows the member has none, and destructive when it
    /// simply never knew. A node says `""` about itself and `nullopt` about a peer,
    /// and only the first of those is an assertion.
    std::optional<std::string> schedulerEndpoint;
};

/// What a leader should propose to make the cluster's state say what it knows.
///
/// The whole decision, as a pure function over two values: what the replicated state
/// currently holds, and what this node believes the membership ought to include. It
/// is a function rather than a method for the reason `WorkerRegistry` and
/// `LeaseTable` are pure -- every rule below is a table-driven unit test rather than
/// a cluster and a sleep -- and it is *one* function rather than one per source
/// because this node has more than one: its own record, which only it can supply,
/// and the peers discovery has proved. Two callers each deciding "is this already
/// there?" would be two places for the answer to drift.
///
/// ## What it never proposes
///
/// **A removal.** A member vanishes from what this node can see for reasons that are
/// almost never "it left": a beacon lost on a broadcast, a switch rebooting, a laptop
/// closed for an hour. Removing on absence would take a node out of the quorum the
/// moment the network hiccupped, and a cluster that re-computes its own membership
/// from reachability is one that can shrink itself below a majority and never come
/// back. Raft already tolerates a member that does not answer; leaving is an operator
/// decision, and stays one.
///
/// **A record that already matches.** Proposing one costs a log entry, a replication
/// round and a snapshot's worth of growth per beacon interval, forever. So the
/// comparison is on the *whole* record rather than on the id -- a member whose
/// scheduler endpoint has just been announced differs from the one recorded, and must
/// be re-proposed, while one that agrees in every field must not.
/// @param state The cluster's state as this node last applied it.
/// @param desired Records this node believes should be present.
/// @return The commands to propose, in `desired` order; empty when nothing differs.
[[nodiscard]] std::vector<Command> MembershipProposals(ClusterState const& state, std::span<DesiredMember const> desired);

} // namespace FastCache::Cluster

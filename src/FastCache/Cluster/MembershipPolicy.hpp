// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>

#include <span>
#include <vector>

namespace FastCache::Cluster
{

/// What a leader should propose to make the cluster's state say what it knows.
///
/// The whole decision, as a pure function over two values: what the replicated state
/// currently holds, and what this node believes the membership ought to include. It
/// is a function rather than a method for the reason `WorkerRegistry` and
/// `LeaseTable` are pure — every rule below is a table-driven unit test rather than a
/// cluster and a sleep — and it is *one* function rather than one per source because
/// this node has more than one: its own record, which only it can supply, and the
/// peers discovery has proved. Two callers each deciding "is this already there?"
/// would be two places for the answer to drift.
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
/// round and a snapshot's worth of growth per beacon interval, forever — and since
/// `AddMember` applies wholesale, a re-proposal that dropped a field would clear it.
/// So the comparison is on the *whole* record rather than on the id: a member whose
/// scheduler endpoint has just been announced differs from the one recorded, and must
/// be re-proposed, while one that agrees in every field must not.
/// @param state The cluster's state as this node last applied it.
/// @param desired Records this node believes should be present.
/// @return The commands to propose, in `desired` order; empty when nothing differs.
[[nodiscard]] std::vector<Command> MembershipProposals(ClusterState const& state, std::span<ClusterMember const> desired);

} // namespace FastCache::Cluster

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

/// The one consensus membership change that moves the quorum towards the state.
///
/// The other half of `MembershipProposals`, and the two are deliberately separate
/// questions. That one decides who the cluster has agreed *exists*; this one
/// decides who it *counts*, which is a Raft configuration change with its own
/// safety rules. Until this existed the second answer never moved: a node
/// admitted at runtime was served by every surface and voted in none, so growing a
/// cluster's consensus meant restarting its members with a longer bootstrap list.
///
/// ## One change, and which one
///
/// A configuration change may add **or** remove exactly one member (§4.3), so this
/// returns one step and is called again when it commits. Additions come first:
/// growing before shrinking keeps the quorum reachable while a replacement is in
/// progress, where the other order passes through a configuration that is smaller
/// than either endpoint.
///
/// ## What it refuses to propose
///
/// **A member with no dialable address.** Counting a node the transport cannot
/// reach is precisely the failure this whole change exists to avoid, reached from
/// the other side: the cluster's quorum grows and the votes to satisfy it cannot
/// arrive. A member already *counted* is never dropped for this, though — an
/// endpoint that has become unreadable is a bad record, and shrinking the quorum
/// over one would turn a typo into a cluster that cannot elect.
///
/// **This node's own removal.** A leader taking itself out of the quorum it leads
/// is an operator's decision and stays one — and it would not stick anyway, since
/// a node always desires its own record and the next pass would propose putting it
/// back. A configuration flapping on a timer is worse than one that is merely
/// wrong.
///
/// **The removal of a bootstrap member.** This is the one that is not obvious, and
/// getting it wrong shrinks a healthy cluster to one node: `--raft-peer` puts a
/// member in the *configuration* and nothing puts it in the *state*, so on a cluster
/// whose peers were typed rather than discovered, `state.members` holds the leader's
/// own record and nothing else. Read as "everybody else was forgotten", that
/// proposes removing every peer, one per commit, until the leader is alone and
/// refuses the others as strangers — which is what it did, exactly once, before
/// @p bootstrap existed.
///
/// So absence means removal only for a member that was **admitted at runtime**,
/// which is what tells "the operator forgot it" apart from "nobody ever wrote it
/// down". A member an operator typed into `--raft-peer` is a member by that
/// operator's own assertion, and taking it out of the quorum is their decision to
/// make by editing that line.
///
/// The bootstrap set rather than a record of what this process has observed, and
/// the difference is a restart. An observation is rebuilt from live members only, so
/// a fleet restarted after a removal — a rolling restart, or one rebuilt from its
/// command lines — would count a forgotten member forever, with a correct-looking
/// member set and nothing logged. The bootstrap set comes off the command line and
/// says the same thing after every start.
///
/// **A node given no bootstrap set proposes no removal at all**, which is the same
/// rule read at its limit rather than an exception to it. A `--raft-join` node was
/// told nothing about the cluster's shape, so every member is equally unexplained to
/// it — and once such a node is elected it would otherwise remove all of them, one
/// per commit, which is the identical failure the parameter exists to prevent
/// reached through the one path that has nothing to compare against. What it costs
/// is that a fleet whose only bootstrapped node is gone can no longer shrink its
/// quorum until one leads again; it fails closed, counting a member too many rather
/// than too few.
/// @param state The cluster's state as this node last applied it.
/// @param active The member set consensus currently counts.
/// @param self This node's id.
/// @param bootstrap The member set this node was started with; never removed.
/// @return The member set to propose, or nullopt when nothing should change.
[[nodiscard]] std::optional<std::vector<Consensus::NodeId>> NextQuorumChange(ClusterState const& state,
                                                                             std::span<Consensus::NodeId const> active,
                                                                             Consensus::NodeId const& self,
                                                                             std::span<Consensus::NodeId const> bootstrap);

} // namespace FastCache::Cluster

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <vector>

namespace FastCache::Consensus
{

/// How a node is configured to take part in a cluster.
///
/// Supplied whole at construction and fixed thereafter, so a constructed
/// `RaftNode` is a usable one: there is no `SetPeers()` and no second phase in
/// which a node exists but does not yet know who it is. Changing the member set
/// of a *running* cluster is a different operation with its own safety rules —
/// it goes through the log as a configuration entry rather than by mutating this.
struct RaftConfig
{
    /// This node's own identity; must appear in `voters` or `learners` unless both
    /// are empty.
    NodeId self;

    /// Every voting member of the cluster, **including this node** when it is one.
    ///
    /// Including self rather than listing "peers" because the member set is what
    /// Raft's configuration actually is, and quorum is a property of the whole
    /// set. A peers-only list makes the reader do the +1 at every use, which is
    /// the arithmetic an off-by-one in a quorum calculation hides in.
    ///
    /// **Empty, with `learners` empty too, is a legal and meaningful value: this
    /// node has no cluster yet.** It is what a machine being added to a running
    /// fleet starts with, and it is the only shape that can be added at all. A node
    /// that bootstrapped itself as the sole member of its own cluster is not a
    /// candidate for admission: it has elected itself, holds a term and a log of its
    /// own, and refuses `AppendEntries` from a leader its configuration has never
    /// heard of -- so a cluster that admitted it would count towards quorum a node
    /// that answers nobody. Two clusters cannot be merged by any local rule, which is
    /// why the joining node must never have formed one. See `RaftNode::HasCluster`.
    std::vector<NodeId> voters;

    /// Members that are replicated to and counted by nothing (#1449).
    ///
    /// Disjoint from `voters`. A node bootstrapped as one of these never stands and
    /// never votes, which is what lets a usually-absent machine be part of the
    /// cluster without its absence costing a quorum. See `Configuration`.
    std::vector<NodeId> learners {};

    /// Lower bound of the randomized election timeout.
    std::chrono::milliseconds electionTimeoutMin { 150 };

    /// Upper bound of the randomized election timeout.
    ///
    /// The *spread* is what breaks a split vote: every node draws its own timeout
    /// from this range, so two candidates that stood simultaneously are unlikely
    /// to time out together again. A zero spread is legal but reduces the
    /// algorithm to one that can repeat a split vote indefinitely.
    std::chrono::milliseconds electionTimeoutMax { 300 };

    /// How often a leader sends heartbeats.
    ///
    /// Must be comfortably below `electionTimeoutMin`: a heartbeat that arrives
    /// no more often than followers time out means followers depose a healthy
    /// leader on a regular basis, which does not corrupt anything and does mean
    /// the cluster spends its time electing instead of working.
    std::chrono::milliseconds heartbeatInterval { 50 };

    /// Validate the configuration.
    ///
    /// A separate check returning `std::expected` rather than a throwing
    /// constructor, so a daemon can refuse to start with a message naming the
    /// field instead of dying on an assertion. This is deliberately the layer
    /// that rejects an inverted election-timeout range: `IRandomSource` defines
    /// that case rather than diagnosing it, precisely because only here is it
    /// known that the two bounds came from a configuration file and can be named
    /// back to whoever wrote them.
    /// @return Nothing on success, or what is wrong with it.
    [[nodiscard]] std::expected<void, ConsensusError> Validate() const;

    /// The bootstrap configuration these two lists describe.
    /// @return `voters` and `learners`, as one value.
    [[nodiscard]] Configuration Bootstrap() const;
};

} // namespace FastCache::Consensus

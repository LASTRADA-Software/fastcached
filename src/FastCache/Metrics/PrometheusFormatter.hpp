// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace FastCache
{

/// Strongly-typed process uptime. A thin box around `std::chrono::seconds` so
/// call sites read `Uptime { 42s }` rather than passing a bare, unit-ambiguous
/// integer that needs a `/* uptimeSeconds */` comment to be legible.
struct Uptime
{
    std::chrono::seconds value { 0 };
};

/// The size of the machine, as a scrape reports it.
///
/// A struct rather than four `IMetricsSink` counters, because none of these is a
/// count of anything: they are gauges, and this interface is counter-only by
/// design. Bundled with the storage snapshot for the same reason that one is —
/// the renderer takes one value per scrape and the server needs no collaborators
/// of its own.
struct HostCapacity
{
    std::size_t logicalCores { 0 };    ///< Schedulable hardware threads.
    std::size_t configuredSlots { 0 }; ///< Concurrent compiles this node advertises.

    // Every byte count is `uint64_t` and not `size_t`/`uintmax_t`, which is what the
    // two sources happen to return. These are numbers this node will state to a
    // scheduler on another machine, so their width has to be a property of the fact
    // rather than of whoever measured it -- and a 32-bit host would otherwise report
    // a machine with more than 4 GiB of memory as having rather less.
    std::uint64_t totalMemoryBytes { 0 };  ///< Physical memory, or the container's ceiling.
    std::uint64_t diskCapacityBytes { 0 }; ///< Size of the filesystem the work happens on.
    std::uint64_t diskFreeBytes { 0 };     ///< What an unprivileged process may still write.

    /// Compiles running right now.
    ///
    /// A gauge, and the one number here that moves. Sampled at scrape time rather
    /// than derived from `WorkerJobsStarted - WorkerJobsCompleted`, because those
    /// two counters are incremented by different components and a scrape landing
    /// between them would report a phantom job — the difference is right on
    /// average and wrong at exactly the moment somebody is looking.
    std::size_t busySlots { 0 };
};

/// What a node believes about its OWN consensus configuration.
///
/// The gap [#435](https://github.com/LASTRADA-Software/fastcached/issues/435) is
/// about: `--cluster-status` reports `ClusterState.members`, which is the FLEET's
/// member record and a different set from the quorum, and only the leader answers
/// it at all — so the one node whose view an operator needs during a stall is the
/// one that redirects them elsewhere. Nothing else could be asked what a given node
/// counts. #388 is what that costs: a joiner that received the state record and
/// never adopted the configuration entry has `HasCluster()` false, so
/// `NextDeadline()` excuses it from every deadline; it campaigns in no election,
/// grants no pre-vote, logs nothing, and is indistinguishable from a healthy
/// follower until the leader dies and the cluster cannot re-elect.
///
/// **Gauges, every one, and none of them is a counter.** These describe a state
/// rather than tally events, and `IMetricsSink` is counter-only by design — so they
/// arrive in the snapshot beside `host` and `upstreamConfigured` rather than as
/// `MetricsCatalog` rows. That is also what makes absence spellable: a *counter* is
/// a tally where zero is the truth about events that never happened, while a
/// *reading* of zero is a claim about the world.
///
/// **`members` being empty is a reading and not an absence**, which is the one
/// distinction worth getting right here. A process that runs no consensus leaves
/// `MetricsSnapshot::consensus` disengaged and renders no consensus line at all; a
/// node that RUNS consensus and holds no configuration renders an empty member set,
/// because that is precisely the #388 state and hiding it would defeat the ticket.
/// The two are told apart by whether the block is there, never by a zero inside it.
struct ConsensusStatus
{
    /// The member set the local Raft node operates under, in whatever order
    /// consensus holds it.
    ///
    /// Empty means this node holds no configuration — the legitimate waiting state
    /// of a `--raft-join` node, and a fatal one for any other. Its SIZE is
    /// `HasCluster()`, which is why no separate boolean is carried: `HasCluster()`
    /// is defined as `!members.empty()`, and a second field saying the same thing is
    /// a second thing to be wrong.
    std::vector<Consensus::NodeId> members {};

    /// Who this node believes leads, if anybody.
    ///
    /// Disengaged during an election, which is a different fact from "somebody else
    /// leads" and the one a client cannot act on. It is NOT constrained to
    /// @ref members: a node with no configuration accepts entries from any leader,
    /// so it can name one while counting nobody — which is the #388 shape exactly.
    std::optional<Consensus::NodeId> knownLeader {};

    /// The term this node is operating in.
    ///
    /// A gauge and not a counter, although it only ever rises within one state
    /// directory: a term is a reading, and wiping `--cluster-dir` legitimately
    /// resets it to zero. A counter that resets is one a scraper renders as a
    /// spike of the whole history.
    Consensus::Term term {};

    /// How far this node's log is committed.
    ///
    /// Carried because it is the one number that separates "this node has adopted a
    /// configuration" from "this node is being caught up": a joiner sits at a commit
    /// index far behind its peers for as long as the walk-back takes.
    Consensus::LogIndex commitIndex {};

    /// What this node is playing.
    ///
    /// Last, so the one byte-wide member sits at the end rather than between two
    /// 8-aligned ones; see the padding rule in
    /// [`.agent/rules/build-and-toolchain.md`](../../../.agent/rules/build-and-toolchain.md).
    Consensus::Role role {};
};

/// Everything a `/metrics` scrape needs that varies per call: the storage
/// snapshot plus the process uptime. Produced by the admin server's snapshot
/// provider so the renderer takes a single bundle and the server itself needs
/// no clock.
struct MetricsSnapshot
{
    /// The cache's own statistics, absent in a process that has no cache.
    ///
    /// Optional rather than a default-constructed `StorageStats`, because a
    /// worker has no storage and zeroes are not the truth about it: a scrape
    /// reporting `fastcached_items 0` and `fastcached_bytes_limit 0` says the
    /// cache is empty and unbounded, which an alert or a dashboard will read as
    /// a fact rather than as an absence. `fastcache-compile-node` runs the same
    /// `AdminHttpServer` and shares this renderer, so the alternative was a
    /// second renderer that drifts from this one.
    std::optional<StorageStats> storage;

    /// The same cache, split by the tier holding each number.
    ///
    /// Not a second source for what `storage` already says: `storage` is the
    /// cache's answer about ITSELF, and a composite has to choose one tier's item
    /// count to give it -- `LayeredStorage` gives L2's, so a node's whole
    /// in-memory tier is missing from every scrape that reads only that field.
    /// These are the numbers that choice discards.
    ///
    /// Every entry is optional and an absent one renders nothing at all, for the
    /// reason `storage` is optional: a memory-only cache has no disk tier, and a
    /// disk tier reporting zero bytes is a claim a dashboard draws as "empty".
    /// A process with no cache leaves them all absent, which is the default.
    TieredStorageStats storageTiers {};

    /// What this machine is and how much room it has, absent when the process has
    /// no reason to say.
    ///
    /// A worker's capacity is the thing a fleet operator most wants off a scrape —
    /// "is this node pulling its weight" is unanswerable without knowing how big
    /// it is — and it is what PR 8's resource-aware scheduling will weigh. The
    /// daemon leaves it absent: it is not a compile node, and reporting cores it
    /// does not schedule against would be noise.
    std::optional<HostCapacity> host;

    /// Whether this node has a shared cache to read through to, absent when the
    /// question does not apply.
    ///
    /// The daemon leaves it absent -- it IS the shared cache -- and a compile node
    /// answers it. It exists because the upstream counters cannot: they are
    /// cumulative, so a node with no upstream and a node with one it has not yet
    /// written to both read zero, and an operator cannot tell a developer laptop
    /// from a fleet whose shared cache is unreachable.
    ///
    /// A gauge rather than an absent counter row on purpose. The counters are
    /// exported in full, deliberately (see the loop below), and a per-counter
    /// "does this apply?" predicate is the mechanism that once left seven of nine
    /// live counters unexported. Absence is modelled HERE, in the snapshot, the way
    /// `storage`, `storageTiers` and `host` already model it.
    /// One carve-out, and it is a different QUESTION rather than a softening of
    /// this one: a row this BUILD cannot represent is omitted and counted by
    /// `fastcached_metrics_catalogue_skew` (#1353). It is argued once, in
    /// `.agent/rules/metrics-and-observability.md`, and nothing about it licenses
    /// dropping a row for the *does this apply to my deployment* reason above.
    ///
    /// Default-initialized like `storageTiers`, and for a mechanical reason as well
    /// as a semantic one: a designated-initializer list that omits a field WITHOUT a
    /// default member initializer is a clang-tidy error, so a field added without one
    /// breaks every exhaustive brace-init of this struct in the tree.
    std::optional<bool> upstreamConfigured {};

    /// What this node believes about its own Raft cluster, absent when it runs no
    /// consensus at all.
    ///
    /// Absent for the daemon, which has none, and for a compile node started
    /// without `--listen-raft`, which leads itself and holds no configuration for
    /// anybody to read. Present — with an empty member set — for a node that runs
    /// consensus and has not yet been admitted to anything, because that is a
    /// *reading* and the one #435 exists to make observable. See `ConsensusStatus`.
    std::optional<ConsensusStatus> consensus {};

    Uptime uptime {};
};

/// Render the Prometheus text exposition format (version 0.0.4) for the given
/// connection-level counters and storage statistics.
///
/// Pure and free of I/O so it can be unit-tested directly. Command-level
/// counters (gets, sets, hit/miss splits, evictions, capacity) come from the
/// storage snapshot — the authoritative source — while connection-level
/// counters come from the metrics sink. Each metric is emitted with its
/// `# HELP` / `# TYPE` lines followed by the `fastcached_<name> <value>` sample.
///
/// @param metrics Connection-level counter sink.
/// @param snapshot Per-scrape storage stats and process uptime.
/// @return A complete metrics body in Prometheus text exposition format.
[[nodiscard]] std::string RenderPrometheus(IMetricsSink const& metrics, MetricsSnapshot const& snapshot);

} // namespace FastCache

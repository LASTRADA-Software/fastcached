// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>

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

    [[nodiscard]] bool operator==(Uptime const&) const = default;
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

    [[nodiscard]] bool operator==(HostCapacity const&) const = default;
};

/// What a machine is doing right now, in the form every consumer can difference itself.
///
/// The moving half of `HostCapacity`, and a block of its own rather than more fields there,
/// because every field here can be absent where every field there cannot: a platform that
/// will not report its CPU leaves `cpu` disengaged, and that is a different claim from an
/// idle machine (`HostLoad` argues it at length).
///
/// **CPU travels as the raw cumulative counters, never as a utilization.** A utilization is
/// the difference between two readings, so something has to hold the earlier one -- and a
/// node serving `/metrics`, several live subscriptions and its own heartbeat would have them
/// cutting each other's intervals, each seeing whatever slice the last reader left. Raw
/// counters leave the baseline with the reader: `CpuBusyPermille(previous.cpu, now.cpu)` over
/// two readings it holds itself, which is the rule a history already follows (a counter is
/// stored raw; the rate is taken at render).
///
/// Free scratch space is not here: `HostCapacity::diskFreeBytes` already reads the scratch
/// filesystem, and a second figure for one fact is a second thing to be wrong.
struct HostLoadReading
{
    /// The machine's cumulative CPU counters, or absent when the platform would not say.
    std::optional<CpuTicks> cpu {};
    /// Memory a new process could actually obtain, or absent when the platform would not say.
    std::optional<std::uint64_t> availableMemoryBytes {};

    [[nodiscard]] bool operator==(HostLoadReading const&) const = default;
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

    [[nodiscard]] bool operator==(ConsensusStatus const&) const = default;
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
    /// What this machine is doing right now, absent when the process samples no load.
    ///
    /// Absent for the daemon for the reason `host` is, and present on a compile node -- whose
    /// available slots are decided by exactly these figures (`Distributed::SlotCeilingsFor`),
    /// so a dashboard that could not see them could name no reason a node offers fewer slots
    /// than it registered. Each figure inside is absent on its own when the platform would not
    /// report it. See `HostLoadReading`.
    std::optional<HostLoadReading> hostLoad {};

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

    [[nodiscard]] bool operator==(MetricsSnapshot const&) const = default;
};

/// One reading of every figure a stats surface reports: the single source of truth `/metrics`
/// and a live-stats snapshot are both encoded from.
///
/// **Why one struct and not two renderings of the same sources.** A scrape reads the sink and
/// the per-call snapshot and writes text; a live subscription would otherwise read the same
/// sources and write bytes, and two walks over one set of sources is two places for a figure
/// to be added to one and not the other -- the failure `MetricsCatalog` exists to prevent, one
/// level up. So the sources are read ONCE, into this, and each encoding is a pure function of
/// it: `RenderPrometheus` for the text, `EncodeStatsReading` for the binary, and
/// `DecodeStatsReading` hands a client the same struct back
/// ([#1399](https://github.com/LASTRADA-Software/fastcached/issues/1399)).
///
/// Plain data, owning everything it holds, so a decoded reading outlives the bytes it came
/// from (the `*View` rule in `.agent/rules/wire-and-protocol.md`).
struct StatsReading
{
    /// Every catalogue counter as the sink stood when this was captured.
    ///
    /// **Absent when this build's sink has no slot for the row**, never zero (#1353): a
    /// catalogue compiled against a newer `Counter` than the sink is a build that cannot state
    /// the figure, and a zero would be indistinguishable from an idle counter.
    CounterCells<std::optional<std::uint64_t>> counters {};

    /// Everything else, as the snapshot provider stated it. Every absence it models stays one.
    MetricsSnapshot snapshot {};

    /// The build that captured this reading, as `VersionString` spells it; empty when whoever produced the
    /// reading did not say.
    ///
    /// **In the reading rather than beside one encoding**, because a live-stats panel titles itself with it
    /// and reads every figure from this struct: `/metrics` carrying it as `fastcached_build_info` while the
    /// binary form carried nothing would be one fact stated by one encoding only (#134, #1399). Captured,
    /// not a constant each encoder reads, so a DECODED reading states the build that sent it rather than the
    /// build decoding it.
    std::string version {};

    /// Every counter and every block. What the round trip through the binary form must preserve.
    [[nodiscard]] bool operator==(StatsReading const&) const = default;
};

/// Read the sink and a per-call snapshot into one `StatsReading`, stamped with this build's version.
///
/// The one place the counters are read, so the question #1353 asks -- does this sink carry
/// the row at all? -- is asked once, here, rather than by every encoding.
/// @param metrics The counter sink.
/// @param snapshot What the provider stated for this call.
/// @return The reading.
[[nodiscard]] StatsReading CaptureStatsReading(IMetricsSink const& metrics, MetricsSnapshot const& snapshot);

} // namespace FastCache

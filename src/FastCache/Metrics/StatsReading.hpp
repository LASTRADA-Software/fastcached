// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    /// 1 while an operator has cordoned this node's worker, 0 otherwise (#1303).
    ///
    /// A gauge beside `busySlots` because the two are read together: a cordoned node with
    /// compiles running is draining, and one with none is safe to stop. A `size_t` rather
    /// than a `bool` so the struct stays one run of 8-byte fields -- the live-stats codec's
    /// completeness check is a size comparison -- and so it renders as the 0/1 gauge an
    /// alert (`fastcache_node_cordoned == 1 for 1d`) is written against.
    ///
    /// Without it a dashboard reads the cordoned node's free slots as capacity the fleet
    /// has, while the scheduler -- which hears the cordon in the heartbeat -- gives it none.
    std::size_t cordoned { 0 };

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
/// **`configuration` being empty is a reading and not an absence**, which is the one
/// distinction worth getting right here. A process that runs no consensus leaves
/// `MetricsSnapshot::consensus` disengaged and renders no consensus line at all; a
/// node that RUNS consensus and holds no configuration renders an empty configuration,
/// because that is precisely the #388 state and hiding it would defeat the ticket.
/// The two are told apart by whether the block is there, never by a zero inside it.
struct ConsensusStatus
{
    /// The configuration the local Raft node operates under -- its voters and its
    /// learners, each in whatever order consensus holds them (#1449).
    ///
    /// Both sets empty means this node holds no configuration — the legitimate
    /// waiting state of a `--raft-join` node, and a fatal one for any other. That is
    /// `HasCluster()`, which is why no separate boolean is carried: a second field
    /// saying the same thing is a second thing to be wrong. Where THIS node sits in
    /// it is `Consensus::Membership::StandingOf`, asked with the node's own id, which
    /// this record does not carry and so does not restate.
    Consensus::Configuration configuration {};

    /// Who this node believes leads, if anybody.
    ///
    /// Disengaged during an election, which is a different fact from "somebody else
    /// leads" and the one a client cannot act on. It is NOT constrained to
    /// @ref configuration: a node with no configuration accepts entries from any leader,
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

    /// Whole seconds until the roster this node verifies lease grants against stops being
    /// certified, 0 once it has (#178) -- ABSENT on a node whose roster has no certificate to
    /// lapse: a consensus member, whose roster is the state it applied, and a node that holds
    /// none. A worker past zero, and the clock-skew slack, refuses every grant `roster-expired`,
    /// so this is the reading an alert on a withholding or unreachable leader watches.
    std::optional<std::uint64_t> rosterExpiresInSeconds {};

    Uptime uptime {};

    [[nodiscard]] bool operator==(MetricsSnapshot const&) const = default;
};

/// Why a counter read back as nothing.
///
/// **Two absences with one spelling is how a healthy process comes to report a broken build.**
/// Before [#1484](https://github.com/LASTRADA-Software/fastcached/issues/1484) a reading carried
/// `std::optional<std::uint64_t>` per counter and every absence meant the first reason below.
/// `RenderPrometheus` reads that absence, omits the series with a `# SKEW` marker and bumps
/// `fastcached_metrics_catalogue_skew` -- so giving the one spelling a second reason made
/// `fastcache-compile-node`, which simply has no writer for the daemon's accept counters, scrape
/// as a build whose catalogue and sink disagree. That is a confident wrong signal replacing a
/// vague one, and it costs the skew counter its meaning.
///
/// **The reason travels on the wire** rather than being re-derived by the renderer, because the
/// renderer is not always in the process that captured the reading: `fastcache-cli`'s poll rung
/// decodes a reading and renders Prometheus from it (`StatsSource.cpp`), so a local derivation
/// would be a fact the CLI cannot reach.
///
/// Asked in the order declared: a build that cannot state the figure AT ALL outranks a process
/// that merely never writes it.
enum class CounterAbsence : std::uint8_t
{
    /// This build's sink has no slot for the row -- a catalogue compiled against a newer
    /// `Counter` than the sink it is reading
    /// ([#1353](https://github.com/LASTRADA-Software/fastcached/issues/1353)).
    /// `IMetricsSink::Carries` is the question, and a skew is what this means.
    NoSlotInThisBuild = 0,

    /// This build can state the figure and this PROCESS has no writer for it: the row belongs to
    /// a surface this binary does not serve (#1484). **Not a skew**, so it must never be counted
    /// as one -- that is the whole reason this enumerator exists rather than a second `bool`.
    NoWriterInThisProcess,

    /// Enumerator count. Not a reason.
    Last
};

/// One counter as a reading holds it: a value, or an absence that says why.
///
/// Not `std::expected<std::uint64_t, CounterAbsence>`, for one decisive reason:
/// `std::expected` default-constructs to a VALUE, so a zero-initialised table of 144 of them
/// reads as 144 present zeroes -- which is the exact defect this type exists to prevent, arriving
/// by the front door. The default here is ABSENT, and absent for #1353's reason, which is what an
/// untouched cell meant before this type existed.
///
/// `operator==` is defaulted and correct because the unused member of each state is left at its
/// default by both factories: a present reading carries `NoSlotInThisBuild`, an absent one
/// carries `0`. That is what keeps `StatsReading::operator==` defaulted too, and with it the
/// codec's round-trip assertion.
class CounterReading
{
  public:
    /// An absent reading, for #1353's reason -- what an untouched cell means.
    constexpr CounterReading() noexcept = default;

    /// A reading that has a value.
    /// @param value The counter's value.
    /// @return The reading.
    [[nodiscard]] static constexpr CounterReading Of(std::uint64_t value) noexcept
    {
        CounterReading reading;
        reading._present = true;
        reading._value = value;
        return reading;
    }

    /// A reading that has no value, and says why.
    /// @param why Which absence this is.
    /// @return The reading.
    [[nodiscard]] static constexpr CounterReading None(CounterAbsence why) noexcept
    {
        CounterReading reading;
        reading._why = why;
        return reading;
    }

    /// Whether this reading carries a value.
    /// @return True when it does.
    [[nodiscard]] constexpr bool Present() const noexcept
    {
        return _present;
    }

    /// The value. A programmer error to ask when `Present()` is false, so an assert rather than
    /// a sentinel: a zero returned here would be the very figure this type refuses to invent.
    /// @return The value.
    [[nodiscard]] constexpr std::uint64_t Value() const noexcept
    {
        assert(_present);
        return _value;
    }

    /// Why there is no value. A programmer error to ask when `Present()` is true.
    /// @return The reason.
    [[nodiscard]] constexpr CounterAbsence Why() const noexcept
    {
        assert(!_present);
        return _why;
    }

    /// Both readings in the same state, carrying the same thing.
    [[nodiscard]] constexpr bool operator==(CounterReading const&) const = default;

  private:
    bool _present { false };
    CounterAbsence _why { CounterAbsence::NoSlotInThisBuild };
    std::uint64_t _value { 0 };
};

/// A surface a process may serve, for deciding which counters it could ever write.
///
/// Deliberately small and deliberately not a list of PROCESS KINDS: "the daemon" and "a compile
/// node" is a vocabulary that grows one enumerator per binary and answers nothing about a binary
/// that serves half of one. A surface is what a counter belongs to, which is the question
/// `CounterSoleWriterTable` below asks.
/// **PRIVATE, never transmitted or persisted**, so the explicit `= N` on the first enumerator is
/// the anchor for a table rather than a wire contract, and an insertion between two enumerators
/// is free. Which kind an enum is has to be said at its declaration, because *no comment* means
/// both in a tree holding both (`.agent/rules/wire-and-protocol.md`).
///
/// Each row names the production files that write its counters, because that is the evidence the
/// attribution rests on and a surface whose writers nobody can point at is a guess.
/// `counter-attribution` re-derives every one of them from the tree and fails when a file stops
/// writing what its surface claims, so this list cannot rot into a list of names.
enum class MetricsSurface : std::uint8_t
{
    /// The cache daemon's accept path: `Server/Server.cpp` and `Server/ReactorServerLoop.cpp`,
    /// established by reading the tree as the only writers of `fastcached_connections_*`.
    /// `fastcache-compile-node` serves none of it -- its own accept path has separate counters.
    CacheAcceptPath = 0,

    /// The daemon's storage and expiry cycle: `Cache/CacheEngine.cpp`, `Cache/ExpiryReaper.cpp`
    /// and `Cache/ReclaimLog.cpp`. A process with no `CacheEngine` reclaims nothing.
    CacheStorage,

    /// The daemon's `0xFC` executor, `Protocol/CompileCacheHandler.cpp` -- the compile-cache
    /// surface the DAEMON serves. The node's own `0xFC` door is `NodeFrameEndpoint`: two
    /// surfaces, because one process can serve either without the other.
    CacheCompileSurface,

    /// The live-stats subscription, `Protocol/LiveStream.cpp` and the node's
    /// `LiveStatsResponder.cpp`. Both binaries serve it, which is why it is its own surface
    /// rather than a column of the two above.
    LiveStats,

    /// The Raft peer wire, `Consensus/RaftPeerRefusals.hpp`. A node that runs no consensus
    /// refuses every one of these before a connection forms, so it writes none of them.
    ConsensusPeerWire,

    /// The fleet scheduler: `Distributed/SchedulerService.cpp`, `SchedulerProtocol.cpp` and
    /// `FleetView.hpp`. Only a node started with `--serve-scheduler` builds one.
    CompileScheduler,

    /// The compile worker, on both sides of the dispatch: `fastcache-cc`'s `WorkerProtocol.cpp`
    /// and `CodecEnvelope.cpp`, the node's `CompileCapacity.hpp`, `CompileResponder.*` and
    /// `WorkerTier.cpp`, and `Distributed/LeaseToken.hpp`'s worker-side outcome rows.
    /// `--slots=0` means no worker, and then none of these can move.
    CompileWorker,

    /// The enrollment window, `EnrollmentResponder.cpp`. Served only where consensus and a
    /// scheduler tier are both running, which is `ServesEnrollment`.
    NodeEnrollment,

    /// The node's own cache tier, `LocalCache.cpp` and `CacheProxy.cpp`. `--cache-memory 0`
    /// means no tier, and a node without one serves its compiles straight through.
    NodeCacheTier,

    /// The node's shared `0xFC` listener and the responders behind it: `FrameEndpoint.cpp`,
    /// `Responders.hpp`, `MembershipGate.hpp`, `NodeProofResponder.cpp`,
    /// `NodeStatusResponder.cpp` and `FleetTextResponder.cpp`. Always served by the node and
    /// never by the daemon.
    NodeFrameEndpoint,

    /// LAN discovery, `Cluster/DiscoveryService.cpp` (#178). Served only where `--discovery` is
    /// configured, which also needs consensus -- so a consensus node without it moves none of
    /// these, and reporting them as zeroes there would be a plausible wrong answer.
    NodeDiscovery,

    /// Enumerator count. Not a surface.
    Last
};

/// Every surface this build knows: what a process that has not stated otherwise serves.
///
/// The DEFAULT for the convenience overloads, and the safe direction. A surface set left too
/// wide reports a plausible zero -- the cost this tree already pays and that somebody has
/// already been surprised by. One left too narrow invents a `-`, which reads as *this process
/// does not do that* and nobody re-checks. So an unrevisited call site keeps today's behaviour,
/// and narrowing is always a positive act.
inline constexpr std::array EverySurface {
    MetricsSurface::CacheAcceptPath,   MetricsSurface::CacheStorage,      MetricsSurface::CacheCompileSurface,
    MetricsSurface::LiveStats,         MetricsSurface::ConsensusPeerWire, MetricsSurface::CompileScheduler,
    MetricsSurface::CompileWorker,     MetricsSurface::NodeEnrollment,    MetricsSurface::NodeCacheTier,
    MetricsSurface::NodeFrameEndpoint, MetricsSurface::NodeDiscovery,
};

static_assert(EverySurface.size() == static_cast<std::size_t>(MetricsSurface::Last),
              "EverySurface must list every MetricsSurface: a surface missing from it silently "
              "narrows every defaulted call site, which invents absences rather than zeroes");

/// A counter whose only writer is one named surface.
struct CounterSoleWriter
{
    IMetricsSink::Counter counter; ///< The row.
    MetricsSurface surface;        ///< A surface that writes it.
};

/// Which surface writes each catalogue row, so a process not serving that surface reports the
/// row ABSENT rather than as a plausible zero.
///
/// **One row per (counter, surface) PAIR, and a counter may have several.** A set-valued field
/// would be a fixed-size array carrying exactly one element for 164 of 165 counters, to serve
/// the single row -- `LiveSubscriptionsRevoked` -- written from two components. Two rows say
/// the same thing with no arithmetic, and `CounterHasAWriterIn` folds them.
///
/// **A row absent from this table may be written anywhere**, which is what this tree assumed
/// for every row before #1484: an omission reports exactly as the tree already did, while a row
/// wrongly ADDED invents a silent absence for a figure that is real. Those directions are not
/// symmetric -- a plausible zero is a known cost somebody has already been surprised by, and a
/// `-` reads as *this process does not do that* and nobody re-checks it.
///
/// **That asymmetry is why completeness here is safe and narrowing a BINARY is not.** This
/// table says where a counter is written, which is a fact about the source; what decides
/// whether a figure is hidden is the surface set a PROCESS states. So the table is complete and
/// the statements stay conservative: `fastcached` still passes `EverySurface` and renders every
/// row, and only `fastcache-compile-node`, whose components are decided by flags it already
/// parses, narrows. Attributing a row wrongly here cannot hide anything on a process that
/// claims every surface.
///
/// **Completeness is a `static_assert`, not a convention.** Every `Counter` enumerator has at
/// least one row below, checked at compile time -- so the failure this ticket was filed about,
/// a row silently absent from the attribution and indistinguishable from one nobody had
/// considered, cannot recur by omission ([#1501](https://github.com/LASTRADA-Software/fastcached/issues/1501)).
///
/// **How the 165 rows are attributed**, since a scan for `Increment(Counter::X)` finds only 45
/// of them and would have rendered the other 120 absent -- the same defect as the bug, three
/// times larger. The rows are written by four mechanisms, and reading only `SurfaceRefusal`
/// tables (the obvious reading of *written through `Refuse(row)`*) reaches 109 of the 120 and
/// leaves eleven looking unwritten:
///
/// | mechanism | rows |
/// |---|---|
/// | a `SurfaceRefusal` row, spent by `Refuse(row)` | 110 |
/// | a `LeaseToken.hpp` outcome row's `workerCounter` | 10 |
/// | returned by a classifier for its caller to spend | 4 |
/// | `Increment(Counter::X)` directly | 45 |
///
/// The column sums past 165 because four rows are written two ways -- and the 109 above is not
/// the 110 here: 110 rows HAVE a refusal row, and 109 of those have no increment site, which is
/// what a `SurfaceRefusal`-only reading would reach. Two figures one apart, measuring different
/// things, is exactly how a census comes to be quoted wrong, so both are asserted.
///
/// **The figures are pinned rather than pointed at**, which the rulebook asks for a
/// measurement's conditions: they describe this tree at one instant and must not silently
/// re-attribute themselves to a later one. `ctest -R counter-attribution` re-derives every one
/// of them from the source and fails when they drift -- so a stale figure here is a red build
/// rather than a sentence nobody re-reads, which is what the previous "106 of 144" became.
inline constexpr std::array CounterSoleWriterTable {
    CounterSoleWriter { .counter = IMetricsSink::Counter::ConnectionsTotal, .surface = MetricsSurface::CacheAcceptPath },
    CounterSoleWriter { .counter = IMetricsSink::Counter::ConnectionsAdmissionRejected,
                        .surface = MetricsSurface::CacheAcceptPath },
    CounterSoleWriter { .counter = IMetricsSink::Counter::ConnectionsTotalTls, .surface = MetricsSurface::CacheAcceptPath },
    CounterSoleWriter { .counter = IMetricsSink::Counter::ConnectionsAdmissionRejectedTls,
                        .surface = MetricsSurface::CacheAcceptPath },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesGranted,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesReleased,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesNoWorker,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesNoCapacity,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesWithdrawn,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesDuplicate,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchWorkerRegistrations,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchWorkerEndpointMismatch,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchWorkersExpired,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchWorkersWithdrawn,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesReclaimed,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesUnauthorized,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchLeasesReleasedLate,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchFramesRefusedUnsupportedVersion,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchFramesRefusedUnknownOpcode,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchFramesRefusedNotPermitted,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DispatchFramesRefusedTruncated,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsStarted, .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsCompleted, .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerCompileMillisTotal,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsAbandonedClientGone,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedRejectedArgument,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedScratchUnavailable,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedSpawnFailed,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedCompilerUnclassified,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedSurveyInFlight,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedNoSlot,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedNotAMember,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedStopping,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedCordoned,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerCordonsRefusedNotLocal,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnsupportedVersion,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedTruncated,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnknownOpcode,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnimplementedVerb,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedNotPermitted,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedPayload,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedPayloadTooLarge,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedCredential,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedRejectedCredential,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerFramesRefusedUnauthenticated,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeMalformed,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeUnsupportedCodec,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeDeclaredTooLarge,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedEnvelopeCorrupt,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnregistered,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseWrongCluster,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseReplayed,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseEndpointMismatch,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerSchedulerTermRegressions,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerScratchRootsReclaimed,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerBytesReceived, .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerBytesReturned, .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheHits, .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheMisses, .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheUpstreamHits, .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheFillFailures, .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheStoreFailures, .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheUpstreamStores,
                        .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheUpstreamStoreFailures,
                        .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeRequestsRefusedHostForgotten,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedPayloadTooLarge,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedEndpointBusy,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeAdmissionExplanationsRefusedMalformed,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedPayloadTooLarge,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedEndpointBusy,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedUnsupportedVersion,
                        .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedMalformedPayload,
                        .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedForeignGeneration,
                        .surface = MetricsSurface::NodeCacheTier },
    CounterSoleWriter { .counter = IMetricsSink::Counter::SchedulerRequestsRefusedUnauthenticated,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::SchedulerCredentialsRejected,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::SchedulerCredentialsMalformed,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeFrameConnectionsRefusedAtCapacity,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeProofsAccepted, .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeProofsRejected, .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeProofsUnchallenged,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::NodeProofsMalformed,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::KeyspaceReclaimEventsDropped,
                        .surface = MetricsSurface::CacheStorage },
    CounterSoleWriter { .counter = IMetricsSink::Counter::ExpiryCycles, .surface = MetricsSurface::CacheStorage },
    CounterSoleWriter { .counter = IMetricsSink::Counter::ExpiryKeysReclaimed, .surface = MetricsSurface::CacheStorage },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheMalformedValues, .surface = MetricsSurface::CacheStorage },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheFramesRefusedUnsupportedVersion,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheFramesRefusedUnknownOpcode,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheFramesRefusedPayloadTooLarge,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheFramesRefusedUnauthenticated,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedPayload,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedCredential,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheCredentialsRejected,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheStoresRefusedNotACompileValue,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheStoresRefusedForeignGeneration,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::CacheStoresFailed,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FrameRequestDeadlineSweeps,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FrameAnswerDeadlineSweeps,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FrameDeadlineRefusalsSent,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FramePeerWatchDepartures,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FramePeerWatchDeparturesObserved,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FramePeerWatchDeparturesAbortive,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedClosed,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedFull,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentControlRefusedNotAMember,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentControlRefusedUnauthenticated,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentWindowsOpened,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::EnrollmentRostersServed,
                        .surface = MetricsSurface::NodeEnrollment },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DiscoveryProofsRefusedUnknownKey,
                        .surface = MetricsSurface::NodeDiscovery },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DiscoveryProofsRefusedRevokedKey,
                        .surface = MetricsSurface::NodeDiscovery },
    CounterSoleWriter { .counter = IMetricsSink::Counter::DiscoveryProofsRefusedForged,
                        .surface = MetricsSurface::NodeDiscovery },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsOpened, .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSnapshotsRendered, .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSnapshotsSkipped, .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRevoked,
                        .surface = MetricsSurface::CacheCompileSurface },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRevoked, .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsEndedNotLeader,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedAtCapacity,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsStalled, .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsEndedByClient,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsEndedByReset,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedPayloadTooLarge,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedEndpointBusy,
                        .surface = MetricsSurface::LiveStats },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedUnauthenticated,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedMalformed,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::FleetTextRequestsRefusedEndpointBusy,
                        .surface = MetricsSurface::NodeFrameEndpoint },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedNoHandshake,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedHandshakeTimeout,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedProof,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedWrongTarget,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedOwnId,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedTag,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerFramesRefusedSender,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedFull,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedTimeout,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedNoChallenge,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorProof,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedWrongTarget,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnId,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsEndedByAcceptor,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::ClusterAdmissionsRefusedMalformedKey,
                        .surface = MetricsSurface::CompileScheduler },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedUnknownKey,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsRefusedRevokedKey,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerConnectionsEndedKeyWithdrawn,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorKeyUnknown,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedAcceptorKeyRevoked,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsRefusedOwnKeyRevoked,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::RaftPeerDialsEndedKeyWithdrawn,
                        .surface = MetricsSurface::ConsensusPeerWire },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseNoRoster,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseRosterExpired,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerJobsRefusedLeaseSignerRevoked,
                        .surface = MetricsSurface::CompileWorker },
    // The roster trust exists only on a node that runs a worker and no consensus, and nothing
    // else offers it a roster -- so the worker's surface, which `--slots=0` removes.
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerRostersRefusedUncertified,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::WorkerRostersRefusedExpired,
                        .surface = MetricsSurface::CompileWorker },
    CounterSoleWriter { .counter = IMetricsSink::Counter::SchedulerRosterEndorsementsRefused,
                        .surface = MetricsSurface::CompileScheduler },
};

/// Whether every catalogue row has at least one surface attributed to it.
///
/// The guard #1501 exists to install. Before it, a row absent from `CounterSoleWriterTable` and
/// a row nobody had considered were spelled identically -- and 145 of the 148 rows the catalogue
/// held then were in that state, so the silence read as coverage exactly the way the rulebook
/// says it does. A measurement of that moment, so it does not move with the catalogue.
///
/// A `consteval` fold rather than a size comparison, because `CounterSoleWriterTable.size()`
/// counts (counter, surface) PAIRS: it is 166 for 165 counters today, and a row duplicated
/// while another went missing would leave any arithmetic on the size perfectly consistent.
///
/// @return True when no enumerator is missing from the table.
[[nodiscard]] consteval bool EveryCounterIsAttributed() noexcept
{
    for (auto const counter: Enumerators<IMetricsSink::Counter>())
    {
        auto found = false;
        for (auto const& row: CounterSoleWriterTable)
            found = found || row.counter == counter;
        if (!found)
            return false;
    }
    return true;
}

static_assert(EveryCounterIsAttributed(),
              "every MetricsCatalog row needs at least one CounterSoleWriterTable row naming a "
              "surface that writes it: a row with none is indistinguishable from one nobody has "
              "considered, which is the silence #1501 was filed to remove");

/// How many distinct catalogue rows the attribution covers.
///
/// **Not `CounterSoleWriterTable.size()`, which is what a reader reaches for and is wrong.** The
/// table holds one row per (counter, surface) PAIR, so its size exceeds the number of counters by
/// however many are written from more than one component -- one today. A process serving no
/// attributed surface reports THIS many rows absent, and `fastcached_metrics_surface_absent` was
/// asserted against the table size while the two happened to be equal.
///
/// @return The number of counters with at least one attributed surface.
[[nodiscard]] constexpr std::size_t AttributedCounterCount() noexcept
{
    auto count = std::size_t { 0 };
    for (auto const counter: Enumerators<IMetricsSink::Counter>())
        for (auto const& row: CounterSoleWriterTable)
            if (row.counter == counter)
            {
                ++count;
                break;
            }
    return count;
}

/// Whether a process serving @p surfaces could ever write @p counter.
///
/// True for every row this table says nothing about, which is the fail-as-today direction
/// argued for at `CounterSoleWriterTable`. The table is complete today, so that arm is
/// unreachable from this build -- it stays because the fallback is what makes ADDING a counter
/// safe: a new enumerator reports as it always did until somebody attributes it, rather than
/// vanishing from every scrape. `EveryCounterIsAttributed` is what stops that being permanent.
/// @param counter The row.
/// @param surfaces The surfaces this process serves.
/// @return True when some writer of the row could run in this process.
[[nodiscard]] constexpr bool CounterHasAWriterIn(IMetricsSink::Counter counter,
                                                 std::span<MetricsSurface const> surfaces) noexcept
{
    auto attributed = false;
    for (auto const& row: CounterSoleWriterTable)
    {
        if (row.counter != counter)
            continue;
        attributed = true;
        if (std::ranges::find(surfaces, row.surface) != surfaces.end())
            return true;
    }
    return !attributed;
}

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
    /// **Absent rather than zero, and the absence says WHY** -- see `CounterAbsence`. A build
    /// whose sink has no slot for the row cannot state the figure at all (#1353); a process
    /// serving no surface that writes the row has nothing to state (#1484). Both render `-` at a
    /// panel, and only the first is a SKEW -- which is the distinction the bare
    /// `std::optional<std::uint64_t>` this field used to hold could not carry, and why a compile
    /// node scraped as a build whose catalogue and sink disagree.
    CounterCells<CounterReading> counters {};

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
/// The one place the counters are read, so both questions about a row -- does this sink carry it
/// (#1353), and could this process ever write it (#1484) -- are asked once, here, rather than by
/// every encoding.
///
/// @p surfaces is REQUIRED rather than defaulted, unlike on the `RenderPrometheus` convenience
/// overload: there are few callers, each is a real process, and each should state its claim
/// where a reader can see it. An EMPTY span is a legitimate answer -- it is a compile node's,
/// which serves none of the surfaces any row is attributed to.
/// @param metrics The counter sink.
/// @param snapshot What the provider stated for this call.
/// @param surfaces The surfaces this process serves. `EverySurface` for a process that has not
///                 been narrowed.
/// @return The reading.
[[nodiscard]] StatsReading CaptureStatsReading(IMetricsSink const& metrics,
                                               MetricsSnapshot const& snapshot,
                                               std::span<MetricsSurface const> surfaces);

/// Every catalogue row this build's sink has no slot for, by the series name it would render as.
///
/// **The scrape's own question, asked through the scrape's own capture** (#1364), so the startup
/// report of a counter-table skew and the `# SKEW` lines `RenderPrometheus` writes cannot disagree
/// about what a skew is. That is #1362's constraint: derived from the COUNTED set, never from the
/// exported one. Every surface is claimed, because a row this process has no writer for is an
/// ordinary absence and never a skew -- the distinction `CounterAbsence` exists to keep.
/// @param metrics The counter sink.
/// @return The rows' series names, in catalogue order; empty on a consistent build.
[[nodiscard]] std::vector<std::string_view> CatalogueRowsWithoutASlot(IMetricsSink const& metrics);

} // namespace FastCache

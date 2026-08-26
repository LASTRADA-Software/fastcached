// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Distributed
{

/// One lease outcome as a report names it.
///
/// The counter is the one `SchedulerService` already increments, rather than a
/// number this layer computes: `/metrics` stays the source of truth for anything
/// alertable, and a dashboard that recomputed a figure would be a second place it
/// could be wrong.
struct LeaseOutcomeRow
{
    IMetricsSink::Counter counter; ///< Where the number comes from.
    std::string_view key;          ///< Machine-readable name, for JSON.
    std::string_view label;        ///< What a person reads.
    std::string_view meaning;      ///< What it tells an operator to do.
};

/// Every lease outcome, granted first and then each refusal.
///
/// **The split is the point, and summing it is the mistake.** An empty fleet is a
/// misconfiguration, a busy one is under-capacity, a withdrawn one is somebody
/// using their own machine, and a duplicate is neither -- four different operator
/// problems with four different fixes, all of which vanish into one number the
/// moment they are added together.
///
/// Not an `EnumTable` keyed on `PickError`, deliberately: two of these rows are not
/// `PickError`s at all. A grant is not a refusal, and a duplicate is refused
/// *before* a worker is ever picked -- so a table indexed by that enum would have
/// to leave them out, and they are half of what an operator reads.
inline constexpr std::array<LeaseOutcomeRow, 5> LeaseOutcomeTable {
    LeaseOutcomeRow { .counter = IMetricsSink::Counter::DispatchLeasesGranted,
                      .key = "granted",
                      .label = "granted",
                      .meaning = "compiles this fleet handed to a worker." },
    LeaseOutcomeRow { .counter = IMetricsSink::Counter::DispatchLeasesNoWorker,
                      .key = "no-worker",
                      .label = "no worker",
                      .meaning = "nothing is registered for that toolchain: a configuration mistake." },
    LeaseOutcomeRow { .counter = IMetricsSink::Counter::DispatchLeasesNoCapacity,
                      .key = "no-capacity",
                      .label = "no capacity",
                      .meaning = "every matching worker is full of this fleet's own work: buy more machines." },
    LeaseOutcomeRow { .counter = IMetricsSink::Counter::DispatchLeasesWithdrawn,
                      .key = "withdrawn",
                      .label = "withdrawn",
                      .meaning = "the machines are busy with something else, or out of scratch space." },
    LeaseOutcomeRow { .counter = IMetricsSink::Counter::DispatchLeasesDuplicate,
                      .key = "duplicate",
                      .label = "duplicate",
                      .meaning = "that object is already being built somewhere: not a shortage." },
};

/// Everything a fleet report shows, gathered once.
///
/// **Pure data.** Collecting it reads the scheduler and the cluster; rendering it
/// reads only this. That split is what lets every rendering rule below be a unit
/// test over a literal rather than a fleet, a socket and a sleep.
struct FleetSnapshot
{
    /// What the node that produced this is.
    SchedulerRole role { SchedulerRole::Undecided };
    /// Where the leader answers, when one is known. Empty while undecided.
    std::string leaderEndpoint;
    /// What the cluster has agreed, or absent when this node runs no cluster.
    ///
    /// Absent and empty are different claims: a node started without `--node-id`
    /// leads itself and has no replicated state for anybody to read, which is not
    /// the same as a cluster that has agreed on nobody.
    std::optional<Cluster::ClusterState> cluster;
    /// Every live machine, already deduped per endpoint.
    std::vector<NodeReport> nodes;
    /// Every live registry entry, one per (toolchain, endpoint).
    std::vector<WorkerReport> workers;
    /// One count per `LeaseOutcomeTable` row, in its order.
    std::vector<std::uint64_t> leases;
    /// Leases outstanding right now.
    std::size_t liveLeases { 0 };
    /// Worker registrations accepted since this leader started counting.
    std::uint64_t registrations { 0 };
    /// Which cache tiers any member reports.
    ///
    /// A table cannot omit one cell the way a scrape omits a line, so this is how
    /// "absent is not zero" is honoured at column granularity: a tier no member
    /// runs contributes **no column at all**, and a member lacking a tier every
    /// other member has renders an absence in it.
    EnumTable<StorageTier, bool> tiersPresent {};
};

/// What a fleet report is collected from.
///
/// Pointers rather than references because two of them are legitimately absent: a
/// node with no `--listen-scheduler` runs no scheduler, and one with no `--node-id`
/// runs no cluster. The bundle then has an obvious "nothing configured" spelling
/// for a test -- the same device `NodeScrapeSources` uses for a node with no cache.
struct FleetSources
{
    /// The scheduler whose registry and role are reported. Never null.
    SchedulerService const* scheduler {};
    /// The cluster, or **null** when this node runs none.
    IClusterAdmin const* cluster {};
    /// Where the lease counters are read. Never null.
    IMetricsSink const* metrics {};
};

/// Gather everything a fleet report shows.
///
/// **I/O-free**, exactly as `SchedulerService` is: it reads memory and returns a
/// value. Cache figures come from `NodeReports()` rather than being summed across
/// workers, because a node serving two toolchains is two registry entries carrying
/// one machine's numbers.
/// @param sources What to read.
/// @return The snapshot.
/// The fleet's capacity, split the way an operator has to act on it.
///
/// Three numbers rather than one, because they have three different fixes. Slots
/// this fleet is using say buy more machines; slots a ceiling withdrew say the
/// machines are busy with somebody else's work or out of scratch, and buying more
/// would not have helped. Collapsing them into "utilisation" is what hides which
/// of those a fleet is suffering from -- the same reason `LeaseOutcomeTable` keeps
/// its five rows apart.
struct FleetTotals
{
    std::uint32_t registered { 0 }; ///< Slots the machines registered with.
    std::uint32_t inFlight { 0 };   ///< This fleet's compiles running right now.
    std::uint32_t free { 0 };       ///< Slots a compile could start on right now.
    std::uint32_t withheld { 0 };   ///< Registered, but behind a ceiling: not ours to use.
};

/// Sum the capacity split across machines.
///
/// Over `NodeReports()` and never over worker entries: a machine serving two
/// toolchains is two registry entries carrying one machine's cores, so summing
/// there reports a fleet twice the size of the one you own.
/// @param snapshot The gathered fleet.
/// @return The split, saturating rather than wrapping if a ceiling exceeds what was registered.
[[nodiscard]] FleetTotals TotalsFor(FleetSnapshot const& snapshot) noexcept;

[[nodiscard]] FleetSnapshot CollectFleet(FleetSources const& sources);

/// Render a fleet snapshot as JSON.
///
/// The endpoint that makes the page replaceable and testable without a browser,
/// and the reason the column tables carry a machine-readable key beside a label.
/// A value nobody reported is `null`, never `0`.
/// @param snapshot What to render.
/// @return A JSON document.
[[nodiscard]] std::string RenderFleetJson(FleetSnapshot const& snapshot);

/// What the page draws over time, gathered before rendering.
///
/// Pure data, like `FleetSnapshot` and for the same reason: every rendering rule
/// below is then a unit test over a literal rather than over a sampler, a timer and
/// a sleep.
struct FleetHistoryView
{
    /// Which range the reader asked for.
    FleetRange range { FleetRange::Day };
    /// That range's buckets, oldest first, gaps included.
    ///
    /// Empty when this node keeps no history at all -- a node that has just started
    /// leading, or one whose `Load` found nothing. The page says which, because a
    /// fleet that did nothing and a fleet nobody was watching are different facts.
    std::vector<FleetBucket> buckets {};
    /// Whether the history survives a restart, for the note under the charts.
    ///
    /// A node with neither `--cluster-dir` nor `--cache-dir` has nowhere to put the
    /// file and keeps its history in memory only. That is a legitimate way to run,
    /// and it is not the same promise as a durable one -- so the page says so
    /// rather than letting an operator find out at the next restart.
    bool durable { false };
};

/// Render a fleet snapshot as one self-contained HTML page.
///
/// No script, no external asset, no bundled framework: the stylesheet is embedded
/// and every value is escaped. A value nobody reported renders as a dash, which is
/// the spelling `--cluster-status` already uses for "has not said".
///
/// The charts are the one thing this page does **not** inline: each is referenced
/// at its own URL so a browser can revalidate it with `If-None-Match` and be told
/// `304` for as long as the bucket it drew is still open.
/// @param snapshot What to render.
/// @param history What to draw over time.
/// @param refreshSeconds How often the page reloads itself; 0 to not.
/// @return A complete HTML document.
[[nodiscard]] std::string RenderFleetHtml(FleetSnapshot const& snapshot,
                                          FleetHistoryView const& history,
                                          unsigned refreshSeconds);

/// Where a chart's SVG is served, relative to the admin root.
inline constexpr std::string_view FleetChartPrefix = "/fleet/chart/";

/// Where the whole series set is served as JSON.
inline constexpr std::string_view FleetSeriesPath = "/fleet/series.json";

/// Whether this snapshot can answer for the whole fleet.
///
/// Only a leader can: a follower's registry holds whatever registered against it
/// rather than the fleet, which is the same reason `SchedulerService::Gate()`
/// refuses every verb -- reads included -- from a node that does not lead.
/// @param snapshot The snapshot.
/// @return True when this node leads.
[[nodiscard]] bool LeadsTheFleet(FleetSnapshot const& snapshot) noexcept;

} // namespace FastCache::Distributed

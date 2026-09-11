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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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
///
/// The tile renders each row's counter **series name**, so an operator can move from
/// the page to `/metrics` or `node-metrics` without guessing it -- derived through
/// `DescriptorOf` rather than restated as a sixth column, since a string beside the
/// typed `counter` would be a second source of truth for one fact, free to drift from
/// the catalogue while both claimed to be current.
///
/// **`DescriptorOf` is total here, and no assertion is added saying so.** The first
/// version of this carried a `static_assert` that every row resolves; it could not
/// fire. `CounterTable` is an `EnumTable` over `IMetricsSink::Counter` already
/// guarded by `RowsInEnumeratorOrder`, so it has a row per enumerator and
/// `DescriptorOf` answers null only past `Last` -- which no `LeaseOutcomeRow::counter`
/// can hold. A counter added without a catalogue row fails at `CounterTable`'s own
/// assertion first, so the second one was the shape AGENT.md names: a guard that
/// fires only when nothing is wrong. One guard, cited, rather than two.
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

/// One outstanding lease, joined to where its holder answers.
///
/// `LeaseReport` carries the worker's **id**, because that is what the lease table
/// knows; an operator needs the address to go and look at. The join belongs here
/// rather than in `LeaseTable`, which must not learn about the registry -- the two
/// are siblings under `SchedulerService`, and a report is the one place they
/// legitimately meet.
struct LeaseHolding
{
    std::string key;      ///< The object key being compiled.
    std::string workerId; ///< The worker it was leased to.
    /// host:port that worker answers on, or **empty** when it is no longer
    /// registered -- which is itself the diagnosis rather than a gap in the row.
    std::string workerEndpoint;
    std::chrono::milliseconds age {}; ///< Since the lease was taken.
};

/// How many outstanding leases a report lists.
///
/// A bound rather than the whole set, because a fleet at full tilt holds thousands
/// and a page that renders all of them is one nobody can read -- and a JSON
/// document that grows without limit. The total travels beside it, so the
/// truncation is visible rather than silent.
inline constexpr std::size_t OutstandingLeaseRows = 50;

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
    /// The oldest of those, at most `OutstandingLeaseRows` of them.
    ///
    /// Beside `liveLeases` rather than replacing it: the count is the total and
    /// this is a window onto it, and a reader that could not tell them apart would
    /// read a truncated list as the whole fleet's work.
    std::vector<LeaseHolding> outstandingLeases;
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
/// node with no `--serve-scheduler` runs no scheduler, and one with no `--node-id`
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

/// How many of the fleet's toolchains the scheduler has never chosen.
///
/// The fleet-wide complement of `WorkerReport::lastPickedAge`, and the cheaper of
/// the two to read: a per-worker column has to be scanned, and this is one number
/// that is zero on a fleet where every toolchain is being used
/// ([#1297](https://github.com/LASTRADA-Software/fastcached/issues/1297)).
///
/// The grain is the **fingerprint**, not the registry entry and not the machine. A
/// toolchain served by two machines where only one is ever chosen is being reached,
/// so it is not what this counts; the failure it exists to find is a fingerprint
/// *nothing at all* is sent to, which is what a driver family that fingerprints
/// differently from the clients' looks like from the leader (#226). Counting over
/// worker entries rather than `NodeReports()` is right here and is not the
/// double-counting hazard that rule guards against: a fingerprint is a per-entry
/// fact deduped into a set, where the fields that rule is about describe a machine
/// and would be added once per toolchain it serves.
struct ToolchainPickCoverage
{
    /// Distinct fingerprints registered right now.
    std::size_t toolchains { 0 };
    /// How many of those nothing has ever been sent to. Never more than `toolchains`.
    std::size_t neverPicked { 0 };
};

/// Count the fleet's toolchains and how many of them are never chosen.
///
/// **Absent when no worker is registered at all**, and that is the same rule as
/// every cell on this page rather than a special case: a fleet with nothing in it
/// would otherwise report `0 never picked`, which is the healthiest possible
/// reading printed for the least healthy possible fleet. Zero has to keep meaning
/// *every toolchain is being reached*, so a fleet with no toolchains says nothing.
///
/// It answers *never*, not *not lately*, and the difference decides whether the
/// number is worth reading. A windowed count is non-zero on every idle fleet --
/// nothing is picked at three in the morning either -- so it could not carry the
/// one claim this exists to support, that a non-zero reading on a fleet that is
/// building means something is wrong.
///
/// One state reads non-zero legitimately and drains on its own, because the record
/// is **this leader's**: a scheduler that has just been elected has chosen nobody
/// yet, however long the fleet has been up, and so has a node whose entry lapsed and
/// re-entered the registry. It clears as soon as one job goes to each toolchain, so
/// it is a reading that PERSISTS which is the finding -- pair it with `registeredAge`
/// on the worker rows, which is what says whether there has been time for a pick.
///
/// An ordinary re-registration is deliberately NOT such a state, and that took a
/// fix: a node falls through to `Register` after any refused heartbeat, so clearing
/// the record there would have spiked this figure on a healthy building fleet every
/// time an election or a busy endpoint refused one round. See `WorkerRegistry`.
/// @param snapshot The gathered fleet.
/// @return The split, or absent when the fleet holds no registered worker.
[[nodiscard]] std::optional<ToolchainPickCoverage> PickCoverageFor(FleetSnapshot const& snapshot);

[[nodiscard]] FleetSnapshot CollectFleet(FleetSources const& sources);

/// Defined below, beside the chart it belongs to. Declared here because the two
/// machine-readable renderers take one and sit above it: a reference parameter needs no
/// complete type, and moving the struct up would put the history's whole vocabulary in
/// front of the page's.
struct FleetHistoryView;

/// Render a fleet snapshot as JSON.
///
/// The endpoint that makes the page replaceable and testable without a browser,
/// and the reason the column tables carry a machine-readable key beside a label.
/// A value nobody reported is `null`, never `0`.
///
/// **Takes the history because the headline figures do**
/// ([#1302](https://github.com/LASTRADA-Software/fastcached/issues/1302)). Three of the
/// seven read a `FleetHistoryView` rather than the snapshot, so a renderer without one
/// could carry four of them and would have to call the other three absent — which is a
/// reading, not an absence. The window is the CALLER's: the page takes a `range` and
/// this must take the same one, or the same fleet reports two different Dispatched
/// figures depending on which surface asked.
/// @param snapshot What to render.
/// @param history The window the headline figures answer for.
/// @return A JSON document.
[[nodiscard]] std::string RenderFleetJson(FleetSnapshot const& snapshot, FleetHistoryView const& history);

/// One table of the fleet report, as a terminal reader asks for it.
///
/// The page's sections, named so a reader can take one
/// ([#1300](https://github.com/LASTRADA-Software/fastcached/issues/1300)). The
/// selection happens HERE, beside the tables, rather than in whatever client is
/// reading: a client that filtered would have to know which columns belong to which
/// section, which is the second place that mapping could be wrong.
///
/// Persisted and transmitted nowhere: this is a query parameter's vocabulary and a
/// document's markers, so the enumerator VALUES bind nothing. The explicit `= 0` is
/// `EnumTable`'s anchor and not a wire contract.
enum class FleetSection : std::uint8_t
{
    Kpi = 0,  ///< The headline figures the page's strip carries.
    Machines, ///< One row per machine — the grain a fleet total is computed over.
    Workers,  ///< One row per `(toolchain, endpoint)` registry entry.
    Leases,   ///< The oldest outstanding leases, bounded as the page bounds them.
    Members,  ///< What the cluster has agreed, when this node runs one.
    Tiers,    ///< Per-tier cache figures, for the tiers some member runs.
    Last,     ///< Not a section, and has no row: the length of a table keyed by one.
};

/// What one section is called.
struct FleetSectionRow
{
    FleetSection section;     ///< The section this row describes.
    std::string_view key;     ///< What a reader asks for, and the marker in the full document.
    std::string_view summary; ///< One line, for whoever guessed the key wrong.

    /// Whether this section is a row TABLE on all three surfaces.
    ///
    /// True for every section whose shape is `FleetColumn` rows crossed with subjects:
    /// a header row of column names, then one line per subject, rendered the same way
    /// as a `<table>`, a JSON array of objects and a tab-separated block.
    ///
    /// **False for `Kpi`, and that is a property of the section rather than an
    /// exemption.** The headline figures are one value per named figure, not many rows
    /// of one shape: the page draws them as a STRIP with no `<th>` anywhere, and the
    /// JSON keys them by name rather than listing them. So `FleetColumnNames` has no
    /// answer for it — and this column is what lets the coverage test say that in both
    /// directions instead of carrying a silent exception, which is the shape
    /// [#1320](https://github.com/LASTRADA-Software/fastcached/issues/1320) is about.
    bool tabular;
};

/// Every section `/fleet.txt` serves, in the order the page presents them.
///
/// A table rather than a `switch`, for the reason every other table here is one: the
/// next section is a row, and the refusal that lists the valid keys walks this rather
/// than restating them — a hand-listed refusal goes stale in the one place a reader
/// who just guessed wrong is looking.
inline constexpr EnumTable<FleetSection, FleetSectionRow> FleetSectionTable {
    FleetSectionRow { .section = FleetSection::Kpi,
                      .key = "kpi",
                      .summary = "the headline figures, one row each; the page's strip",
                      .tabular = false },
    FleetSectionRow { .section = FleetSection::Machines,
                      .key = "machines",
                      .summary = "one row per machine; the grain a fleet total is computed over",
                      .tabular = true },
    FleetSectionRow { .section = FleetSection::Workers,
                      .key = "workers",
                      .summary = "one row per (toolchain, endpoint) registry entry",
                      .tabular = true },
    FleetSectionRow { .section = FleetSection::Leases,
                      .key = "leases",
                      .summary = "the oldest outstanding leases, bounded as the page bounds them",
                      .tabular = true },
    FleetSectionRow { .section = FleetSection::Members,
                      .key = "members",
                      .summary = "what the cluster has agreed; absent when this node runs none",
                      .tabular = true },
    FleetSectionRow { .section = FleetSection::Tiers,
                      .key = "tiers",
                      .summary = "per-tier cache figures, for the tiers some member runs",
                      .tabular = true },
};
static_assert(RowsInEnumeratorOrder(FleetSectionTable, &FleetSectionRow::section));

/// Which section a reader named.
/// @param key The `section` value, as typed.
/// @return The section, or absent when nothing is called that.
[[nodiscard]] std::optional<FleetSection> FleetSectionFromKey(std::string_view key) noexcept;

/// Every column name @p section renders for @p snapshot, in the order all three
/// surfaces walk them.
///
/// Here so the coverage test can be **derived** rather than written out. It used to
/// iterate four braced lists of column-name literals under a comment saying it walked
/// the tables, which is [#492](https://github.com/LASTRADA-Software/fastcached/issues/492)'s
/// rule one axis over: a column added to a table joined the test's blind spot silently,
/// the case still passed, and the prose retired the suspicion that would have found the
/// gap. Two node columns and the whole tier section were uncovered that way.
///
/// **Only the NAMES come out; the tables stay file-local.** A `FleetColumn` row carries
/// a projector over the row type it reads, so publishing the rows would put
/// `NodeReport`, `WorkerReport`, `LeaseHolding` and `ClusterMember` projections in this
/// header for no production reader. The column SET is the contract between the page, the
/// JSON and the text — the projectors are not.
///
/// **It takes the snapshot, because one section's column set is not static.** The tier
/// columns are `StorageTierTable` crossed with the per-tier suffixes and *a tier no
/// member runs gets no column*, so a fixed list for `Tiers` would name columns the
/// document does not carry. Composed through the same `TierColumnName` the three
/// renderers use, so there is no second spelling to disagree with.
///
/// A `switch` with no default arm, exactly as `AppendSectionText` has none: a section
/// added to `FleetSectionTable` is a BUILD failure here rather than one that quietly
/// reports no columns — which reads identically to a section nothing needs to check.
///
/// It answers for EVERY section, the non-tabular ones included: `Kpi` is one value per
/// named figure rather than rows of one shape, but its text form is the key/value table
/// that shape implies and those four column names are as real as any other's. What
/// `FleetSectionRow::tabular` decides is whether those same names also appear as page
/// headers and JSON keys — for `Kpi` they do not, because the page draws a strip and
/// the JSON keys by figure. A caller asserting coverage reads that column rather than
/// carrying a silent exception for one section.
///
/// @param section Which section.
/// @param snapshot The document the names are being asked about.
/// @return The names, in render order; empty only for `FleetSection::Last`.
[[nodiscard]] std::vector<std::string> FleetColumnNames(FleetSection section, FleetSnapshot const& snapshot);

/// Every headline figure's machine key, in the order the strip presents them.
///
/// The same door `FleetColumnNames` is, for the same reason: the strip's table is
/// file-local to `FleetView.cpp` because its rows carry projections, and a coverage
/// test that cannot name the table writes the list out instead — which is the defect
/// [#1320](https://github.com/LASTRADA-Software/fastcached/issues/1320) records.
///
/// KEYS rather than labels. A label is what the page calls a tile and is free to be
/// reworded; the key is what a scraper keyed on `/fleet.json` depends on.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<std::string_view const> FleetKpiKeys() noexcept;

/// Render a fleet snapshot as tab-separated text.
///
/// The **third** walk over the same `FleetColumn` tables the page and the JSON walk,
/// which is what makes this parity rather than a second report: there is no second
/// table to disagree with, so a column added once appears in all three and a
/// `project()` changed once changes all three. A terminal reader had no door to these
/// tables at all — `/fleet.json` needs a JSON parser the operator supplies, and `jq`
/// is not on a Windows build box (#1300).
///
/// Numbers are RAW, as the JSON carries them rather than as the page renders them:
/// `13314398617` and not `12.4 GiB`. This output is for `awk` and `cut`, and a
/// humanised figure would have to be parsed back before it could be compared.
///
/// Absent is `-`, which is the spelling `fastcache-cli`'s own human format already
/// uses, and never an empty field: a blank cell is indistinguishable from a value
/// that is genuinely the empty string, and *absent is not zero* loses its meaning the
/// moment the two render alike.
///
/// **Every field is escaped.** A fingerprint, a version and a display name are text a
/// PEER chose, and a tab is perfectly valid UTF-8 — so one worker registering with a
/// tab in its name would shift every later column of that row, and a newline would
/// invent a row, for whoever is reading. That is the same shape as one bad byte making
/// `/fleet.json` unparseable for the whole fleet, and the answer is the one the
/// encoders already owe their formats: this is what escaping a quote is to JSON.
/// @param snapshot What to render.
/// @param section Which table, or absent for every section with its marker line.
/// @return The document, each line ending in `\n`.
/// @param history The window the `kpi` section's figures answer for; see `RenderFleetJson`.
[[nodiscard]] std::string RenderFleetText(FleetSnapshot const& snapshot,
                                          FleetHistoryView const& history,
                                          std::optional<FleetSection> section);

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

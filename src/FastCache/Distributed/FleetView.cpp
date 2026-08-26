// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Distributed
{

namespace
{

    /// One cell: a number, some text, or nothing said.
    ///
    /// `Absent` is the default state and it is spelled as a state rather than as an
    /// empty string or a zero, because **absent is not zero** and the two lead to
    /// opposite conclusions: `0 items` says a cache is standing empty, and a node
    /// that never reported one has no cache to be empty. Every `optional` in
    /// `NodeLoad`, `NodeCacheLoad` and `NodeCacheCapacity` reaches a report through
    /// this, unflattened.
    ///
    /// A number stays a number rather than becoming its formatted text, because the
    /// two consumers want it differently: a page wants `12.4 GiB` and a JSON
    /// document wants `13314398617`, and one that quoted its numbers would make
    /// every reader parse them twice.
    struct FleetCell
    {
        enum class Kind : std::uint8_t
        {
            Absent = 0,
            Number,
            Text,
        };

        Kind kind { Kind::Absent };
        std::uint64_t number { 0 };
        std::string text {};

        [[nodiscard]] static FleetCell Nothing() noexcept
        {
            return FleetCell {};
        }
        [[nodiscard]] static FleetCell Of(std::uint64_t value) noexcept
        {
            return FleetCell { .kind = Kind::Number, .number = value, .text = {} };
        }
        [[nodiscard]] static FleetCell Of(std::string value)
        {
            return FleetCell { .kind = Kind::Text, .number = 0, .text = std::move(value) };
        }
        /// An optional's value, or the absence it already carries.
        template <typename T>
        [[nodiscard]] static FleetCell Maybe(std::optional<T> const& value)
        {
            return value.has_value() ? Of(static_cast<std::uint64_t>(*value)) : Nothing();
        }
    };

    /// How a page renders a number. The JSON ignores it and writes the integer.
    enum class CellFormat : std::uint8_t
    {
        Count = 0,
        Bytes,
        Permille,
        Millis,
        Text,
    };

    /// One column: what it is called, and how to read it off one subject.
    ///
    /// A **projection** rather than a value, which is the shape `TierMetric`
    /// already uses against `StorageTierTable`: one row renders once per subject,
    /// and the subjects are not known where the table is written. A hand-written
    /// list of `<td>`s is the defect this repository has already recorded once, as
    /// a series an operator was told to scrape that was never exported.
    template <typename Subject>
    struct FleetColumn
    {
        std::string_view name;   ///< JSON key AND header cell: one spelling, so the two cannot disagree.
        std::string_view help;   ///< The header's tooltip; a reader's, so not in the JSON.
        CellFormat format;       ///< How the page renders it.
        FleetCell (*project)(Subject const&); ///< What to read.
    };

    // ---------------------------------------------------------------- helpers

    [[nodiscard]] std::string EscapeHtml(std::string_view text)
    {
        // Every value on this page came off a wire: a toolchain fingerprint and an
        // endpoint are whatever a peer sent. `Stats.cpp` has the sibling of this
        // function in its own anonymous namespace; two small copies are the
        // accepted shape here rather than a shared header, for the reason the
        // launcher does not link this library at all.
        std::string out;
        out.reserve(text.size());
        for (auto const ch: text)
        {
            switch (ch)
            {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                default: out += ch; break;
            }
        }
        return out;
    }

    void AppendJsonString(std::string& out, std::string_view text)
    {
        out += '"';
        for (auto const ch: text)
        {
            switch (ch)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                    else
                        out += ch;
                    break;
            }
        }
        out += '"';
    }

    /// Render a byte count the way an operator reads one.
    [[nodiscard]] std::string HumanBytes(std::uint64_t bytes)
    {
        constexpr std::array<std::string_view, 5> Units { "B", "KiB", "MiB", "GiB", "TiB" };
        auto scaled = static_cast<double>(bytes);
        std::size_t unit = 0;
        while (scaled >= 1024.0 && unit + 1 < Units.size())
        {
            scaled /= 1024.0;
            ++unit;
        }
        return unit == 0 ? std::format("{} {}", bytes, Units[unit]) : std::format("{:.1f} {}", scaled, Units[unit]);
    }

    /// Render an age the way an operator reads one.
    [[nodiscard]] std::string HumanMillis(std::uint64_t millis)
    {
        if (millis < 1000)
            return std::format("{} ms", millis);
        if (millis < 60'000)
            return std::format("{:.1f} s", static_cast<double>(millis) / 1000.0);
        return std::format("{:.1f} min", static_cast<double>(millis) / 60'000.0);
    }

    /// One cell, as the page shows it.
    ///
    /// The dash is the spelling `--cluster-status` already uses for "has not said",
    /// kept the same so an operator reading both sees one vocabulary.
    [[nodiscard]] std::string CellAsText(FleetCell const& cell, CellFormat format)
    {
        constexpr std::string_view Absent = "&ndash;";
        if (cell.kind == FleetCell::Kind::Absent)
            return std::string { Absent };
        if (cell.kind == FleetCell::Kind::Text)
            return EscapeHtml(cell.text);
        switch (format)
        {
            case CellFormat::Bytes: return HumanBytes(cell.number);
            case CellFormat::Permille: return std::format("{:.1f}%", static_cast<double>(cell.number) / 10.0);
            case CellFormat::Millis: return HumanMillis(cell.number);
            case CellFormat::Count:
            case CellFormat::Text: break;
        }
        return std::format("{}", cell.number);
    }

    /// One cell, as the JSON carries it. Absent is `null`, never `0`.
    void AppendCellAsJson(std::string& out, FleetCell const& cell)
    {
        switch (cell.kind)
        {
            case FleetCell::Kind::Absent: out += "null"; break;
            case FleetCell::Kind::Number: out += std::format("{}", cell.number); break;
            case FleetCell::Kind::Text: AppendJsonString(out, cell.text); break;
        }
    }

    /// The hit rate a node's cache is serving, or nothing when it served no reads.
    ///
    /// Absent rather than `0%` when nothing was asked of it, one level in from
    /// `NodeCacheLoad::hits` being optional: a cache nobody has read yet has no
    /// hit rate, which is a different claim from one that misses everything.
    [[nodiscard]] FleetCell HitRateOf(NodeCacheLoad const& cache)
    {
        if (!cache.hits.has_value() || !cache.misses.has_value())
            return FleetCell::Nothing();
        auto const total = *cache.hits + *cache.misses;
        if (total == 0)
            return FleetCell::Nothing();
        return FleetCell::Of(static_cast<std::uint64_t>((*cache.hits * 1000) / total));
    }

    // ------------------------------------------------------------ the tables

    /// What a member row shows.
    constexpr std::array<FleetColumn<Cluster::ClusterMember>, 3> MemberColumns {
        FleetColumn<Cluster::ClusterMember> {
            .name = "id",
            .help = "The member's stable identity: what consensus counts.",
            .format = CellFormat::Text,
            .project = [](Cluster::ClusterMember const& m) { return FleetCell::Of(m.id); } },
        FleetColumn<Cluster::ClusterMember> {
            .name = "raft-endpoint",
            .help = "Where its consensus port answers. Always present.",
            .format = CellFormat::Text,
            .project = [](Cluster::ClusterMember const& m) { return FleetCell::Of(m.raftEndpoint); } },
        FleetColumn<Cluster::ClusterMember> {
            .name = "scheduler-endpoint",
            .help = "Where clients reach the fleet while this member leads. Absent until it has led.",
            .format = CellFormat::Text,
            // Absent, not an empty string presented as a fact: a member that has
            // never led has not said, which is the normal state rather than a
            // fault.
            .project = [](Cluster::ClusterMember const& m) {
                return m.schedulerEndpoint.empty() ? FleetCell::Nothing() : FleetCell::Of(m.schedulerEndpoint);
            } },
    };

    /// What a node row shows, before its per-tier cache columns.
    constexpr std::array<FleetColumn<NodeReport>, 10> NodeColumns {
        FleetColumn<NodeReport> { .name = "endpoint",
                                  .help = "host:port the machine answers on.",
                                  .format = CellFormat::Text,
                                  .project = [](NodeReport const& n) { return FleetCell::Of(n.endpoint); } },
        FleetColumn<NodeReport> {
            .name = "toolchains",
            .help = "How many toolchains this one machine serves. Each is a separate registry entry.",
            .format = CellFormat::Count,
            .project = [](NodeReport const& n) { return FleetCell::Of(n.fingerprints.size()); } },
        FleetColumn<NodeReport> {
            .name = "cores",
            .help = "Hardware threads. Absent when the machine could not read its own.",
            .format = CellFormat::Count,
            // Zero means "did not say" in `NodeCapacity`, and rendering it as 0
            // would claim a machine with no CPU.
            .project = [](NodeReport const& n) {
                return n.capacity.logicalCores == 0 ? FleetCell::Nothing() : FleetCell::Of(n.capacity.logicalCores);
            } },
        FleetColumn<NodeReport> { .name = "memory",
                                  .help = "Physical memory. Absent when the machine did not say.",
                                  .format = CellFormat::Bytes,
                                  .project = [](NodeReport const& n) {
                                      return n.capacity.totalMemoryBytes == 0
                                                 ? FleetCell::Nothing()
                                                 : FleetCell::Of(n.capacity.totalMemoryBytes);
                                  } },
        FleetColumn<NodeReport> {
            .name = "class",
            .help = "How hard this machine may be driven, and how many cores are held back for whoever uses it.",
            .format = CellFormat::Text,
            .project = [](NodeReport const& n) {
                auto const& traits = TraitsFor(n.capacity.nodeClass);
                auto const reserve =
                    n.capacity.reserveIsExplicit ? n.capacity.reservedCores : traits.reservedCores;
                return FleetCell::Of(std::format("{} (reserve {})", traits.name, reserve));
            } },
        FleetColumn<NodeReport> { .name = "cpu-busy",
                                  .help = "Host-wide CPU in use, this fleet's work included. Absent when unread.",
                                  .format = CellFormat::Permille,
                                  .project = [](NodeReport const& n) {
                                      return FleetCell::Maybe(n.load.cpuBusyPermille);
                                  } },
        FleetColumn<NodeReport> { .name = "memory-available",
                                  .help = "Memory a new compile could get. Absent when unread.",
                                  .format = CellFormat::Bytes,
                                  .project = [](NodeReport const& n) {
                                      return FleetCell::Maybe(n.load.availableMemoryBytes);
                                  } },
        FleetColumn<NodeReport> { .name = "scratch-free",
                                  .help = "Room where compiles run. The limit that most often reaches zero.",
                                  .format = CellFormat::Bytes,
                                  .project = [](NodeReport const& n) {
                                      return FleetCell::Maybe(n.load.freeScratchBytes);
                                  } },
        FleetColumn<NodeReport> { .name = "cache-hit-rate",
                                  .help = "Reads this node's cache served. Absent when it has served none.",
                                  .format = CellFormat::Permille,
                                  .project = [](NodeReport const& n) { return HitRateOf(n.load.cache); } },
        FleetColumn<NodeReport> {
            .name = "heartbeat-age",
            .help = "Since this machine last reported. Everything on its row is that old.",
            .format = CellFormat::Millis,
            // The column that tells "this cache is empty" from "this node stopped
            // answering an hour ago and these are its last figures" -- which look
            // identical without it, and lead to opposite conclusions.
            .project =
                [](NodeReport const& n) { return FleetCell::Of(static_cast<std::uint64_t>(n.heartbeatAge.count())); } },
    };

    /// What a worker row shows.
    constexpr std::array<FleetColumn<WorkerReport>, 8> WorkerColumns {
        FleetColumn<WorkerReport> { .name = "id",
                                    .help = "The id this leader assigned at registration.",
                                    .format = CellFormat::Text,
                                    .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.id); } },
        FleetColumn<WorkerReport> {
            .name = "toolchain",
            .help = "Matched byte-for-byte. A job never crosses fingerprints.",
            .format = CellFormat::Text,
            .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.fingerprint); } },
        FleetColumn<WorkerReport> { .name = "endpoint",
                                    .help = "host:port a client is sent to.",
                                    .format = CellFormat::Text,
                                    .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.endpoint); } },
        FleetColumn<WorkerReport> { .name = "slots",
                                    .help = "Concurrent compiles it registered with.",
                                    .format = CellFormat::Count,
                                    .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.slots); } },
        FleetColumn<WorkerReport> { .name = "in-flight",
                                    .help = "This fleet's compiles running on it right now.",
                                    .format = CellFormat::Count,
                                    .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.inFlight); } },
        FleetColumn<WorkerReport> {
            .name = "available",
            .help = "Compiles it may take right now. Below the registered count when something withdrew capacity.",
            .format = CellFormat::Count,
            .project = [](WorkerReport const& w) {
                return FleetCell::Of(AvailableSlots(w.info.capacity, w.info.slots, w.info.load));
            } },
        FleetColumn<WorkerReport> {
            .name = "limited-by",
            .help = "Which ceiling withdrew the difference: the three have opposite fixes.",
            .format = CellFormat::Text,
            .project = [](WorkerReport const& w) {
                auto const ceilings = SlotCeilingsFor(w.info.capacity, w.info.slots, w.info.load);
                return FleetCell::Of(std::string { TraitsFor(ceilings.binding).name });
            } },
        FleetColumn<WorkerReport> { .name = "heartbeat-age",
                                    .help = "Since this entry last reported. A worker unheard-from is dropped.",
                                    .format = CellFormat::Millis,
                                    .project = [](WorkerReport const& w) {
                                        return FleetCell::Of(static_cast<std::uint64_t>(w.heartbeatAge.count()));
                                    } },
    };

    /// The per-tier cache columns, rendered once per tier a member actually runs.
    ///
    /// A projection for the reason `TierMetric` is one: the tiers come from
    /// `StorageTierTable`, never from a list written out here, so a tier added to
    /// the enum reaches a report by being a row.
    struct TierColumn
    {
        std::string_view suffix;
        std::string_view help;
        CellFormat format;
        FleetCell (*project)(NodeReport const&, StorageTier);
    };

    constexpr std::array<TierColumn, 4> TierColumns {
        TierColumn { .suffix = "items",
                     .help = "Entries this tier holds.",
                     .format = CellFormat::Count,
                     .project = [](NodeReport const& n, StorageTier tier) {
                         auto const& usage = n.load.cache.tiers[static_cast<std::size_t>(tier)];
                         return usage.has_value() ? FleetCell::Of(usage->itemCount) : FleetCell::Nothing();
                     } },
        TierColumn { .suffix = "bytes",
                     .help = "Bytes this tier holds.",
                     .format = CellFormat::Bytes,
                     .project = [](NodeReport const& n, StorageTier tier) {
                         auto const& usage = n.load.cache.tiers[static_cast<std::size_t>(tier)];
                         return usage.has_value() ? FleetCell::Of(usage->bytesUsed) : FleetCell::Nothing();
                     } },
        TierColumn { .suffix = "budget",
                     .help = "What this tier may hold. Absent means no such tier; unbounded means no ceiling.",
                     .format = CellFormat::Bytes,
                     // The two claims `NodeCacheCapacity` keeps apart: absent is
                     // "this node runs no tier of that kind", zero is "a tier with
                     // no ceiling". A dashboard that flattened them would render
                     // both as the same thing.
                     .project = [](NodeReport const& n, StorageTier tier) {
                         auto const& limit = n.capacity.cache.tierBytesLimit[static_cast<std::size_t>(tier)];
                         if (!limit.has_value())
                             return FleetCell::Nothing();
                         return *limit == 0 ? FleetCell::Of(std::string { "unbounded" }) : FleetCell::Of(*limit);
                     } },
        TierColumn { .suffix = "evictions",
                     .help = "Entries this tier dropped to stay within its budget.",
                     .format = CellFormat::Count,
                     .project = [](NodeReport const& n, StorageTier tier) {
                         auto const& usage = n.load.cache.tiers[static_cast<std::size_t>(tier)];
                         return usage.has_value() ? FleetCell::Of(usage->evictions) : FleetCell::Nothing();
                     } },
    };

    /// The name a tier column carries, e.g. `memory-items`.
    [[nodiscard]] std::string TierColumnName(StorageTier tier, std::string_view suffix)
    {
        return std::format("{}-{}", StorageTierTable[static_cast<std::size_t>(tier)].name, suffix);
    }

    /// Whether any member reports this tier.
    [[nodiscard]] bool AnyNodeHasTier(std::vector<NodeReport> const& nodes, StorageTier tier)
    {
        auto const index = static_cast<std::size_t>(tier);
        return std::ranges::any_of(nodes, [index](NodeReport const& n) {
            return n.load.cache.tiers[index].has_value() || n.capacity.cache.tierBytesLimit[index].has_value();
        });
    }

    /// How the page and the JSON both name a role.
    [[nodiscard]] std::string_view RoleName(SchedulerRole role) noexcept
    {
        switch (role)
        {
            case SchedulerRole::Leader: return "leader";
            case SchedulerRole::Follower: return "follower";
            case SchedulerRole::Undecided: return "undecided";
            case SchedulerRole::Last: break;
        }
        return "undecided";
    }

} // namespace

bool LeadsTheFleet(FleetSnapshot const& snapshot) noexcept
{
    return snapshot.role == SchedulerRole::Leader;
}

FleetSnapshot CollectFleet(FleetSources const& sources)
{
    FleetSnapshot snapshot;
    if (sources.scheduler == nullptr)
        return snapshot;

    snapshot.role = sources.scheduler->Role();
    snapshot.leaderEndpoint = std::string { sources.scheduler->LeaderEndpoint() };
    snapshot.nodes = sources.scheduler->Workers().NodeReports();
    snapshot.workers = sources.scheduler->Workers().LiveWorkerReports();
    snapshot.liveLeases = sources.scheduler->LiveLeaseCount();

    // Absent rather than empty: a node started without `--node-id` leads itself and
    // has no replicated state at all, which is not the same claim as a cluster that
    // has agreed on nobody.
    if (sources.cluster != nullptr)
        snapshot.cluster = sources.cluster->ClusterState();

    if (sources.metrics != nullptr)
    {
        snapshot.leases.reserve(LeaseOutcomeTable.size());
        for (auto const& row: LeaseOutcomeTable)
            snapshot.leases.push_back(sources.metrics->Read(row.counter));
        snapshot.registrations = sources.metrics->Read(IMetricsSink::Counter::DispatchWorkerRegistrations);
    }

    for (auto const& tier: StorageTierTable)
        snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)] = AnyNodeHasTier(snapshot.nodes, tier.tier);

    return snapshot;
}

namespace
{
    /// Append one JSON array of objects, one per subject, driven by a column table.
    template <typename Subject, std::size_t N>
    void AppendJsonRows(std::string& out,
                        std::array<FleetColumn<Subject>, N> const& columns,
                        std::vector<Subject> const& subjects)
    {
        out += '[';
        bool firstRow = true;
        for (auto const& subject: subjects)
        {
            if (!std::exchange(firstRow, false))
                out += ',';
            out += '{';
            bool firstCell = true;
            for (auto const& column: columns)
            {
                if (!std::exchange(firstCell, false))
                    out += ',';
                AppendJsonString(out, column.name);
                out += ':';
                AppendCellAsJson(out, column.project(subject));
            }
            out += '}';
        }
        out += ']';
    }

    /// Append one HTML table, driven by a column table.
    template <typename Subject, std::size_t N>
    void AppendHtmlRows(std::string& out,
                        std::array<FleetColumn<Subject>, N> const& columns,
                        std::vector<Subject> const& subjects)
    {
        out += "<table><thead><tr>";
        for (auto const& column: columns)
            out += std::format(R"(<th title="{}">{}</th>)", EscapeHtml(column.help), EscapeHtml(column.name));
        out += "</tr></thead><tbody>";
        if (subjects.empty())
            out += std::format(R"(<tr><td class="empty" colspan="{}">(none)</td></tr>)", columns.size());
        for (auto const& subject: subjects)
        {
            out += "<tr>";
            for (auto const& column: columns)
            {
                auto const cell = column.project(subject);
                auto const absent = cell.kind == FleetCell::Kind::Absent ? R"( class="absent")" : "";
                out += std::format("<td{}>{}</td>", absent, CellAsText(cell, column.format));
            }
            out += "</tr>";
        }
        out += "</tbody></table>";
    }
} // namespace

std::string RenderFleetJson(FleetSnapshot const& snapshot)
{
    std::string out;
    out.reserve(4096);
    out += '{';

    AppendJsonString(out, "role");
    out += ':';
    AppendJsonString(out, RoleName(snapshot.role));

    out += ',';
    AppendJsonString(out, "leader");
    out += ':';
    // Absent, not an empty string: while an election is in progress there is
    // nobody to name, which is a different fact from a leader whose address is "".
    if (snapshot.leaderEndpoint.empty())
        out += "null";
    else
        AppendJsonString(out, snapshot.leaderEndpoint);

    out += ',';
    AppendJsonString(out, "members");
    out += ':';
    if (snapshot.cluster.has_value())
        AppendJsonRows(out, MemberColumns, snapshot.cluster->members);
    else
        out += "null"; // Runs no cluster at all -- not "a cluster with no members".

    out += ',';
    AppendJsonString(out, "nodes");
    out += ':';
    AppendJsonRows(out, NodeColumns, snapshot.nodes);

    // The per-tier columns travel as their own object per node, so a tier no
    // member runs contributes no key at all rather than a null nobody asked for.
    out += ',';
    AppendJsonString(out, "node-tiers");
    out += ":[";
    bool firstNode = true;
    for (auto const& node: snapshot.nodes)
    {
        if (!std::exchange(firstNode, false))
            out += ',';
        out += '{';
        AppendJsonString(out, "endpoint");
        out += ':';
        AppendJsonString(out, node.endpoint);
        for (auto const& tier: StorageTierTable)
        {
            if (!snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)])
                continue;
            for (auto const& column: TierColumns)
            {
                out += ',';
                AppendJsonString(out, TierColumnName(tier.tier, column.suffix));
                out += ':';
                AppendCellAsJson(out, column.project(node, tier.tier));
            }
        }
        out += '}';
    }
    out += ']';

    out += ',';
    AppendJsonString(out, "workers");
    out += ':';
    AppendJsonRows(out, WorkerColumns, snapshot.workers);

    out += ',';
    AppendJsonString(out, "leases");
    out += ":{";
    for (auto const& [index, row]: std::views::enumerate(LeaseOutcomeTable))
    {
        if (index != 0)
            out += ',';
        AppendJsonString(out, row.key);
        out += ':';
        out += std::format("{}",
                           static_cast<std::size_t>(index) < snapshot.leases.size()
                               ? snapshot.leases[static_cast<std::size_t>(index)]
                               : 0);
    }
    out += '}';

    out += ',';
    AppendJsonString(out, "leases-outstanding");
    out += std::format(":{}", snapshot.liveLeases);
    out += ',';
    AppendJsonString(out, "registrations");
    out += std::format(":{}", snapshot.registrations);

    out += '}';
    return out;
}

namespace
{
    /// The page's whole stylesheet, embedded.
    ///
    /// No CDN, no bundled framework and no script: the issue forbids a new
    /// dependency, and a page that fetched one would also be a page that does not
    /// render on the air-gapped network a build fleet usually lives on.
    constexpr std::string_view DashboardStyle = R"CSS(
:root { color-scheme: light dark; --fg: #1a1a1a; --bg: #fbfbfb; --muted: #6a6a6a;
        --line: #d8d8d8; --head: #f0f0f0; --accent: #1a5fb4; }
@media (prefers-color-scheme: dark) {
  :root { --fg: #e6e6e6; --bg: #161616; --muted: #9a9a9a;
          --line: #333; --head: #202020; --accent: #78aeed; } }
body { font: 14px/1.5 system-ui, -apple-system, Segoe UI, sans-serif; color: var(--fg);
       background: var(--bg); margin: 0; padding: 1.5rem; }
h1 { font-size: 1.3rem; margin: 0 0 .25rem; }
h2 { font-size: 1rem; margin: 1.75rem 0 .5rem; }
p.sub { color: var(--muted); margin: 0 0 1rem; }
table { border-collapse: collapse; width: 100%; margin-bottom: .5rem; font-variant-numeric: tabular-nums; }
th, td { text-align: left; padding: .35rem .6rem; border-bottom: 1px solid var(--line); }
th { background: var(--head); font-weight: 600; white-space: nowrap; }
td.absent { color: var(--muted); }
td.empty { color: var(--muted); font-style: italic; }
.note { color: var(--muted); font-size: .85rem; margin: .25rem 0 0; }
.leader { color: var(--accent); font-weight: 600; }
.wrap { overflow-x: auto; }
)CSS";

    /// The sentence a split of numbers needs beside it.
    constexpr std::string_view LeaseNote =
        "Do not add these together. An empty fleet, a busy one, machines somebody else is using and an object "
        "already being built are four different problems with four different fixes, and a total hides all of them.";
} // namespace

std::string RenderFleetHtml(FleetSnapshot const& snapshot, unsigned refreshSeconds)
{
    std::string out;
    out.reserve(8192);
    out += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">";
    out += R"(<meta name="viewport" content="width=device-width, initial-scale=1">)";
    if (refreshSeconds != 0)
        out += std::format(R"(<meta http-equiv="refresh" content="{}">)", refreshSeconds);
    out += "<title>fastcache fleet</title><style>";
    out += DashboardStyle;
    out += "</style></head><body>";

    out += "<h1>fastcache fleet</h1>";

    if (!LeadsTheFleet(snapshot))
    {
        // A page, not a redirect. A dashboard address is local configuration on
        // each node and is not replicated, so any URL built here would be a guess
        // -- and a redirect naming a port the browser cannot use is the failure
        // this project has already had once, when a follower sent clients to the
        // leader's consensus port.
        out += std::format(R"(<p class="sub">This node is a <strong>{}</strong> and cannot answer for the fleet: )"
                           R"(its registry holds only what registered against it.</p>)",
                           EscapeHtml(RoleName(snapshot.role)));
        if (snapshot.leaderEndpoint.empty())
            out += "<p>No leader is known — an election is in progress, so there is nobody to name yet.</p>";
        else
            out += std::format(R"(<p>The leader answers clients at <span class="leader">{}</span>.</p>)"
                               R"(<p class="note">That is its <em>scheduler</em> port, not its dashboard. )"
                               R"(Ask that machine's admin endpoint for this page — where it is served is )"
                               R"(configuration on that node, and nothing replicates it here.</p>)",
                               EscapeHtml(snapshot.leaderEndpoint));
        out += "</body></html>";
        return out;
    }

    out += std::format(R"(<p class="sub">Served by the leader. {} member(s), {} machine(s), {} worker entr(ies), )"
                       R"({} lease(s) outstanding.</p>)",
                       snapshot.cluster.has_value() ? snapshot.cluster->members.size() : 0,
                       snapshot.nodes.size(),
                       snapshot.workers.size(),
                       snapshot.liveLeases);

    out += "<h2>Members</h2><div class=\"wrap\">";
    if (snapshot.cluster.has_value())
        AppendHtmlRows(out, MemberColumns, snapshot.cluster->members);
    else
        out += R"(<p class="note">This node runs no cluster: it leads itself, and has no replicated state.</p>)";
    out += "</div>";

    out += "<h2>Machines</h2><div class=\"wrap\">";
    AppendHtmlRows(out, NodeColumns, snapshot.nodes);
    out += R"(<p class="note">One row per machine, not per toolchain: a node started with two --toolchain flags )"
           R"(is two worker entries carrying one machine's cores, and summing them would report a fleet twice )"
           R"(the size of the one you own.</p></div>)";

    // Per-tier cache, rendered only for tiers some member actually runs. A table
    // cannot omit one cell the way a scrape omits a line, so the granularity of
    // "absent is not zero" here is the column.
    auto const anyTier = std::ranges::any_of(StorageTierTable, [&snapshot](auto const& tier) {
        return snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)];
    });
    if (anyTier)
    {
        out += "<h2>Caches</h2><div class=\"wrap\"><table><thead><tr><th>endpoint</th>";
        for (auto const& tier: StorageTierTable)
        {
            if (!snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)])
                continue;
            for (auto const& column: TierColumns)
                out += std::format(R"(<th title="{}">{}</th>)",
                                   EscapeHtml(column.help),
                                   EscapeHtml(TierColumnName(tier.tier, column.suffix)));
        }
        out += "</tr></thead><tbody>";
        for (auto const& node: snapshot.nodes)
        {
            out += std::format("<tr><td>{}</td>", EscapeHtml(node.endpoint));
            for (auto const& tier: StorageTierTable)
            {
                if (!snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)])
                    continue;
                for (auto const& column: TierColumns)
                {
                    auto const cell = column.project(node, tier.tier);
                    auto const absent = cell.kind == FleetCell::Kind::Absent ? R"( class="absent")" : "";
                    out += std::format("<td{}>{}</td>", absent, CellAsText(cell, column.format));
                }
            }
            out += "</tr>";
        }
        out += "</tbody></table>";
        out += R"(<p class="note">Nothing here is a total waiting to be summed: the memory tier mirrors what it )"
               R"(reads out of the disk tier, so adding the item counts counts the mirrored entries twice.</p></div>)";
    }

    out += "<h2>Workers</h2><div class=\"wrap\">";
    AppendHtmlRows(out, WorkerColumns, snapshot.workers);
    out += "</div>";

    out += "<h2>Leases</h2><div class=\"wrap\"><table><thead><tr><th>outcome</th><th>count</th><th>what it means"
           "</th></tr></thead><tbody>";
    for (auto const& [index, row]: std::views::enumerate(LeaseOutcomeTable))
    {
        auto const value = static_cast<std::size_t>(index) < snapshot.leases.size()
                               ? snapshot.leases[static_cast<std::size_t>(index)]
                               : 0;
        out += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                           EscapeHtml(row.label),
                           value,
                           EscapeHtml(row.meaning));
    }
    out += "</tbody></table>";
    out += std::format(R"(<p class="note">{}</p></div>)", EscapeHtml(LeaseNote));

    out += std::format(R"(<p class="note">{} worker registration(s) accepted. )"
                       R"(/metrics remains the source of truth for anything alertable.</p>)",
                       snapshot.registrations);

    out += "</body></html>";
    return out;
}

} // namespace FastCache::Distributed

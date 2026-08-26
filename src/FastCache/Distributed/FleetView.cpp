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

    /// How the PAGE dresses a cell. The JSON ignores it entirely: a chip and a
    /// freshness pill are a reader's shorthand for a value the machine-readable
    /// form already carries verbatim, so decorating there would put presentation
    /// into a wire format.
    ///
    /// A column of the table rather than a branch in the renderer, for the reason
    /// every other column is: the next decorated column is a row, not an `if`
    /// somebody has to find.
    enum class CellDecor : std::uint8_t
    {
        Plain = 0, ///< The value, as text.
        Limit,     ///< Which ceiling bound a worker: a chip, coloured by which one.
        Freshness, ///< A heartbeat age: a pill that goes amber once the value is stale.
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
        std::string_view name;                   ///< JSON key AND header cell: one spelling, so the two cannot disagree.
        std::string_view help;                   ///< The header's tooltip; a reader's, so not in the JSON.
        CellFormat format { CellFormat::Count }; ///< How the page renders it.
        CellDecor decor { CellDecor::Plain };    ///< How the page dresses it; the JSON ignores this.
        FleetCell (*project)(Subject const&);    ///< What to read.
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
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                case '"':
                    out += "&quot;";
                    break;
                case '\'':
                    out += "&#39;";
                    break;
                default:
                    out += ch;
                    break;
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
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
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
            case CellFormat::Bytes:
                return HumanBytes(cell.number);
            case CellFormat::Permille:
                return std::format("{:.1f}%", static_cast<double>(cell.number) / 10.0);
            case CellFormat::Millis:
                return HumanMillis(cell.number);
            case CellFormat::Count:
            case CellFormat::Text:
                break;
        }
        return std::format("{}", cell.number);
    }

    /// One cell, as the JSON carries it. Absent is `null`, never `0`.
    void AppendCellAsJson(std::string& out, FleetCell const& cell)
    {
        switch (cell.kind)
        {
            case FleetCell::Kind::Absent:
                out += "null";
                break;
            case FleetCell::Kind::Number:
                out += std::format("{}", cell.number);
                break;
            case FleetCell::Kind::Text:
                AppendJsonString(out, cell.text);
                break;
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
        FleetColumn<Cluster::ClusterMember> { .name = "id",
                                              .help = "The member's stable identity: what consensus counts.",
                                              .format = CellFormat::Text,
                                              .project =
                                                  [](Cluster::ClusterMember const& m) {
                                                      return FleetCell::Of(m.id);
                                                  } },
        FleetColumn<Cluster::ClusterMember> { .name = "raft-endpoint",
                                              .help = "Where its consensus port answers. Always present.",
                                              .format = CellFormat::Text,
                                              .project =
                                                  [](Cluster::ClusterMember const& m) {
                                                      return FleetCell::Of(m.raftEndpoint);
                                                  } },
        FleetColumn<Cluster::ClusterMember> {
            .name = "scheduler-endpoint",
            .help = "Where clients reach the fleet while this member leads. Absent until it has led.",
            .format = CellFormat::Text,
            // Absent, not an empty string presented as a fact: a member that has
            // never led has not said, which is the normal state rather than a
            // fault.
            .project =
                [](Cluster::ClusterMember const& m) {
                    return m.schedulerEndpoint.empty() ? FleetCell::Nothing() : FleetCell::Of(m.schedulerEndpoint);
                } },
    };

    /// What a node row shows, before its per-tier cache columns.
    constexpr std::array<FleetColumn<NodeReport>, 10> NodeColumns {
        FleetColumn<NodeReport> { .name = "endpoint",
                                  .help = "host:port the machine answers on.",
                                  .format = CellFormat::Text,
                                  .project =
                                      [](NodeReport const& n) {
                                          return FleetCell::Of(n.endpoint);
                                      } },
        FleetColumn<NodeReport> { .name = "toolchains",
                                  .help = "How many toolchains this one machine serves. Each is a separate registry entry.",
                                  .format = CellFormat::Count,
                                  .project =
                                      [](NodeReport const& n) {
                                          return FleetCell::Of(n.fingerprints.size());
                                      } },
        FleetColumn<NodeReport> { .name = "cores",
                                  .help = "Hardware threads. Absent when the machine could not read its own.",
                                  .format = CellFormat::Count,
                                  // Zero means "did not say" in `NodeCapacity`, and rendering it as 0
                                  // would claim a machine with no CPU.
                                  .project =
                                      [](NodeReport const& n) {
                                          return n.capacity.logicalCores == 0 ? FleetCell::Nothing()
                                                                              : FleetCell::Of(n.capacity.logicalCores);
                                      } },
        FleetColumn<NodeReport> { .name = "memory",
                                  .help = "Physical memory. Absent when the machine did not say.",
                                  .format = CellFormat::Bytes,
                                  .project =
                                      [](NodeReport const& n) {
                                          return n.capacity.totalMemoryBytes == 0
                                                     ? FleetCell::Nothing()
                                                     : FleetCell::Of(n.capacity.totalMemoryBytes);
                                      } },
        FleetColumn<NodeReport> {
            .name = "class",
            .help = "How hard this machine may be driven, and how many cores are held back for whoever uses it.",
            .format = CellFormat::Text,
            .project =
                [](NodeReport const& n) {
                    auto const& traits = TraitsFor(n.capacity.nodeClass);
                    auto const reserve = n.capacity.reserveIsExplicit ? n.capacity.reservedCores : traits.reservedCores;
                    return FleetCell::Of(std::format("{} (reserve {})", traits.name, reserve));
                } },
        FleetColumn<NodeReport> { .name = "cpu-busy",
                                  .help = "Host-wide CPU in use, this fleet's work included. Absent when unread.",
                                  .format = CellFormat::Permille,
                                  .project =
                                      [](NodeReport const& n) {
                                          return FleetCell::Maybe(n.load.cpuBusyPermille);
                                      } },
        FleetColumn<NodeReport> { .name = "memory-available",
                                  .help = "Memory a new compile could get. Absent when unread.",
                                  .format = CellFormat::Bytes,
                                  .project =
                                      [](NodeReport const& n) {
                                          return FleetCell::Maybe(n.load.availableMemoryBytes);
                                      } },
        FleetColumn<NodeReport> { .name = "scratch-free",
                                  .help = "Room where compiles run. The limit that most often reaches zero.",
                                  .format = CellFormat::Bytes,
                                  .project =
                                      [](NodeReport const& n) {
                                          return FleetCell::Maybe(n.load.freeScratchBytes);
                                      } },
        FleetColumn<NodeReport> { .name = "cache-hit-rate",
                                  .help = "Reads this node's cache served. Absent when it has served none.",
                                  .format = CellFormat::Permille,
                                  .project =
                                      [](NodeReport const& n) {
                                          return HitRateOf(n.load.cache);
                                      } },
        FleetColumn<NodeReport> { .name = "heartbeat-age",
                                  .help = "Since this machine last reported. Everything on its row is that old.",
                                  .format = CellFormat::Millis,
                                  .decor = CellDecor::Freshness,
                                  // The column that tells "this cache is empty" from "this node stopped
                                  // answering an hour ago and these are its last figures" -- which look
                                  // identical without it, and lead to opposite conclusions.
                                  .project =
                                      [](NodeReport const& n) {
                                          return FleetCell::Of(static_cast<std::uint64_t>(n.heartbeatAge.count()));
                                      } },
    };

    /// What a worker row shows.
    constexpr std::array<FleetColumn<WorkerReport>, 8> WorkerColumns {
        FleetColumn<WorkerReport> { .name = "id",
                                    .help = "The id this leader assigned at registration.",
                                    .format = CellFormat::Text,
                                    .project =
                                        [](WorkerReport const& w) {
                                            return FleetCell::Of(w.info.id);
                                        } },
        FleetColumn<WorkerReport> { .name = "toolchain",
                                    .help = "Matched byte-for-byte. A job never crosses fingerprints.",
                                    .format = CellFormat::Text,
                                    .project =
                                        [](WorkerReport const& w) {
                                            return FleetCell::Of(w.info.fingerprint);
                                        } },
        FleetColumn<WorkerReport> { .name = "endpoint",
                                    .help = "host:port a client is sent to.",
                                    .format = CellFormat::Text,
                                    .project =
                                        [](WorkerReport const& w) {
                                            return FleetCell::Of(w.info.endpoint);
                                        } },
        FleetColumn<WorkerReport> { .name = "slots",
                                    .help = "Concurrent compiles it registered with.",
                                    .format = CellFormat::Count,
                                    .project =
                                        [](WorkerReport const& w) {
                                            return FleetCell::Of(w.info.slots);
                                        } },
        FleetColumn<WorkerReport> { .name = "in-flight",
                                    .help = "This fleet's compiles running on it right now.",
                                    .format = CellFormat::Count,
                                    .project =
                                        [](WorkerReport const& w) {
                                            return FleetCell::Of(w.info.inFlight);
                                        } },
        FleetColumn<WorkerReport> {
            .name = "available",
            .help = "Compiles it may take right now. Below the registered count when something withdrew capacity.",
            .format = CellFormat::Count,
            .project =
                [](WorkerReport const& w) {
                    return FleetCell::Of(AvailableSlots(w.info.capacity, w.info.slots, w.info.load));
                } },
        FleetColumn<WorkerReport> { .name = "limited-by",
                                    .help = "Which ceiling withdrew the difference: the three have opposite fixes.",
                                    .format = CellFormat::Text,
                                    .decor = CellDecor::Limit,
                                    .project =
                                        [](WorkerReport const& w) {
                                            auto const ceilings =
                                                SlotCeilingsFor(w.info.capacity, w.info.slots, w.info.load);
                                            return FleetCell::Of(std::string { TraitsFor(ceilings.binding).name });
                                        } },
        FleetColumn<WorkerReport> { .name = "heartbeat-age",
                                    .help = "Since this entry last reported. A worker unheard-from is dropped.",
                                    .format = CellFormat::Millis,
                                    .decor = CellDecor::Freshness,
                                    .project =
                                        [](WorkerReport const& w) {
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
        CellFormat format { CellFormat::Count };
        FleetCell (*project)(NodeReport const&, StorageTier);
    };

    constexpr std::array<TierColumn, 4> TierColumns {
        TierColumn { .suffix = "items",
                     .help = "Entries this tier holds.",
                     .format = CellFormat::Count,
                     .project =
                         [](NodeReport const& n, StorageTier tier) {
                             auto const& usage = n.load.cache.tiers[static_cast<std::size_t>(tier)];
                             return usage.has_value() ? FleetCell::Of(usage->itemCount) : FleetCell::Nothing();
                         } },
        TierColumn { .suffix = "bytes",
                     .help = "Bytes this tier holds.",
                     .format = CellFormat::Bytes,
                     .project =
                         [](NodeReport const& n, StorageTier tier) {
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
                     .project =
                         [](NodeReport const& n, StorageTier tier) {
                             auto const& limit = n.capacity.cache.tierBytesLimit[static_cast<std::size_t>(tier)];
                             if (!limit.has_value())
                                 return FleetCell::Nothing();
                             return *limit == 0 ? FleetCell::Of(std::string { "unbounded" }) : FleetCell::Of(*limit);
                         } },
        TierColumn { .suffix = "evictions",
                     .help = "Entries this tier dropped to stay within its budget.",
                     .format = CellFormat::Count,
                     .project =
                         [](NodeReport const& n, StorageTier tier) {
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

    /// The count for one `LeaseOutcomeTable` row, or zero when the snapshot is short.
    ///
    /// A snapshot collected without a metrics sink carries no counts at all, and
    /// both renderers walk the table regardless -- so the bounds check belongs in
    /// one place rather than being spelled the same way twice and drifting.
    /// @param snapshot What to read.
    /// @param index The row's position in `LeaseOutcomeTable`.
    /// @return The count, or zero.
    [[nodiscard]] std::uint64_t CountAt(FleetSnapshot const& snapshot, std::size_t index) noexcept
    {
        return index < snapshot.leases.size() ? snapshot.leases[index] : 0;
    }

    /// How the page and the JSON both name a role.
    [[nodiscard]] std::string_view RoleName(SchedulerRole role) noexcept
    {
        switch (role)
        {
            case SchedulerRole::Leader:
                return "leader";
            case SchedulerRole::Follower:
                return "follower";
            case SchedulerRole::Undecided:
                return "undecided";
            case SchedulerRole::Last:
                break;
        }
        return "undecided";
    }

} // namespace

bool LeadsTheFleet(FleetSnapshot const& snapshot) noexcept
{
    return snapshot.role == SchedulerRole::Leader;
}

FleetTotals TotalsFor(FleetSnapshot const& snapshot) noexcept
{
    FleetTotals totals;
    for (auto const& node: snapshot.nodes)
    {
        // `SlotCeilingsFor` rather than a second copy of the arithmetic: two
        // implementations of what a machine may take is how a worker comes to
        // accept more jobs than the scheduler believes it has, which is the
        // reason that function exists at all.
        auto const ceilings = SlotCeilingsFor(node.capacity, node.registeredSlots, node.load);
        totals.registered += node.registeredSlots;
        totals.inFlight += node.fleetJobsInFlight;
        totals.free += ceilings.available;
    }

    // Saturating, not wrapping. These are unsigned and the three parts are read
    // from a heartbeat that a machine may have sent at different moments, so a
    // ceiling can legitimately exceed what is left after in-flight work. A
    // wrapped `withheld` would draw a bar four billion slots wide.
    auto const used = totals.inFlight + totals.free;
    totals.withheld = used >= totals.registered ? 0U : totals.registered - used;
    return totals;
}

FleetSnapshot CollectFleet(FleetSources const& sources)
{
    FleetSnapshot snapshot;
    if (sources.scheduler == nullptr)
        return snapshot;

    snapshot.role = sources.scheduler->Role();
    snapshot.leaderEndpoint = sources.scheduler->LeaderEndpoint();
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

    /// Dress one cell the way its column asks.
    ///
    /// An absent cell is never decorated: a chip saying nothing, or a pill that is
    /// green because no heartbeat has arrived, would read as a healthy value where
    /// the truth is that nobody said anything.
    /// @param cell What was projected.
    /// @param column The column it came from.
    /// @return The cell's inner HTML.
    template <typename Subject>
    [[nodiscard]] std::string Decorate(FleetCell const& cell, FleetColumn<Subject> const& column)
    {
        // Not `auto const`: this is returned by value, and const defeats the
        // implicit move, copying every cell on the page.
        auto text = CellAsText(cell, column.format);
        if (cell.kind == FleetCell::Kind::Absent)
            return text;

        switch (column.decor)
        {
            case CellDecor::Plain:
                break;
            case CellDecor::Limit: {
                // The class is derived from the value rather than mapped in a
                // second table: `SlotLimitTable` already names these, and a
                // lookup here would be a second place to update when it grows.
                char const* modifier = "chip--cpu";
                if (cell.text == "scratch")
                    modifier = "chip--scratch";
                else if (cell.text == "registered")
                    modifier = "chip--registered";
                return std::format(R"(<span class="chip {}">{}</span>)", modifier, text);
            }
            case CellDecor::Freshness: {
                // Amber past the point where a reader should stop trusting the
                // rest of the row. Everything on it is as old as this number.
                constexpr std::uint64_t StaleAfterMillis = 15'000;
                auto const* const tone = cell.number >= StaleAfterMillis ? "pill--warn" : "pill--ok";
                return std::format(R"(<span class="pill pill--value {}"><span class="dot"></span>{}</span>)", tone, text);
            }
        }
        return text;
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
                auto const* const absent = cell.kind == FleetCell::Kind::Absent ? R"( class="absent")" : "";
                out += std::format("<td{}>{}</td>", absent, Decorate(cell, column));
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
    for (auto const index: std::views::iota(std::size_t { 0 }, LeaseOutcomeTable.size()))
    {
        if (index != 0)
            out += ',';
        AppendJsonString(out, LeaseOutcomeTable[index].key);
        out += ':';
        out += std::format("{}", CountAt(snapshot, index));
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
:root {
  --ground:#EEF1F5; --surface:#FFF; --sunk:#E4E8EE; --ink:#10141A; --muted:#616B78;
  --faint:#8A939F; --line:#D2D8E0; --hairline:#E3E8EE;
  --accent:#2F5FA8; --accent-soft:#DCE6F5; --ok:#197A4B; --ok-soft:#DCEFE4;
  --warn:#A15C07; --warn-soft:#F7EBD6; --crit:#B3261E; --crit-soft:#F8E1DF; --inert:#93A0B0;
  --shadow:0 1px 2px rgb(16 20 26/.06),0 4px 14px rgb(16 20 26/.05);
}
@media (prefers-color-scheme: dark) {
  :root {
    --ground:#0D1016; --surface:#151A21; --sunk:#1D242D; --ink:#E3E8EE; --muted:#8D97A5;
    --faint:#6B7686; --line:#29313B; --hairline:#212932;
    --accent:#6E9BE0; --accent-soft:#1B2739; --ok:#45AE79; --ok-soft:#14261D;
    --warn:#D2913A; --warn-soft:#2A2115; --crit:#E36B6B; --crit-soft:#2C1918; --inert:#5B6675;
    --shadow:0 1px 2px rgb(0 0 0/.5),0 4px 16px rgb(0 0 0/.35);
  }
}
* { box-sizing:border-box; }
body { margin:0; padding:0 0 3rem; background:var(--ground); color:var(--ink);
       font:400 14px/1.55 ui-sans-serif,system-ui,-apple-system,Segoe UI,sans-serif;
       font-variant-numeric:tabular-nums; }
.shell { max-width:1240px; margin:0 auto; padding:0 1.5rem; }
.mono,td.num,.kpi-value,.chip,.pill,th { font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }
.statusbar { border-bottom:1px solid var(--line); background:var(--surface); }
.statusbar .shell { display:flex; flex-wrap:wrap; align-items:baseline; gap:.7rem 1.2rem;
                    padding-top:.85rem; padding-bottom:.85rem; }
.brand { font:600 15px/1 ui-monospace,monospace; letter-spacing:.02em; }
.brand span { color:var(--muted); font-weight:400; }
.spacer { margin-left:auto; }
.meta { color:var(--muted); font-size:12px; font-family:ui-monospace,monospace; }
.meta b { color:var(--ink); font-weight:500; }
.pill { display:inline-flex; align-items:center; gap:.4rem; font-size:11px; font-weight:600;
        letter-spacing:.06em; text-transform:uppercase; padding:.2rem .5rem; border-radius:3px;
        border:1px solid transparent; }
.pill--leader { background:var(--accent-soft); color:var(--accent); }
.pill--ok   { background:var(--ok-soft);   color:var(--ok); }
/* A pill carrying a VALUE is not a label: uppercasing turns "1.2 s" into
   "1.2 S", which is a unit that does not exist. */
.pill--value { text-transform:none; letter-spacing:normal; font-weight:500; }
.pill--warn { background:var(--warn-soft); color:var(--warn); }
.dot { width:6px; height:6px; border-radius:50%; background:currentColor; flex:none; }
section { margin-top:2.1rem; }
.sec-head { display:flex; align-items:baseline; gap:.75rem; flex-wrap:wrap; margin-bottom:.65rem; }
h1 { font-size:1.15rem; margin:0; }
h2 { margin:0; font:600 11px/1 ui-monospace,monospace; letter-spacing:.12em;
     text-transform:uppercase; color:var(--muted); }
.sec-head .rule { flex:1; height:1px; background:var(--line); }
.note { color:var(--muted); font-size:12.5px; margin:.5rem 0 0; max-width:78ch; }
.note strong { color:var(--ink); font-weight:600; }
.panel { background:var(--surface); border:1px solid var(--line); border-radius:3px; box-shadow:var(--shadow); }
.kpis { display:grid; grid-template-columns:repeat(auto-fit,minmax(168px,1fr)); gap:1px;
        background:var(--line); border:1px solid var(--line); border-radius:3px;
        overflow:hidden; box-shadow:var(--shadow); }
.kpi { background:var(--surface); padding:.8rem 1rem .85rem; display:flex; flex-direction:column; gap:.25rem; }
.kpi-label { font:500 10.5px/1.3 ui-monospace,monospace; letter-spacing:.08em;
             text-transform:uppercase; color:var(--muted); }
.kpi-value { font-size:22px; font-weight:600; line-height:1.1; }
.kpi-value small { font-size:12px; font-weight:400; color:var(--muted); margin-left:.15rem; }
.kpi-sub { font-size:11.5px; color:var(--faint); font-family:ui-monospace,monospace; }
.occ { padding:1.1rem 1.2rem 1.2rem; }
.occ-bar { display:flex; height:34px; border-radius:2px; overflow:hidden;
           background:var(--sunk); border:1px solid var(--line); }
.occ-seg { min-width:0; }
.occ-busy { background:var(--accent); }
.occ-free { background:repeating-linear-gradient(90deg,var(--sunk) 0 5px,var(--hairline) 5px 6px); }
.occ-held { background:repeating-linear-gradient(135deg,var(--warn-soft) 0 6px,var(--sunk) 6px 12px); }
.occ-scale { display:flex; justify-content:space-between; margin-top:.35rem;
             font:400 11px/1 ui-monospace,monospace; color:var(--faint); }
.legend { display:flex; flex-wrap:wrap; gap:.25rem 1.4rem; margin-top:.85rem; }
.legend-item { font-size:12.5px; }
.swatch { display:inline-block; width:11px; height:11px; border-radius:2px;
          transform:translateY(1px); margin-right:.45rem; border:1px solid rgb(0 0 0/.12); }
.swatch--busy { background:var(--accent); }
.swatch--free { background:var(--sunk); }
.swatch--held { background:var(--warn-soft); }
.legend-item b { font-family:ui-monospace,monospace; }
.legend-item span { color:var(--muted); }
.wrap { overflow-x:auto; }
table { border-collapse:collapse; width:100%; font-size:13px; }
th { text-align:left; font-size:10.5px; font-weight:600; letter-spacing:.07em; text-transform:uppercase;
     color:var(--muted); white-space:nowrap; padding:.5rem .7rem;
     border-bottom:1px solid var(--line); background:var(--surface); }
td { padding:.5rem .7rem; border-bottom:1px solid var(--hairline); white-space:nowrap; vertical-align:middle; }
tbody tr:last-child td { border-bottom:0; }
td.num { text-align:right; }
td.absent { color:var(--faint); }
td.empty { color:var(--muted); font-style:italic; }
th[title] { text-decoration:underline dotted var(--line); text-underline-offset:3px; }
.chip { display:inline-block; font-size:10.5px; font-weight:500; padding:.12rem .4rem; border-radius:2px;
        background:var(--sunk); color:var(--muted); border:1px solid var(--hairline); }
.chip--cpu { background:var(--warn-soft); color:var(--warn); }
.chip--scratch { background:var(--crit-soft); color:var(--crit); }
.chip--registered { background:var(--ok-soft); color:var(--ok); }
.bar { display:inline-flex; align-items:center; gap:.45rem; }
.bar-track { width:54px; height:6px; border-radius:2px; background:var(--sunk);
             overflow:hidden; border:1px solid var(--hairline); flex:none; }
.bar-fill { display:block; height:100%; background:var(--accent); }
.bar-fill--ok { background:var(--ok); }
.bar-fill--warn { background:var(--warn); }
.bar-fill--crit { background:var(--crit); }
.reasons { display:grid; grid-template-columns:repeat(auto-fit,minmax(205px,1fr)); gap:1px;
           background:var(--line); border:1px solid var(--line); border-radius:3px;
           overflow:hidden; box-shadow:var(--shadow); }
.reason { background:var(--surface); padding:.75rem .95rem .85rem; border-top:3px solid var(--inert); }
.reason--granted { border-top-color:var(--ok); }
.reason--no-worker { border-top-color:var(--crit); }
.reason--no-capacity { border-top-color:var(--accent); }
.reason--withdrawn { border-top-color:var(--warn); }
.reason-n { font:600 19px/1.15 ui-monospace,monospace; }
.reason-k { font:500 10.5px/1.3 ui-monospace,monospace; letter-spacing:.07em;
            text-transform:uppercase; color:var(--muted); margin-top:.1rem; }
.reason-d { font-size:12px; color:var(--muted); margin:.4rem 0 0; line-height:1.45; }
.follower { padding:1.1rem 1.2rem 1.2rem; border-left:3px solid var(--warn); }
.leader { font-family:ui-monospace,monospace; font-weight:600; color:var(--accent); }
footer { margin-top:2.4rem; padding-top:1rem; border-top:1px solid var(--line);
         color:var(--faint); font-size:12px; }
)CSS";

    /// The sentence a split of numbers needs beside it.
    constexpr std::string_view LeaseNote =
        "Do not add these together. An empty fleet, a busy one, machines somebody else is using and an object "
        "already being built are four different problems with four different fixes, and a total hides all of them.";
} // namespace

std::string RenderFleetHtml(FleetSnapshot const& snapshot, unsigned refreshSeconds)
{
    std::string out;
    out.reserve(16384);
    out += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">";
    out += R"(<meta name="viewport" content="width=device-width, initial-scale=1">)";
    if (refreshSeconds != 0)
        out += std::format(R"(<meta http-equiv="refresh" content="{}">)", refreshSeconds);
    out += "<title>fastcache fleet</title><style>";
    out += DashboardStyle;
    out += "</style></head><body>";

    auto const leads = LeadsTheFleet(snapshot);

    out += R"(<div class="statusbar"><div class="shell"><span class="brand">fastcache<span>/fleet</span></span>)";
    out += std::format(R"(<span class="pill pill--{}"><span class="dot"></span>{}</span>)",
                       leads ? "leader" : "warn",
                       EscapeHtml(RoleName(snapshot.role)));
    if (leads)
        out += std::format(R"(<span class="spacer"></span><span class="meta">)"
                           R"({} member(s) &middot; {} machine(s) &middot; {} worker entr(ies)</span>)",
                           snapshot.cluster.has_value() ? snapshot.cluster->members.size() : 0,
                           snapshot.nodes.size(),
                           snapshot.workers.size());
    out += R"(</div></div><div class="shell">)";

    if (!leads)
    {
        // A page, not a redirect. A dashboard address is local configuration on
        // each node and is not replicated, so any URL built here would be a guess
        // -- and a redirect naming a port the browser cannot use is the failure
        // this project has already had once, when a follower sent clients to the
        // leader's consensus port.
        out += R"(<section><div class="panel follower"><h1>This node cannot answer for the fleet.</h1>)";
        out += std::format(R"(<p class="note">Its registry holds only what registered against it, which is a )"
                           R"(fraction of the fleet rather than a smaller picture of it.</p>)");
        if (snapshot.leaderEndpoint.empty())
            out += R"(<p class="note">No leader is known &mdash; an election is in progress, so there is )"
                   R"(nobody to name yet.</p>)";
        else
            out += std::format(R"(<p>The leader answers clients at <span class="leader">{}</span>.</p>)"
                               R"(<p class="note">That is its <em>scheduler</em> port, not its dashboard. )"
                               R"(Ask that machine's admin endpoint for this page &mdash; where it is served is )"
                               R"(configuration on that node, and nothing replicates it here, so a link built )"
                               R"(from a guessed port is a link to nowhere. <strong>A page, never a redirect.)"
                               R"(</strong></p>)",
                               EscapeHtml(snapshot.leaderEndpoint));
        out += "</div></section></div></body></html>";
        return out;
    }

    auto const totals = TotalsFor(snapshot);

    // ---- the readouts, before any table ------------------------------------
    out += R"(<section><div class="kpis">)";
    auto const kpi =
        [&out](std::string_view label, std::string const& value, std::string_view unit, std::string const& sub) {
            out += std::format(R"(<div class="kpi"><span class="kpi-label">{}</span>)"
                               R"(<span class="kpi-value">{}<small>{}</small></span>)"
                               R"(<span class="kpi-sub">{}</span></div>)",
                               EscapeHtml(label),
                               EscapeHtml(value),
                               EscapeHtml(unit),
                               EscapeHtml(sub));
        };
    kpi("Compiling now",
        std::to_string(totals.inFlight),
        std::format("/ {} slots", totals.registered),
        "this fleet's own work");
    kpi("Free now", std::to_string(totals.free), "", "a compile could start");
    kpi("Withheld", std::to_string(totals.withheld), "", "CPU or scratch, not us");
    kpi("Leases outstanding", std::to_string(snapshot.liveLeases), "", "granted, not yet claimed");
    kpi("Machines", std::to_string(snapshot.nodes.size()), "", std::format("{} worker entr(ies)", snapshot.workers.size()));
    kpi("Registrations", std::to_string(snapshot.registrations), "", "accepted since start");
    out += "</div></section>";

    // ---- the signature element ---------------------------------------------
    out += R"(<section><div class="sec-head"><h2>Fleet capacity</h2><span class="rule"></span></div>)";
    out += R"(<div class="panel occ">)";
    if (totals.registered == 0)
        out += R"(<p class="note">No machine has registered, so there is no capacity to draw. )"
               R"(A fleet with nothing in it refuses every request with <em>no worker</em>.</p>)";
    else
    {
        out += std::format(R"(<div class="occ-bar" role="img" aria-label="{} registered slots: )"
                           R"({} compiling, {} free, {} withheld by an external limit">)"
                           R"(<div class="occ-seg occ-busy" style="flex-grow:{}"></div>)"
                           R"(<div class="occ-seg occ-free" style="flex-grow:{}"></div>)"
                           R"(<div class="occ-seg occ-held" style="flex-grow:{}"></div></div>)",
                           totals.registered,
                           totals.inFlight,
                           totals.free,
                           totals.withheld,
                           totals.inFlight,
                           totals.free,
                           totals.withheld);
        out +=
            std::format(R"(<div class="occ-scale"><span>0</span><span>{} registered slots</span></div>)", totals.registered);
        // Classes rather than inline styles, and not only for tidiness: a raw
        // string literal is delimited by `)"`, which is exactly what
        // `style="background:var(--accent)"` ends with -- so the inline spelling
        // terminates the literal early and the file stops compiling.
        out += std::format(R"(<div class="legend">)"
                           R"(<span class="legend-item"><span class="swatch swatch--busy"></span>)"
                           R"(<b>{}</b> <span>compiling &mdash; this fleet's work</span></span>)"
                           R"(<span class="legend-item"><span class="swatch swatch--free"></span>)"
                           R"(<b>{}</b> <span>free &mdash; a compile could start now</span></span>)"
                           R"(<span class="legend-item"><span class="swatch swatch--held"></span>)"
                           R"(<b>{}</b> <span>withheld &mdash; CPU or scratch, not us</span></span></div>)",
                           totals.inFlight,
                           totals.free,
                           totals.withheld);
        // The sentence the split exists for. Which of the two shortages a fleet
        // has decides what an operator buys, and a single "utilisation" number
        // answers neither.
        if (totals.withheld > 0)
            out += std::format(R"(<p class="note"><strong>Read the hatching first.</strong> {} of the {} slots )"
                               R"(these machines registered are not offerable right now, and that is not this )"
                               R"(fleet being busy: a ceiling withdrew them, because the host CPU is doing )"
                               R"(somebody else's work or the scratch filesystem is nearly full. Buying machines )"
                               R"(fixes a full blue bar. It does not fix this one.</p>)",
                               totals.withheld,
                               totals.registered);
        else
            out += R"(<p class="note">Nothing is being withheld: every registered slot is offerable, so what is )"
                   R"(not blue is genuinely idle. A fleet that refuses work in this state needs more machines, )"
                   R"(not quieter ones.</p>)";
    }
    out += "</div></section>";

    // ---- machines -----------------------------------------------------------
    out += R"(<section><div class="sec-head"><h2>Machines</h2><span class="rule"></span>)"
           R"(<span class="meta">one row per machine, not per toolchain</span></div>)"
           R"(<div class="panel wrap">)";
    AppendHtmlRows(out, NodeColumns, snapshot.nodes);
    out += "</div>";
    out += R"(<p class="note">A node started with two --toolchain flags is two worker entries carrying one )"
           R"(machine's cores, and summing a hardware column across them would report a fleet twice the size )"
           R"(of the one you own.</p></section>)";

    // ---- workers ------------------------------------------------------------
    out += R"(<section><div class="sec-head"><h2>Workers</h2><span class="rule"></span>)"
           R"(<span class="meta">one row per (toolchain, endpoint)</span></div>)"
           R"(<div class="panel wrap">)";
    AppendHtmlRows(out, WorkerColumns, snapshot.workers);
    out += "</div></section>";

    // ---- why requests were refused -----------------------------------------
    out += R"(<section><div class="sec-head"><h2>Why requests were refused</h2><span class="rule"></span>)"
           R"(<span class="meta">since this leader started</span></div><div class="reasons">)";
    for (auto const index: std::views::iota(std::size_t { 0 }, LeaseOutcomeTable.size()))
    {
        auto const& row = LeaseOutcomeTable[index];
        out += std::format(R"(<div class="reason reason--{}"><div class="reason-n">{}</div>)"
                           R"(<div class="reason-k">{}</div><p class="reason-d">{}</p></div>)",
                           EscapeHtml(row.key),
                           CountAt(snapshot, index),
                           EscapeHtml(row.label),
                           EscapeHtml(row.meaning));
    }
    out += "</div>";
    out += std::format(R"(<p class="note">{}</p></section>)", EscapeHtml(LeaseNote));

    // ---- cache tiers --------------------------------------------------------
    out += R"(<section><div class="sec-head"><h2>Cache tiers</h2><span class="rule"></span></div>)";
    // Per-tier cache, rendered only for tiers some member actually runs. A table
    // cannot omit one cell the way a scrape omits a line, so the granularity of
    // "absent is not zero" here is the column.
    auto const anyTier = std::ranges::any_of(StorageTierTable, [&snapshot](auto const& tier) {
        return snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)];
    });
    if (anyTier)
    {
        out += R"(<div class="panel wrap"><table><thead><tr><th>endpoint</th>)";
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
                    auto const* const absent = cell.kind == FleetCell::Kind::Absent ? R"( class="absent")" : "";
                    out += std::format("<td{}>{}</td>", absent, CellAsText(cell, column.format));
                }
            }
            out += "</tr>";
        }
        out += "</tbody></table>";
        out += "</div>";
        out += R"(<p class="note">Nothing here is a total waiting to be summed: the memory tier mirrors what it )"
               R"(reads out of the disk tier, so adding the item counts counts the mirrored entries twice.</p>)";
    }
    else
        // A heading with nothing under it reads as a broken page. Absent is not
        // zero here either: no member reports a tier, which is a fleet that
        // caches nothing rather than one whose caches are empty.
        out += R"(<p class="note">No member reports a cache tier. That is a fleet whose nodes cache nothing, )"
               R"(not one whose caches happen to be empty &mdash; a node started without --cache-memory or )"
               R"(--cache-dir has no tier to report.</p>)";

    out += "</section>";

    // ---- members ------------------------------------------------------------
    out += R"(<section><div class="sec-head"><h2>Members</h2><span class="rule"></span>)"
           R"(<span class="meta">replicated cluster state</span></div>)";
    if (snapshot.cluster.has_value())
    {
        out += R"(<div class="panel wrap">)";
        AppendHtmlRows(out, MemberColumns, snapshot.cluster->members);
        out += "</div>";
    }
    else
        out += R"(<p class="note">This node runs no cluster: it leads itself, and has no replicated state.</p>)";
    out += "</section>";

    out += R"(<footer>/metrics remains the source of truth for anything alertable)";
    if (refreshSeconds != 0)
        out += std::format(" &middot; this page refreshes every {} s", refreshSeconds);
    out += "</footer>";

    out += "</div></body></html>";
    return out;
}

} // namespace FastCache::Distributed

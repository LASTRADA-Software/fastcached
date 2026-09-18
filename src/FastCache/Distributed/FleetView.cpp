// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/FigureText.hpp>
#include <FastCache/Core/MachineName.hpp>
#include <FastCache/Core/NumericText.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Distributed/FleetChart.hpp>
#include <FastCache/Distributed/FleetText.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>

#include <algorithm>
#include <array>
#include <chrono>
// For `std::llround`, which the KPI cells round with. MSVC pulls this in
// transitively and libstdc++ does not, so its absence is a GCC/Clang-only build
// failure that a green Windows build says nothing about -- measured, thirteen red
// Linux legs against a clean MSVC tree.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
            /// A share in 0..1, carried as the fraction it is (#1445).
            ///
            /// **A fourth kind rather than a scaled `Number`**, which is what this was:
            /// a share travelled as an integer per-mille because `number` is integral,
            /// and the scale then had to be undone somewhere. It was undone on the
            /// human side and NOT on the machine side, so `/fleet.json` and
            /// `/fleet.txt` published `853` where the CLI's own piped output published
            /// `0.7403` for the same kind of figure -- one concept, two scales, no unit
            /// in either header.
            ///
            /// Dividing at emission instead would fix the scale and keep a lie: a true
            /// share of 1/3 reaches an integer per-mille as `333` and comes back as
            /// `0.3330`, whose last digit is wrong rather than absent. The carrier has
            /// to hold what was measured.
            Share,
            Text,
        };

        Kind kind { Kind::Absent };
        std::uint64_t number { 0 };
        /// Meaningful for `Share` alone. Not folded into `number`: a byte count needs
        /// all 64 bits and a `double` carries 53 exactly, so one field for both would
        /// trade a silent rounding of large caches for a tidier struct.
        double share { 0.0 };
        std::string text {};

        [[nodiscard]] static FleetCell Nothing() noexcept
        {
            return FleetCell {};
        }
        [[nodiscard]] static FleetCell Of(std::uint64_t value) noexcept
        {
            return FleetCell { .kind = Kind::Number, .number = value, .share = 0.0, .text = {} };
        }
        [[nodiscard]] static FleetCell Of(std::string value)
        {
            return FleetCell { .kind = Kind::Text, .number = 0, .share = 0.0, .text = std::move(value) };
        }
        /// A share, as the fraction it was measured as.
        /// @param value The share in 0..1.
        /// @return The cell.
        [[nodiscard]] static FleetCell OfShare(double value) noexcept
        {
            return FleetCell { .kind = Kind::Share, .number = 0, .share = value, .text = {} };
        }
        /// An optional's value, or the absence it already carries.
        template <typename T>
        [[nodiscard]] static FleetCell Maybe(std::optional<T> const& value)
        {
            return value.has_value() ? Of(static_cast<std::uint64_t>(*value)) : Nothing();
        }
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
        /// A lease age: the same pill on a far longer scale.
        ///
        /// Not `Freshness`, and the difference is the threshold rather than the
        /// drawing. A heartbeat is stale after fifteen seconds; a lease that old is
        /// an ordinary compile still running, so reusing that decor would paint
        /// every row amber and the colour would stop meaning anything at all.
        LeaseAge,
        /// A condition's severity: a chip coloured by how loudly it asks (#1364).
        Severity,
        /// A condition's state: a chip for a row that asks for attention, plain for one that does not.
        ConditionState,
        /// Prose: a sentence that wraps rather than a figure that must not, so a remedy does not
        /// drag its whole table a screen wide.
        Prose,
    };

    /// Append the headline figures as a table, and the same for the JSON object.
    ///
    /// DEFINED below, beside `KpiTable`: the two machine-readable renderers are laid
    /// out above the strip's projections in this file, and a forward declaration is
    /// cheaper than moving either block past the other. Every unnamed-namespace block
    /// in a translation unit is the same namespace, so this is the same entity.
    /// @param out Appended to.
    /// @param snapshot What to read.
    /// @param history The window the history-derived figures answer for.
    void AppendKpiText(std::string& out, FleetSnapshot const& snapshot, FleetHistoryView const& history);
    void AppendKpiJson(std::string& out, FleetSnapshot const& snapshot, FleetHistoryView const& history);

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
        ColumnKeep keep { ColumnKeep::Useful };  ///< How long a narrow human surface keeps it; see `FleetColumnKeep`.
        FleetCell (*project)(Subject const&);    ///< What to read.
    };

    // ---------------------------------------------------------------- helpers

    /// The two escapes live in `FleetText.hpp` so the chart renderer shares them
    /// rather than carrying a third copy. Named as they were here, so no call site
    /// moves.
    [[nodiscard]] std::string EscapeHtml(std::string_view text)
    {
        return EscapeMarkup(text);
    }

    void AppendJsonString(std::string& out, std::string_view text)
    {
        AppendJsonText(out, text);
    }

    /// How a human surface dresses one slot limit: the page's chip class, and the tone every other
    /// surface tints the same cell with.
    struct LimitDress
    {
        SlotLimit limit;            ///< The enumerator this row describes.
        std::string_view chipClass; ///< The page's chip.
        CellTone tone;              ///< The tone a terminal gives it.
    };

    /// One row per slot limit, in enumerator order.
    ///
    /// A table rather than the `if` ladder this used to be, and the ladder is why:
    /// it named `scratch` and `registered` and let everything else fall through to
    /// the CPU class, so a MEMORY-bound worker was dressed as "somebody else is
    /// using this machine" -- pointing an operator at the wrong remedy, which is
    /// the one thing `SlotLimitTraits::remedy` exists to get right. The chip and the
    /// tone are one row, so the page and a terminal cannot dress one limit two ways;
    /// a terminal has no accent colour, so memory is `Limited` where the page's chip
    /// is its own hue.
    constexpr EnumTable<SlotLimit, LimitDress> LimitDressTable { {
        { .limit = SlotLimit::Registered, .chipClass = "chip--registered", .tone = CellTone::Fresh },
        { .limit = SlotLimit::ExternalCpu, .chipClass = "chip--cpu", .tone = CellTone::Limited },
        { .limit = SlotLimit::Memory, .chipClass = "chip--memory", .tone = CellTone::Limited },
        { .limit = SlotLimit::Scratch, .chipClass = "chip--scratch", .tone = CellTone::Alert },
        { .limit = SlotLimit::Cordoned, .chipClass = "chip--cordoned", .tone = CellTone::Limited },
    } };

    static_assert(RowsInEnumeratorOrder(LimitDressTable, &LimitDress::limit),
                  "LimitDressTable must hold one row per SlotLimit, in enumerator order");

    /// The dress for a limit named the way `SlotLimitTable` spells it.
    ///
    /// Resolved through that table rather than by matching strings here, so the
    /// two cannot drift: the cell carries the limit's NAME, and its name is what
    /// the table already owns.
    ///
    /// @param limitName The cell's text.
    /// @return The dress, or null for a name no limit has.
    [[nodiscard]] LimitDress const* LimitDressFor(std::string_view limitName) noexcept
    {
        for (auto const& row: SlotLimitTable)
            if (row.name == limitName)
                return &LimitDressTable[static_cast<std::size_t>(row.limit)];
        return nullptr;
    }

    /// The chip class for a limit named the way `SlotLimitTable` spells it.
    /// @param limitName The cell's text.
    /// @return The chip class; `Registered`'s when nothing matches, which is the
    ///         reading that claims least.
    [[nodiscard]] std::string_view ChipClassFor(std::string_view limitName)
    {
        auto const* const dress = LimitDressFor(limitName);
        return (dress != nullptr ? *dress : LimitDressTable[static_cast<std::size_t>(SlotLimit::Registered)]).chipClass;
    }

    /// How the page spells "nobody reported this".
    ///
    /// The same dash `--cluster-status` prints, deliberately: an operator reading
    /// both surfaces should not have to learn that a blank, a zero and a dash are
    /// the same claim on one and different claims on the other.
    constexpr std::string_view AbsentText = "&ndash;";

    /// One cell, as the page shows it.
    ///
    /// The dash is the spelling `--cluster-status` already uses for "has not said",
    /// kept the same so an operator reading both sees one vocabulary.
    [[nodiscard]] std::string CellAsText(FleetCell const& cell, CellFormat format)
    {
        if (cell.kind == FleetCell::Kind::Absent)
            return std::string { AbsentText };
        if (cell.kind == FleetCell::Kind::Text)
            return EscapeHtml(cell.text);
        if (cell.kind == FleetCell::Kind::Share)
            return EscapeHtml(HumanShareFigure(cell.share));
        // Through the one writer the terminal fleet panel uses too, so a figure reads the same on both.
        return EscapeHtml(HumanFleetFigure(cell.number, format));
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
            case FleetCell::Kind::Share:
                out += WriteMachineFigure(cell.share, FigureFormat::Percent);
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
        // The fraction as measured. `(hits * 1000) / total` in integers is what this was,
        // and it discarded the precision at the ONE place that had it: 3 hits of 4 became
        // `750`, which no reader could turn back into `0.75` without knowing the scale
        // (#1445).
        return FleetCell::OfShare(static_cast<double>(*cache.hits) / static_cast<double>(total));
    }

    // ------------------------------------------------------------ the tables

    /// What a member row shows.
    constexpr std::array<FleetColumn<Cluster::ClusterMember>, 4> MemberColumns {
        FleetColumn<Cluster::ClusterMember> {
            .name = "id",
            .help = "The member's stable identity: what consensus counts.",
            .format = CellFormat::Text,
            .keep = ColumnKeep::Identity,
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
            .project =
                [](Cluster::ClusterMember const& m) {
                    return m.schedulerEndpoint.empty() ? FleetCell::Nothing() : FleetCell::Of(m.schedulerEndpoint);
                } },
        FleetColumn<Cluster::ClusterMember> {
            .name = "scheduler-endpoint-state",
            .help = "Whether that endpoint is announced, never announced, or cleared by a re-admit; it returns when "
                    "the member next leads.",
            .format = CellFormat::Text,
            // Always present, and a column of its own rather than a word in the endpoint
            // cell, so that cell stays an address or absent in the JSON (#1340). Spelled
            // from the table `--cluster-status` and `cluster-members` read too.
            .project =
                [](Cluster::ClusterMember const& m) {
                    return FleetCell::Of(std::string { Cluster::SchedulerEndpointStateName(m) });
                } },
    };

    /// What a forgotten-client row shows (#1471).
    ///
    /// One column, because a tombstone IS a host: `ClusterState::forgotten` records the set and
    /// not when each entry joined it, so a date column could only be invented. The section earns
    /// its place from the other direction -- `node` reports how many tombstones a node enforces
    /// and never which, so an operator who has issued three forgets cannot tell which machine a
    /// given node is refusing, and a count that disagrees between two nodes says only that they
    /// disagree.
    constexpr std::array<FleetColumn<std::string>, 1> ForgottenColumns {
        FleetColumn<std::string> { .name = "host",
                                   .help = "A client host `--cluster-forget-client` removed. It stays refused however many "
                                           "admission routes name it.",
                                   .format = CellFormat::Text,
                                   .keep = ColumnKeep::Identity,
                                   .project = [](std::string const& host) { return FleetCell::Of(host); } },
    };

    /// One row of the conditions section: a machine, and one condition it reported (#1364).
    ///
    /// **A machine that reported NO list is one row with no condition**, every cell but the
    /// endpoint absent. That is the absent state -- a node too old to carry conditions, or whose
    /// announcements have stopped arriving -- and it must not read as a machine with nothing
    /// raised, which reports every row of its table as `clear` or `not-evaluated`. Collapsing the
    /// two would hide exactly the machines nobody can vouch for.
    struct MachineCondition
    {
        std::string_view endpoint;                              ///< The machine; borrows the snapshot.
        CompileCacheWire::NodeConditionFields const* condition; ///< One of its rows, or null when it sent none.
    };

    /// The section's rows, machine by machine, each machine's rows in the order it sent them.
    /// @param snapshot What to read; must outlive the rows, which borrow it.
    /// @return The rows.
    [[nodiscard]] std::vector<MachineCondition> ConditionRowsOf(FleetSnapshot const& snapshot)
    {
        std::vector<MachineCondition> rows;
        for (auto const& node: snapshot.nodes)
        {
            if (!node.conditions.has_value())
            {
                rows.push_back(MachineCondition { .endpoint = node.endpoint, .condition = nullptr });
                continue;
            }
            for (auto const& condition: *node.conditions)
                rows.push_back(MachineCondition { .endpoint = node.endpoint, .condition = &condition });
        }
        return rows;
    }

    /// One text field of a condition, or absent for a machine that sent none.
    /// @param row The row.
    /// @param member Which field.
    /// @return The cell.
    [[nodiscard]] FleetCell ConditionText(MachineCondition const& row,
                                          std::string CompileCacheWire::NodeConditionFields::* member)
    {
        if (row.condition == nullptr)
            return FleetCell::Nothing();
        // A detail is legitimately empty on a clear row, and an empty cell there is the reading
        // rather than an absence: the machine sent the row and had nothing to add.
        return FleetCell::Of(row.condition->*member);
    }

    /// What a conditions row shows.
    ///
    /// Every column is TEXT the machine wrote, never looked up: a leader older than a row renders
    /// it exactly as the node sent it, which is the whole reason the row travels as words.
    constexpr std::array<FleetColumn<MachineCondition>, 7> ConditionColumns {
        FleetColumn<MachineCondition> {
            .name = "endpoint",
            .help = "host:port the machine answers on.",
            .format = CellFormat::Text,
            .keep = ColumnKeep::Identity,
            .project = [](MachineCondition const& r) { return FleetCell::Of(std::string { r.endpoint }); } },
        FleetColumn<MachineCondition> {
            .name = "condition",
            .help = "Which condition, by its stable id. Absent on a machine that reports no conditions at all: a "
                    "build older than them, or one whose announcements have stopped arriving.",
            .format = CellFormat::Text,
            // Vital rather than Identity: the ENDPOINT says which machine a line is, and a section has
            // one identity column. This is what the section exists to show, so it goes last of the rest.
            .keep = ColumnKeep::Vital,
            .project =
                [](MachineCondition const& r) { return ConditionText(r, &CompileCacheWire::NodeConditionFields::id); } },
        FleetColumn<MachineCondition> {
            .name = "state",
            .help = "raised: it holds now. clear: checked and benign. not-evaluated: this machine runs nothing that "
                    "could raise it. undecided: nothing evaluated it, which is a node wired wrongly.",
            .format = CellFormat::Text,
            .decor = CellDecor::ConditionState,
            .keep = ColumnKeep::Vital,
            .project =
                [](MachineCondition const& r) { return ConditionText(r, &CompileCacheWire::NodeConditionFields::state); } },
        FleetColumn<MachineCondition> {
            .name = "persistence",
            .help = "latched: fixed for the life of the process, so only a restart on a different build or "
                    "configuration clears it. live: it can clear while the process runs.",
            .format = CellFormat::Text,
            .keep = ColumnKeep::Vital,
            .project =
                [](MachineCondition const& r) {
                    return ConditionText(r, &CompileCacheWire::NodeConditionFields::persistence);
                } },
        FleetColumn<MachineCondition> { .name = "severity",
                                        .help = "How loudly a raised condition asks: notice, warning or alert.",
                                        .format = CellFormat::Text,
                                        .decor = CellDecor::Severity,
                                        .project =
                                            [](MachineCondition const& r) {
                                                return ConditionText(r, &CompileCacheWire::NodeConditionFields::severity);
                                            } },
        FleetColumn<MachineCondition> {
            .name = "detail",
            .help = "What was observed when raised, or why nothing could be when not evaluated.",
            .format = CellFormat::Text,
            .decor = CellDecor::Prose,
            .project =
                [](MachineCondition const& r) { return ConditionText(r, &CompileCacheWire::NodeConditionFields::detail); } },
        FleetColumn<MachineCondition> {
            .name = "remedy",
            .help = "What to do about it, in the machine's own words.",
            .format = CellFormat::Text,
            .decor = CellDecor::Prose,
            .keep = ColumnKeep::Detail,
            .project =
                [](MachineCondition const& r) { return ConditionText(r, &CompileCacheWire::NodeConditionFields::remedy); } },
    };

    /// What a node row shows, before its per-tier cache columns.
    constexpr std::array<FleetColumn<NodeReport>, 12> NodeColumns {
        FleetColumn<NodeReport> { .name = "endpoint",
                                  .help = "host:port the machine answers on.",
                                  .format = CellFormat::Text,
                                  .keep = ColumnKeep::Identity,
                                  .project = [](NodeReport const& n) { return FleetCell::Of(n.endpoint); } },
        FleetColumn<NodeReport> { .name = "name",
                                  .help = "What this machine calls itself, as an operator would recognise it. Advisory: "
                                          "nothing routes, admits or dispatches by it, and it need not be unique. Absent "
                                          "when the node predates the field or would not say.",
                                  .format = CellFormat::Text,
                                  // Second, right after the endpoint it annotates, because that is the pair
                                  // an operator reads together: the address is what a machine ANSWERS on and
                                  // this is what they call it when they go and look at it. Absent rather than
                                  // blank, for the reason `version` gives below -- a node too old to say is
                                  // not a node with nothing interesting about it.
                                  .project =
                                      [](NodeReport const& n) {
                                          return n.displayName.empty() ? FleetCell::Nothing() : FleetCell::Of(n.displayName);
                                      } },
        FleetColumn<NodeReport> {
            .name = "version",
            .help = "Which build of fastcache-compile-node this machine is running. Absent "
                    "when the node predates the field and cannot report one.",
            .format = CellFormat::Text,
            // Detail at a narrow width, whatever it means during an upgrade: a build string is the widest
            // cell on the row, and the page, which draws every column, is where a rollout is read.
            .keep = ColumnKeep::Detail,
            // Absent rather than blank, and the distinction earns its place during
            // the one activity this column exists for: a rolling upgrade. A node
            // too old to report a version is exactly the node an operator is
            // looking for, so it must not render as the emptiest-looking cell in
            // the table -- it renders as the page's dash, like every other thing
            // nobody told us.
            .project =
                [](NodeReport const& n) { return n.version.empty() ? FleetCell::Nothing() : FleetCell::Of(n.version); } },
        FleetColumn<NodeReport> { .name = "toolchains",
                                  .help = "How many toolchains this one machine serves. Each is a separate registry entry.",
                                  .format = CellFormat::Count,
                                  .project = [](NodeReport const& n) { return FleetCell::Of(n.fingerprints.size()); } },
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
                                  .keep = ColumnKeep::Detail,
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
            .keep = ColumnKeep::Detail,
            .project =
                [](NodeReport const& n) {
                    auto const& traits = TraitsFor(n.capacity.nodeClass);
                    auto const reserve = n.capacity.reserveIsExplicit ? n.capacity.reservedCores : traits.reservedCores;
                    return FleetCell::Of(std::format("{} (reserve {})", traits.name, reserve));
                } },
        FleetColumn<NodeReport> { .name = "cpu-busy",
                                  .help = "Host-wide CPU in use, this fleet's work included. Absent when unread.",
                                  .format = CellFormat::Share,
                                  .keep = ColumnKeep::Vital,
                                  // `cpuBusyPermille` stays a per-mille on the WIRE -- it is what a worker
                                  // reports, and #1445 is about what a DOCUMENT publishes. The conversion
                                  // is here, once, where the wire's vocabulary meets the document's.
                                  .project =
                                      [](NodeReport const& n) {
                                          return n.load.cpuBusyPermille.has_value()
                                                     ? FleetCell::OfShare(static_cast<double>(*n.load.cpuBusyPermille)
                                                                          / 1000.0)
                                                     : FleetCell::Nothing();
                                      } },
        FleetColumn<NodeReport> {
            .name = "memory-available",
            .help = "Memory a new compile could get. Absent when unread.",
            .format = CellFormat::Bytes,
            .keep = ColumnKeep::Detail,
            .project = [](NodeReport const& n) { return FleetCell::Maybe(n.load.availableMemoryBytes); } },
        FleetColumn<NodeReport> { .name = "scratch-free",
                                  .help = "Room where compiles run. The limit that most often reaches zero.",
                                  .format = CellFormat::Bytes,
                                  .keep = ColumnKeep::Vital,
                                  .project = [](NodeReport const& n) { return FleetCell::Maybe(n.load.freeScratchBytes); } },
        FleetColumn<NodeReport> { .name = "cache-hit-rate",
                                  .help = "Reads this node's cache served. Absent when it has served none.",
                                  .format = CellFormat::Share,
                                  .project = [](NodeReport const& n) { return HitRateOf(n.load.cache); } },
        FleetColumn<NodeReport> {
            .name = "heartbeat-age",
            .help = "Since this machine last reported. Everything on its row is that old.",
            .format = CellFormat::Millis,
            .decor = CellDecor::Freshness,
            .keep = ColumnKeep::Vital,
            // The column that tells "this cache is empty" from "this node stopped
            // answering an hour ago and these are its last figures" -- which look
            // identical without it, and lead to opposite conclusions.
            .project =
                [](NodeReport const& n) { return FleetCell::Of(static_cast<std::uint64_t>(n.heartbeatAge.count())); } },
    };

    /// What a worker row shows.
    constexpr std::array<FleetColumn<WorkerReport>, 11> WorkerColumns {
        FleetColumn<WorkerReport> { .name = "id",
                                    .help = "The id this leader assigned at registration.",
                                    .format = CellFormat::Text,
                                    .keep = ColumnKeep::Identity,
                                    .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.id); } },
        FleetColumn<WorkerReport> { .name = "toolchain",
                                    .help = "Matched byte-for-byte. A job never crosses fingerprints.",
                                    .format = CellFormat::Text,
                                    .project = [](WorkerReport const& w) { return FleetCell::Of(w.info.fingerprint); } },
        // BESIDE the fingerprint, never instead of it. The digest is what a launcher
        // compares and what decides every match; this decides nothing and exists to be
        // read (#194). One machine with two MSVC toolsets -- what an ordinary Visual
        // Studio update leaves behind -- showed two opaque hashes here and no way to
        // tell which was which, and the digest deliberately stopped being something a
        // person can derive.
        FleetColumn<WorkerReport> {
            .name = "compiler",
            .help = "What this toolchain is, for a reader. Never matched on -- the fingerprint beside it is what "
                    "decides. Absent when the node did not say, which a pinned --toolchain override never does.",
            .format = CellFormat::Text,
            .keep = ColumnKeep::Detail,
            // Absent rather than blank, like `version` above and for the same reason:
            // a node too old to report one, or one whose fingerprint an operator
            // pinned by hand, is exactly the row somebody is looking for -- so it must
            // not render as the emptiest-looking cell in the table.
            .project =
                [](WorkerReport const& w) {
                    return w.info.toolchainLabel.empty() ? FleetCell::Nothing() : FleetCell::Of(w.info.toolchainLabel);
                } },
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
            .keep = ColumnKeep::Vital,
            .project =
                [](WorkerReport const& w) {
                    return FleetCell::Of(AvailableSlots(w.info.capacity, w.info.slots, w.info.load));
                } },
        FleetColumn<WorkerReport> { .name = "limited-by",
                                    .help = "Which ceiling withdrew the difference: the three have opposite fixes.",
                                    .format = CellFormat::Text,
                                    .decor = CellDecor::Limit,
                                    .keep = ColumnKeep::Vital,
                                    .project =
                                        [](WorkerReport const& w) {
                                            auto const ceilings =
                                                SlotCeilingsFor(w.info.capacity, w.info.slots, w.info.load);
                                            return FleetCell::Of(std::string { TraitsFor(ceilings.binding).name });
                                        } },
        FleetColumn<WorkerReport> {
            .name = "heartbeat-age",
            .help = "Since this entry last reported. A worker unheard-from is dropped.",
            .format = CellFormat::Millis,
            .decor = CellDecor::Freshness,
            .keep = ColumnKeep::Vital,
            .project =
                [](WorkerReport const& w) { return FleetCell::Of(static_cast<std::uint64_t>(w.heartbeatAge.count())); } },
        // The two below are one answer in two cells, and neither half means much
        // alone (#1297). A worker whose toolchain the scheduler never selects is
        // healthy from every other angle on this page -- it registered, it
        // heartbeats, its refusal counters are zero -- because nothing ever arrives
        // to be refused, so no counter moves on either machine. "Registered forty
        // minutes, never picked" is the one-line answer, and it needs the age of the
        // registration beside the absence to be worth anything: an empty
        // `last-picked-age` says nothing at all one second after a node comes up.
        FleetColumn<WorkerReport> {
            .name = "registered-age",
            .help = "Since this entry last registered. Read it beside last-picked-age: never picked matters at forty "
                    "minutes and means nothing at one second.",
            .format = CellFormat::Millis,
            .keep = ColumnKeep::Detail,
            .project =
                [](WorkerReport const& w) { return FleetCell::Of(static_cast<std::uint64_t>(w.registeredAge.count())); } },
        FleetColumn<WorkerReport> {
            .name = "last-picked-age",
            .help = "Since the scheduler last chose this entry. Absent means it never has -- which on a fleet that is "
                    "building is a toolchain nothing is reaching.",
            .format = CellFormat::Millis,
            // Absent at the CELL, never flattened to zero. The two are opposite
            // claims here rather than merely different: zero says a job went to this
            // worker a moment ago, which is the healthiest reading on the row, and it
            // would be printed for the worker in the worst state on the page.
            .project =
                [](WorkerReport const& w) {
                    return w.lastPickedAge.has_value() ? FleetCell::Of(static_cast<std::uint64_t>(w.lastPickedAge->count()))
                                                       : FleetCell::Nothing();
                } },
    };

    /// What an outstanding-lease row shows.
    ///
    /// Four columns and deliberately not five: the toolchain is a property of the
    /// WORKER, and the id below joins to the table above, where the fingerprint and
    /// the compiler already are. A column repeating it here would be a second place
    /// for the same fact to be right.
    constexpr std::array<FleetColumn<LeaseHolding>, 4> LeaseColumns {
        // The key earns its place despite being a digest, and it is the only column
        // that answers the question an operator arrives with. A client refused
        // `already-in-flight` was refused ON a key, and `fastcache-cc` prints that
        // same key when it dispatches -- so this is what joins a stuck build to
        // whoever is holding it.
        FleetColumn<LeaseHolding> { .name = "key",
                                    .help = "The object key being compiled. What an already-in-flight refusal named, "
                                            "and what the launcher logs as key=.",
                                    .format = CellFormat::Text,
                                    .keep = ColumnKeep::Identity,
                                    .project = [](LeaseHolding const& l) { return FleetCell::Of(l.key); } },
        FleetColumn<LeaseHolding> { .name = "worker",
                                    .help = "The worker it was leased to; its row is in the table above.",
                                    .format = CellFormat::Text,
                                    .project = [](LeaseHolding const& l) { return FleetCell::Of(l.workerId); } },
        FleetColumn<LeaseHolding> {
            .name = "endpoint",
            .help = "host:port that worker answers on. Absent when it is no longer registered, which is the "
                    "answer rather than a missing cell.",
            .format = CellFormat::Text,
            .keep = ColumnKeep::Detail,
            .project =
                [](LeaseHolding const& l) {
                    return l.workerEndpoint.empty() ? FleetCell::Nothing() : FleetCell::Of(l.workerEndpoint);
                } },
        FleetColumn<LeaseHolding> {
            .name = "age",
            .help = "Since the lease was taken. A client resolves its own lease when the job ends, so an old one "
                    "is a client that died mid-build whose worker is still answering.",
            .format = CellFormat::Millis,
            .decor = CellDecor::LeaseAge,
            .keep = ColumnKeep::Vital,
            .project = [](LeaseHolding const& l) { return FleetCell::Of(static_cast<std::uint64_t>(l.age.count())); } },
    };

    /// What the lease section says about its own completeness.
    ///
    /// The truncation has to be legible at the section, not discovered by counting
    /// rows against a tile elsewhere on the page: a reader who takes fifty rows for
    /// the whole fleet's work draws the wrong conclusion from a correct table.
    /// @param snapshot The report.
    /// @return The header's meta line.
    [[nodiscard]] std::string OutstandingLeaseMeta(FleetSnapshot const& snapshot)
    {
        if (snapshot.outstandingLeases.size() < snapshot.liveLeases)
            return std::format("the {} oldest of {}", snapshot.outstandingLeases.size(), snapshot.liveLeases);
        return "oldest first";
    }

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

    constexpr std::array<TierColumn, 5> TierColumns {
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
        // The column #175 exists to produce. Without it a memory-bound disk-cache node
        // showed `Memory` as its binding limit and nothing anywhere said the cache's
        // own key index was what consumed the memory -- the next question an operator
        // asks, and the one the page could not answer.
        //
        // Bytes, and NOT comparable with `budget` beside it: that one is denominated
        // in whatever the tier is bounded by, which for a disk tier is bytes on a
        // filesystem, while this is always RAM. The help text has to say so, because
        // two byte columns side by side otherwise invite the sum nobody should take.
        TierColumn { .suffix = "index-ram",
                     .help = "RAM this tier spends on its key index. Always memory, even for a disk tier, so it is "
                             "not comparable with the budget beside it.",
                     .format = CellFormat::Bytes,
                     .project =
                         [](NodeReport const& n, StorageTier tier) {
                             auto const& usage = n.load.cache.tiers[static_cast<std::size_t>(tier)];
                             // Zero is rendered rather than suppressed: an in-memory
                             // tier genuinely spends none SEPARATELY -- its index is
                             // inside the bytes it already reports -- and a blank
                             // would read as "did not say" instead.
                             return usage.has_value() ? FleetCell::Of(usage->indexBytes) : FleetCell::Nothing();
                         } },
    };

    /// What joins a tier's name to a tier column's suffix.
    constexpr std::string_view TierColumnSeparator = "-";

    /// The name a tier column carries, e.g. `memory-items`.
    [[nodiscard]] std::string TierColumnName(StorageTier tier, std::string_view suffix)
    {
        return std::format("{}{}{}", StorageTierTable[static_cast<std::size_t>(tier)].name, TierColumnSeparator, suffix);
    }

    /// What the tier table's first column is called.
    ///
    /// The tier section is the one whose columns are composed rather than tabled, so
    /// its leading column is the only name here with no row to come from. Spelled once:
    /// it was a literal in the three renderers, and `FleetColumnNames` would have made
    /// it a fourth -- at which point the name a caller is told to expect and the name a
    /// renderer emits are different strings that happen to agree.
    constexpr std::string_view TierEndpointColumn = "endpoint";

    /// Whether every name @p columns gives a program is kebab-case.
    /// @param columns A column table.
    /// @return True when each `name` is.
    template <typename Subject, std::size_t N>
    [[nodiscard]] constexpr bool ColumnNamesAreKebab(std::array<FleetColumn<Subject>, N> const& columns) noexcept
    {
        return std::ranges::all_of(columns, [](FleetColumn<Subject> const& column) { return IsKebabName(column.name); });
    }

    /// Whether every name the fleet documents' tables give a program is spelled the one way a program reads a
    /// name: kebab-case, through the predicate the CLI's piped keys are held to as well (#1445) -- the column
    /// tables, a tier column as it is JOINED from its tier and suffix, the section keys, the lease outcomes and the
    /// series. The headline figures' keys are asserted beside `KpiTable` and the bucket arrays' beside theirs, where
    /// those tables are; `FleetMachineNameTables` is the run-time walk over all of them.
    /// @return True when every name is.
    [[nodiscard]] consteval bool FleetNamesAreKebab() noexcept
    {
        auto whole = ColumnNamesAreKebab(MemberColumns) && ColumnNamesAreKebab(NodeColumns)
                     && ColumnNamesAreKebab(WorkerColumns) && ColumnNamesAreKebab(LeaseColumns)
                     && ColumnNamesAreKebab(ConditionColumns) && IsKebabName(TierEndpointColumn);
        for (auto const& tier: StorageTierTable)
            for (auto const& column: TierColumns)
                whole = whole && IsKebabJoin({ tier.name, TierColumnSeparator, column.suffix });
        whole =
            whole && std::ranges::all_of(FleetSectionTable, [](FleetSectionRow const& row) { return IsKebabName(row.key); });
        whole =
            whole && std::ranges::all_of(LeaseOutcomeTable, [](LeaseOutcomeRow const& row) { return IsKebabName(row.key); });
        whole =
            whole && std::ranges::all_of(FleetSeriesTable, [](FleetSeriesRow const& row) { return IsKebabName(row.key); });
        return whole;
    }
    static_assert(FleetNamesAreKebab(), "every name a fleet document gives a program must be kebab-case");

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

    /// Whether a snapshot carries lease figures at all.
    ///
    /// Separate from `CountAt` because the two questions have different right
    /// answers. Rendering the outcome list wants a zero for a count nobody
    /// supplied — a row has to print something. Deciding whether this fleet has
    /// ever been dispatched to must not read that same absence as a zero, or a
    /// caller who said nothing gets told something. One predicate, so the two
    /// policies are a visible choice rather than two bounds checks that drifted.
    /// @param snapshot What to read.
    /// @return True when every `LeaseOutcomeTable` row has a count.
    [[nodiscard]] bool HasLeaseFigures(FleetSnapshot const& snapshot) noexcept
    {
        return snapshot.leases.size() == LeaseOutcomeTable.size();
    }

    /// Where `DispatchLeasesGranted` sits in `LeaseOutcomeTable`.
    ///
    /// Derived from the table by enumerator rather than written as `0`.
    /// `FleetSnapshot::leases` is indexed by that position, so a literal would be a
    /// second place the row order is recorded — and it would reorder in silence.
    constexpr std::size_t GrantedLeaseIndex = static_cast<std::size_t>(std::ranges::distance(
        LeaseOutcomeTable.begin(),
        std::ranges::find(LeaseOutcomeTable, IMetricsSink::Counter::DispatchLeasesGranted, &LeaseOutcomeRow::counter)));
    static_assert(GrantedLeaseIndex < LeaseOutcomeTable.size(), "a grant must be a row of LeaseOutcomeTable");

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
        // `NodeLoad::inFlight` on a report is whatever the CONTRIBUTING ENTRY last
        // carried, while `fleetJobsInFlight` is folded across every entry of the
        // machine. Both are machine-wide quantities -- a node samples one
        // `WorkerServer::InFlight()` for all its toolchains -- so this is not two
        // grains being reconciled; it is one figure taken from the whole machine
        // rather than from whichever entry happened to contribute the rest of the
        // load. A ceiling built from one entry's copy and a subtraction using the
        // fold would still disagree the moment a sibling registered mid-flight.
        auto machineLoad = node.load;
        machineLoad.inFlight = node.fleetJobsInFlight;

        // A machine that runs no worker contributes to NEITHER total and gets no ceiling
        // computed: it offers nothing, so counting it as zero registered and zero free is
        // arithmetically identical and semantically a claim -- it would put a machine in the
        // denominator of *how much of this fleet is busy* that was never able to take work
        // (#1440). Its in-flight count is still read, because a machine with no worker of THIS
        // fleet can still be running jobs nobody here granted, and that is what the external
        // ceiling is about.
        totals.inFlight += node.fleetJobsInFlight;
        if (!node.registeredSlots.has_value())
            continue;
        auto const registered = *node.registeredSlots;

        auto const ceilings = SlotCeilingsFor(node.capacity, registered, machineLoad);
        totals.registered += registered;

        // A ceiling is the total a machine supports with its RUNNING jobs
        // INCLUDED -- `Detail::CeilingFrom` says so in as many words -- so adding
        // it straight into `free` counts this fleet's own work twice: once as
        // in-flight and once as room to start more. A full 8-slot machine then
        // rendered "8 compiling, 8 free" out of 8 registered, and the legend
        // offered a compile a start on a machine that could take none.
        //
        // `WorkerRegistry::FreeSlots` is the definition this has to match, and it
        // subtracts before calling anything free. Two answers to "what is free"
        // is how a page comes to disagree with the scheduler it is describing.
        auto const ceiling = std::min(ceilings.available, registered);
        totals.free += node.fleetJobsInFlight >= ceiling ? 0U : ceiling - node.fleetJobsInFlight;
    }

    // Saturating, not wrapping. These are unsigned and the three parts are read
    // from a heartbeat that a machine may have sent at different moments, so a
    // ceiling can legitimately exceed what is left after in-flight work. A
    // wrapped `withheld` would draw a bar four billion slots wide.
    auto const used = totals.inFlight + totals.free;
    totals.withheld = used >= totals.registered ? 0U : totals.registered - used;
    return totals;
}

std::optional<ToolchainPickCoverage> PickCoverageFor(FleetSnapshot const& snapshot)
{
    // Absent, not zero, and this is the guard the header argues for: `0 never
    // picked` on an empty fleet is the healthiest reading on the strip printed for
    // the least healthy fleet there is. Zero has to go on meaning "every toolchain
    // is being reached", which an empty fleet cannot claim.
    if (snapshot.workers.empty())
        return std::nullopt;

    // Folded over a SET of fingerprints rather than over the rows: this registry
    // keys `(fingerprint, endpoint)`, so one toolchain served by four machines is
    // four rows, and counting rows would report one unreached toolchain as four.
    // Views into `snapshot.workers`, which outlives this call by construction --
    // the caller holds the snapshot.
    std::unordered_map<std::string_view, bool> reached;
    for (auto const& worker: snapshot.workers)
    {
        // OR across the siblings, never the last one's answer: a toolchain is being
        // reached if ANY machine serving it has been chosen. Overwriting per row
        // would make the verdict depend on which entry the map happened to visit
        // last, so a two-machine toolchain would flicker between reached and not
        // with nothing about the fleet changing.
        auto const inserted = reached.try_emplace(worker.info.fingerprint, false).first;
        inserted->second = inserted->second || worker.lastPickedAge.has_value();
    }

    auto const never = std::ranges::count_if(reached, [](auto const& entry) { return !entry.second; });
    return ToolchainPickCoverage { .toolchains = reached.size(), .neverPicked = static_cast<std::size_t>(never) };
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

    // Both halves from ONE call, so the count and the listing cannot disagree about
    // how much was left out -- see `LeaseListing`.
    auto const held = sources.scheduler->OutstandingLeases(OutstandingLeaseRows);
    snapshot.liveLeases = held.total;

    // Joined against `snapshot.workers`, which is already in hand -- so the address
    // an operator needs costs no second walk of the registry and no lookup API that
    // exists for one caller. A worker that has gone since the lease was taken leaves
    // the endpoint empty, and that absence is the diagnosis rather than a hole.
    snapshot.outstandingLeases.reserve(held.oldest.size());
    for (auto const& lease: held.oldest)
    {
        // The projection returns a REFERENCE. Returning `std::string` by value
        // constructs one per comparison, which on a fleet of a few hundred workers
        // is thousands of allocations per page request for a lookup that reads a
        // name and discards it.
        auto const holder = std::ranges::find(
            snapshot.workers, lease.workerId, [](WorkerReport const& w) -> std::string const& { return w.info.id; });
        snapshot.outstandingLeases.push_back(
            LeaseHolding { .key = lease.key,
                           .workerId = lease.workerId,
                           .workerEndpoint = holder != snapshot.workers.end() ? holder->info.endpoint : std::string {},
                           .age = lease.age });
    }

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
    /// What an absent cell reads as in the text rendering.
    ///
    /// Not an empty field, which is the tempting spelling and the one that destroys
    /// the distinction: a blank cell and a value that genuinely IS the empty string
    /// would render alike, and *absent is not zero* means nothing once a reader
    /// cannot tell them apart. This is the token `fastcache-cli`'s own human format
    /// already uses, so a reader who learned one learned the other.
    constexpr std::string_view TextAbsent = "-";

    /// One cell, as the text rendering carries it.
    ///
    /// The RAW number, exactly as `AppendCellAsJson` writes it and never as the page
    /// humanises it: this document is read by `awk` and `cut`, and `12.4 GiB` would
    /// have to be parsed back before it could be compared or summed.
    /// @param out Appended to.
    /// @param cell The cell.
    void AppendCellAsText(std::string& out, FleetCell const& cell)
    {
        switch (cell.kind)
        {
            case FleetCell::Kind::Absent:
                out += TextAbsent;
                break;
            case FleetCell::Kind::Number:
                out += std::format("{}", cell.number);
                break;
            case FleetCell::Kind::Share:
                out += WriteMachineFigure(cell.share, FigureFormat::Percent);
                break;
            case FleetCell::Kind::Text:
                out += EscapeDelimited(cell.text);
                break;
        }
    }

    /// Append one TSV table -- a header line, then one line per subject.
    ///
    /// The same walk as `AppendJsonRows` and `AppendHtmlRows` over the same table, so
    /// the three cannot name different columns. The header is emitted even when there
    /// are no subjects: a reader piping this still needs to know the shape, and an
    /// empty document would not say whether the fleet is empty or the section is
    /// unknown.
    template <typename Subject, std::size_t N>
    void AppendTextRows(std::string& out,
                        std::array<FleetColumn<Subject>, N> const& columns,
                        std::vector<Subject> const& subjects)
    {
        bool firstCell = true;
        for (auto const& column: columns)
        {
            if (!std::exchange(firstCell, false))
                out += '\t';
            out += EscapeDelimited(column.name);
        }
        out += '\n';
        for (auto const& subject: subjects)
        {
            firstCell = true;
            for (auto const& column: columns)
            {
                if (!std::exchange(firstCell, false))
                    out += '\t';
                AppendCellAsText(out, column.project(subject));
            }
            out += '\n';
        }
    }

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

    /// When a heartbeat age stops being one a reader should trust the row behind.
    constexpr std::uint64_t HeartbeatStaleAfterMillis = 15'000;

    /// When an outstanding lease has been outstanding long enough to look at.
    ///
    /// Half the lease lifetime: past it, a lease is closer to expiring than to
    /// having been taken. Derived from `DefaultLeaseTimeout` rather than typed as a
    /// number, so it stays half of whatever that becomes -- a threshold that
    /// silently stopped tracking the timeout would colour rows by nothing.
    constexpr std::uint64_t LeaseOldAfterMillis = static_cast<std::uint64_t>(LeaseTable::DefaultLeaseTimeout.count()) / 2;

    /// The tone @p decor gives a present value of @p number.
    ///
    /// **The one place an age threshold is applied**, for the page's pill and for every other human
    /// surface through `FleetCellTone`. The two age decors differ by their threshold and by nothing
    /// else, so they are one comparison.
    /// @param decor The column's decoration.
    /// @param number The cell's integer.
    /// @return `Stale` past the decor's threshold, `Fresh` inside it, `Plain` for a decor with none.
    [[nodiscard]] constexpr CellTone ToneOf(CellDecor decor, std::uint64_t number) noexcept
    {
        auto const agedPast = [number](std::uint64_t staleAfter) {
            return number >= staleAfter ? CellTone::Stale : CellTone::Fresh;
        };
        switch (decor)
        {
            case CellDecor::Plain:
            case CellDecor::Limit:
            case CellDecor::Severity:
            case CellDecor::ConditionState:
            case CellDecor::Prose:
                return CellTone::Plain;
            case CellDecor::Freshness:
                return agedPast(HeartbeatStaleAfterMillis);
            case CellDecor::LeaseAge:
                return agedPast(LeaseOldAfterMillis);
        }
        return CellTone::Plain;
    }

    /// How a human surface dresses one condition severity: the page's chip and a terminal's tone.
    struct SeverityDress
    {
        CompileCacheWire::ConditionSeverity severity; ///< The enumerator this row describes.
        std::string_view chipClass;                   ///< The page's chip.
        CellTone tone;                                ///< The tone a terminal gives it.
    };

    /// One row per severity, in enumerator order: one dress for the page and every terminal, so a
    /// warning cannot be amber on one screen and red on another.
    constexpr EnumTable<CompileCacheWire::ConditionSeverity, SeverityDress> SeverityDressTable { {
        { .severity = CompileCacheWire::ConditionSeverity::Notice, .chipClass = "chip--notice", .tone = CellTone::Plain },
        { .severity = CompileCacheWire::ConditionSeverity::Warning,
          .chipClass = "chip--warning",
          .tone = CellTone::Limited },
        { .severity = CompileCacheWire::ConditionSeverity::Alert, .chipClass = "chip--alert", .tone = CellTone::Alert },
    } };
    static_assert(RowsInEnumeratorOrder(SeverityDressTable, &SeverityDress::severity),
                  "SeverityDressTable must hold one row per ConditionSeverity, in enumerator order");

    /// The tone @p decor gives the text @p text, for the decorations that judge a word.
    ///
    /// A word this build cannot name is plain rather than a guess -- except a condition STATE,
    /// which `AsksForAttention` already says is worth a look when unknown: a newer node's word for
    /// a state may be a raised one, and the reassuring default is the wrong one.
    /// @param decor The column's decoration.
    /// @param text The cell's text.
    /// @return The tone.
    [[nodiscard]] CellTone TextToneOf(CellDecor decor, std::string_view text) noexcept
    {
        switch (decor)
        {
            case CellDecor::Limit: {
                auto const* const dress = LimitDressFor(text);
                return dress != nullptr ? dress->tone : CellTone::Plain;
            }
            case CellDecor::Severity: {
                auto const severity = CompileCacheWire::ConditionSeverityNamed(text);
                return severity.has_value() ? SeverityDressTable[static_cast<std::size_t>(*severity)].tone : CellTone::Plain;
            }
            case CellDecor::ConditionState:
                return CompileCacheWire::StateAsksForAttention(text) ? CellTone::Stale : CellTone::Plain;
            case CellDecor::Plain:
            case CellDecor::Freshness:
            case CellDecor::LeaseAge:
            case CellDecor::Prose:
                return CellTone::Plain;
        }
        return CellTone::Plain;
    }

    /// An age, as a pill that goes amber once its tone is `Stale`.
    /// @param text The already-formatted value.
    /// @param tone What `ToneOf` decided.
    /// @return The cell's inner HTML.
    [[nodiscard]] std::string AgePill(std::string_view text, CellTone tone)
    {
        auto const* const pill = tone == CellTone::Stale ? "pill--warn" : "pill--ok";
        return std::format(R"(<span class="pill pill--value {}"><span class="dot"></span>{}</span>)", pill, text);
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
            case CellDecor::Limit:
                return std::format(R"(<span class="chip {}">{}</span>)", ChipClassFor(cell.text), text);
            case CellDecor::Freshness:
                // Amber past the point where a reader should stop trusting the
                // rest of the row. Everything on it is as old as this number.
            case CellDecor::LeaseAge:
                return AgePill(text, ToneOf(column.decor, cell.number));
            case CellDecor::Severity: {
                auto const severity = CompileCacheWire::ConditionSeverityNamed(cell.text);
                // A severity this build cannot name gets the plain chip: the word is shown as sent,
                // and a colour would be a guess about how loud it is.
                auto const chip =
                    severity.has_value() ? SeverityDressTable[static_cast<std::size_t>(*severity)].chipClass : "";
                return std::format(R"(<span class="chip {}">{}</span>)", chip, text);
            }
            case CellDecor::ConditionState:
                return TextToneOf(column.decor, cell.text) == CellTone::Stale
                           ? std::format(R"(<span class="chip chip--raised">{}</span>)", text)
                           : text;
            case CellDecor::Prose:
                return std::format(R"(<span class="prose">{}</span>)", text);
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

std::string RenderFleetJson(FleetSnapshot const& snapshot, FleetHistoryView const& history)
{
    std::string out;
    out.reserve(4096);
    out += '{';

    // FIRST, so a reader that streams can decide what it is reading before it reaches a
    // value whose meaning the generation settles.
    AppendJsonString(out, "schema");
    out += std::format(":{},", FleetJsonSchema);

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

    // The headline figures, keyed by the name each `KpiTable` row carries (#1302).
    //
    // An OBJECT rather than an array, because these are one value per named figure
    // rather than rows of one shape: `.kpi["cache-hit-rate"].value` is what a scraper
    // wants, and a list would make it search for the element whose key matches.
    //
    // What travels is the NUMBER and its scale, never the page's `unit` and `sub`. A
    // consumer handed `"12"` and `"/ 32 slots"` has to parse the figure back out of a
    // label -- a receiver recomputing by string-scraping, in a vocabulary this
    // document does not otherwise speak. `of` is the denominator as its own number for
    // the two tiles that have one, and `null` for the five that do not.
    //
    // This is the one-spelling rule, not a deviation from *nothing a receiver can
    // recompute travels*: what must not be re-implemented is the DERIVATION -- the
    // hit-rate arithmetic, the oldest-heartbeat fold, which a re-deriver reaching for
    // a mean gets plausibly and differently wrong -- rather than the arithmetic being
    // hidden. One derivation, three renderings.
    out += ',';
    AppendKpiJson(out, snapshot, history);

    out += ',';
    AppendJsonString(out, "members");
    out += ':';
    if (snapshot.cluster.has_value())
        AppendJsonRows(out, MemberColumns, snapshot.cluster->members);
    else
        out += "null"; // Runs no cluster at all -- not "a cluster with no members".

    // Every row every machine reported, and one row with a null condition for a machine that
    // reported none (#1364) -- so *absent* and *nothing raised* are two shapes a consumer can
    // tell apart without knowing which states are quiet.
    out += ',';
    AppendJsonString(out, "conditions");
    out += ':';
    AppendJsonRows(out, ConditionColumns, ConditionRowsOf(snapshot));

    out += ',';
    AppendJsonString(out, "forgotten");
    out += ':';
    if (snapshot.cluster.has_value())
        AppendJsonRows(out, ForgottenColumns, snapshot.cluster->forgotten);
    else
        out += "null"; // Runs no cluster -- not "a cluster that has forgotten nobody".

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
        AppendJsonString(out, TierEndpointColumn);
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

    // A separate key from the count above rather than a replacement for it, and its
    // name says what it is: the OLDEST of them, because a busy fleet holds thousands
    // and a document that grew without limit would be one nothing could consume. A
    // reader comparing the two sees the truncation instead of having to know about
    // it.
    out += ',';
    AppendJsonString(out, "leases-outstanding-oldest");
    out += ':';
    AppendJsonRows(out, LeaseColumns, snapshot.outstandingLeases);
    out += ',';
    AppendJsonString(out, "registrations");
    out += std::format(":{}", snapshot.registrations);

    out += '}';
    return out;
}

std::optional<FleetSection> FleetSectionFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetSectionTable)
        if (row.key == key)
            return row.section;
    return std::nullopt;
}

namespace
{
    /// Append the tiers table: the endpoint, then every column of every tier some
    /// member runs.
    ///
    /// Its own walk rather than `AppendTextRows`, because this table is not a
    /// `FleetColumn` one -- the page builds it the same way, from `StorageTierTable`
    /// crossed with `TierColumns`, and a tier no member runs contributes no column at
    /// all rather than a column of absences. That is "absent is not zero" at COLUMN
    /// granularity, and it has to survive into this rendering or a reader here would
    /// see an empty disk tier where the page correctly shows none.
    /// @param out Appended to.
    /// @param snapshot What to render.
    void AppendTierText(std::string& out, FleetSnapshot const& snapshot)
    {
        out += TierEndpointColumn;
        for (auto const& tier: StorageTierTable)
        {
            if (!snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)])
                continue;
            for (auto const& column: TierColumns)
                out += std::format("\t{}", EscapeDelimited(TierColumnName(tier.tier, column.suffix)));
        }
        out += '\n';

        for (auto const& node: snapshot.nodes)
        {
            out += EscapeDelimited(node.endpoint);
            for (auto const& tier: StorageTierTable)
            {
                if (!snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)])
                    continue;
                for (auto const& column: TierColumns)
                {
                    out += '\t';
                    AppendCellAsText(out, column.project(node, tier.tier));
                }
            }
            out += '\n';
        }
    }

    /// The `kpi` section's column names, which are the only ones not from a table.
    ///
    /// The headline figures are one value per NAME rather than rows of one shape, so
    /// the text form is the key/value table that shape implies. Spelled once and read
    /// by both the header row and `FleetColumnNames`, so what a reader is told to
    /// expect and what the renderer emits cannot part company.
    constexpr std::array<std::string_view, 4> KpiTextColumns { "kpi", "value", "unit", "of" };

    /// Append one named section's table.
    ///
    /// A `switch` over the enum with no default arm, so a section added to
    /// `FleetSectionTable` is a BUILD failure here rather than a key the route
    /// accepts and this renders as nothing. A default would make the new section
    /// answer an empty document, which reads exactly like a fleet with nothing in it.
    /// @param out Appended to.
    /// @param snapshot What to render.
    /// @param history The window the `kpi` and `series` sections answer for.
    /// @param section Which table.
    void AppendSectionText(std::string& out,
                           FleetSnapshot const& snapshot,
                           FleetHistoryView const& history,
                           FleetSection section)
    {
        switch (section)
        {
            case FleetSection::Kpi:
                AppendKpiText(out, snapshot, history);
                return;
            case FleetSection::Machines:
                AppendTextRows(out, NodeColumns, snapshot.nodes);
                return;
            case FleetSection::Workers:
                AppendTextRows(out, WorkerColumns, snapshot.workers);
                return;
            case FleetSection::Leases:
                AppendTextRows(out, LeaseColumns, snapshot.outstandingLeases);
                return;
            case FleetSection::Members:
                // An empty vector where the node runs no cluster at all, which is a
                // different fact -- but one the JSON spells `null` and a TSV table has
                // no room for, so the marker line in the full document is what carries
                // it: a reader sees the section, its header, and no rows.
                AppendTextRows(out,
                               MemberColumns,
                               snapshot.cluster.has_value() ? snapshot.cluster->members
                                                            : std::vector<Cluster::ClusterMember> {});
                return;
            case FleetSection::Forgotten:
                // Empty where the node runs no cluster, exactly as `Members` is: the marker
                // line in the full document carries that difference, a TSV table has no room
                // for it, and an empty table is the honest rendering of *nothing to show*.
                AppendTextRows(out,
                               ForgottenColumns,
                               snapshot.cluster.has_value() ? snapshot.cluster->forgotten : std::vector<std::string> {});
                return;
            case FleetSection::Conditions:
                AppendTextRows(out, ConditionColumns, ConditionRowsOf(snapshot));
                return;
            case FleetSection::Tiers:
                AppendTierText(out, snapshot);
                return;
            case FleetSection::Series:
                AppendSeriesText(out, history.buckets, history.range);
                return;
            case FleetSection::Last:
                break;
        }
    }
} // namespace

std::vector<std::string> FleetColumnNames(FleetSection section, FleetSnapshot const& snapshot)
{
    auto const namesOf = [](auto const& columns) {
        std::vector<std::string> names;
        names.reserve(columns.size());
        for (auto const& column: columns)
            names.emplace_back(column.name);
        return names;
    };

    // The same `switch` shape as `AppendSectionText`, with no default arm and the same
    // table per section. A section whose names came from one table while its rows came
    // from another would report perfect coverage of a set nothing renders, which is the
    // defect this function exists to remove rather than relocate.
    switch (section)
    {
        case FleetSection::Kpi:
            // A key/value table rather than rows of one shape, so its four columns are
            // its own rather than a `FleetColumn` set -- and its `tabular` column says
            // so, which is what lets the coverage case assert that in both directions
            // instead of carrying a silent exception.
            return { KpiTextColumns.begin(), KpiTextColumns.end() };
        case FleetSection::Machines:
            return namesOf(NodeColumns);
        case FleetSection::Workers:
            return namesOf(WorkerColumns);
        case FleetSection::Leases:
            return namesOf(LeaseColumns);
        case FleetSection::Members:
            return namesOf(MemberColumns);
        case FleetSection::Forgotten:
            return namesOf(ForgottenColumns);
        case FleetSection::Conditions:
            return namesOf(ConditionColumns);
        case FleetSection::Tiers: {
            // Laid down exactly as `AppendTierText` lays them: the endpoint column, then
            // every present tier crossed with every suffix. `tiersPresent` is what makes
            // this snapshot-dependent -- a tier no member runs gets no column, so naming
            // one here would tell a caller to look for a column that is correctly absent.
            std::vector<std::string> names { std::string { TierEndpointColumn } };
            for (auto const& tier: StorageTierTable)
            {
                if (!snapshot.tiersPresent[static_cast<std::size_t>(tier.tier)])
                    continue;
                for (auto const& column: TierColumns)
                    names.push_back(TierColumnName(tier.tier, column.suffix));
            }
            return names;
        }
        case FleetSection::Series:
            // The bucket arrays and the series, from the tables `AppendSeriesText` walks; not a
            // `FleetColumn` set, and `tabular` says so.
            return FleetSeriesColumnNames();
        case FleetSection::Last:
            break;
    }
    return {};
}

std::optional<CellFormat> CellFormatFromName(std::string_view name) noexcept
{
    auto const* row =
        FindIfOrNull(CellFormatTable, [name](CellFormatRow const& candidate) { return candidate.name == name; });
    return row == nullptr ? std::nullopt : std::optional<CellFormat> { row->format };
}

namespace
{
    /// What a human surface other than the page reads about one column, whichever table holds it.
    struct ColumnFacts
    {
        CellFormat format; ///< Its scale.
        CellDecor decor;   ///< Its decoration.
        ColumnKeep keep;   ///< How long it is kept for width.
    };

    /// The facts of the column @p name in @p section.
    ///
    /// **The one walk behind `FleetColumnFormat`, `FleetColumnKeep` and `FleetCellTone`**, so the three
    /// answer for one column set: the tables `FleetColumnNames` reads, section for section, with no
    /// default arm for its reason.
    /// @param section The section the header belongs to.
    /// @param name The column's name.
    /// @return The facts, or absent for a name the section does not render.
    [[nodiscard]] std::optional<ColumnFacts> FactsOf(FleetSection section, std::string_view name)
    {
        auto const factsOf = [name](auto const& columns) -> std::optional<ColumnFacts> {
            for (auto const& column: columns)
                if (column.name == name)
                    return ColumnFacts { .format = column.format, .decor = column.decor, .keep = column.keep };
            return std::nullopt;
        };

        switch (section)
        {
            case FleetSection::Kpi:
                // Each row names its own unit, so no column of this section has one scale.
                return std::nullopt;
            case FleetSection::Machines:
                return factsOf(NodeColumns);
            case FleetSection::Workers:
                return factsOf(WorkerColumns);
            case FleetSection::Leases:
                return factsOf(LeaseColumns);
            case FleetSection::Members:
                return factsOf(MemberColumns);
            case FleetSection::Forgotten:
                return factsOf(ForgottenColumns);
            case FleetSection::Conditions:
                return factsOf(ConditionColumns);
            case FleetSection::Tiers:
                if (name == TierEndpointColumn)
                    return ColumnFacts { .format = CellFormat::Text,
                                         .decor = CellDecor::Plain,
                                         .keep = ColumnKeep::Identity };
                // Every tier crossed with every suffix, composed through `TierColumnName` exactly as the
                // renderers compose it, so there is no second spelling of a tier column to parse.
                for (auto const& tier: StorageTierTable)
                    for (auto const& column: TierColumns)
                        if (TierColumnName(tier.tier, column.suffix) == name)
                            return ColumnFacts { .format = column.format,
                                                 .decor = CellDecor::Plain,
                                                 .keep = ColumnKeep::Useful };
                return std::nullopt;
            case FleetSection::Series:
                // Raw per-bucket values: a rate is per minute, a share a fraction, a level a count, so no
                // column of this section has one scale a human surface could apply.
                return std::nullopt;
            case FleetSection::Last:
                break;
        }
        return std::nullopt;
    }
} // namespace

std::optional<FleetRowUnitColumns> FleetRowUnitColumnsFor(FleetSection section) noexcept
{
    // Read out of `KpiTextColumns` rather than spelled again, so the names a renderer looks for
    // and the names the document carries cannot part company -- which is the reason that array
    // exists at all.
    if (section != FleetSection::Kpi)
        return std::nullopt;
    return FleetRowUnitColumns { .value = KpiTextColumns[1], .unit = KpiTextColumns[2] };
}

std::optional<CellFormat> FleetColumnFormat(FleetSection section, std::string_view name)
{
    return FactsOf(section, name).transform([](ColumnFacts const& facts) -> CellFormat { return facts.format; });
}

std::optional<ColumnKeep> FleetColumnKeep(FleetSection section, std::string_view name)
{
    return FactsOf(section, name).transform([](ColumnFacts const& facts) -> ColumnKeep { return facts.keep; });
}

CellTone FleetCellTone(FleetSection section, std::string_view name, std::uint64_t number)
{
    auto const facts = FactsOf(section, name);
    return facts.has_value() ? ToneOf(facts->decor, number) : CellTone::Plain;
}

CellTone SlotLimitTone(SlotLimit limit) noexcept
{
    return limit < SlotLimit::Last ? LimitDressTable[static_cast<std::size_t>(limit)].tone : CellTone::Plain;
}

CellTone FleetCellTone(FleetSection section, std::string_view name, std::string_view text)
{
    auto const facts = FactsOf(section, name);
    return facts.has_value() ? TextToneOf(facts->decor, text) : CellTone::Plain;
}

std::optional<std::string> HumanFigureFromMachineText(std::string_view lexical, CellFormat format)
{
    // `ParseFiniteDouble` and not `from_chars`: libc++ before macOS 26.0 has no
    // floating-point overload, so the obvious spelling compiles on two standard
    // libraries and fails to BUILD on the one leg CI runs this on.
    auto value = 0.0;
    if (!ParseFiniteDouble(lexical, value))
        return std::nullopt;

    // A share is the only fractional format, and it is asked of the TABLE rather than
    // compared against the enumerator here, so a second fractional format added later
    // needs no edit at this site.
    if (CellFormatTable[static_cast<std::size_t>(format)].figure == FigureFormat::Percent)
        return HumanShareFigure(value);

    // Everything else is integral on the wire. A value with a fractional part is not a
    // cell of this format, so the caller is told rather than handed a rounded answer.
    if (value < 0.0 || value != std::floor(value))
        return std::nullopt;
    return HumanFleetFigure(static_cast<std::uint64_t>(value), format);
}

std::string HumanShareFigure(double share)
{
    // A person is shown a percentage and a program is given the fraction, from the ONE
    // value the cell carries -- which is the point of carrying the fraction. The two
    // spellings cannot drift apart because neither is stored.
    return WriteFigure(share, FigureFormat::Percent).Text();
}

std::string HumanFleetFigure(std::uint64_t number, CellFormat format)
{
    auto const& row = CellFormatTable[static_cast<std::size_t>(format)];
    if (!row.figure.has_value())
        return std::format("{}", number);
    return WriteFigure(static_cast<double>(number) * row.scale, *row.figure).Text();
}

std::string RenderFleetText(FleetSnapshot const& snapshot,
                            FleetHistoryView const& history,
                            std::optional<FleetSection> section)
{
    std::string out;
    out.reserve(2048);

    // A follower renders NO table, exactly as the page renders none and for the same
    // reason: its registry holds whatever registered against IT, so a table here is a
    // fraction of the fleet in the shape of the whole of it. The status says "not me"
    // and only the document can say "and here is who" -- and this format is the worst
    // of the three to get that wrong in, because the page has room for a sentence
    // while a partial table is shaped exactly like a complete one and the reader is
    // piping it into `cut`.
    //
    // Every line is a comment, so a reader stripping them is left with an EMPTY
    // document rather than a partial one. Naming the leader, never linking to it: the
    // endpoint below is that machine's SCHEDULER port, where its dashboard is served
    // is configuration on that node, and nothing replicates it here.
    if (!LeadsTheFleet(snapshot))
    {
        out += "# this node does not lead the fleet, so it cannot answer for it\n";
        out += "# its registry holds only what registered against it, which is a fraction of\n";
        out += "# the fleet rather than a smaller picture of it\n";
        if (snapshot.leaderEndpoint.empty())
            out += "# leader: none known -- an election is in progress, so there is nobody to name\n";
        else
            out += std::format("# leader: {} -- that is its scheduler port, not its dashboard\n",
                               EscapeDelimited(snapshot.leaderEndpoint));
        return out;
    }

    // ONE section: the header row and its rows, and nothing else. No marker, because
    // this is the form a reader pipes straight into `cut` or `awk` -- a marker line
    // would be one more thing every consumer has to know to skip.
    if (section.has_value())
    {
        AppendSectionText(out, snapshot, history, *section);
        return out;
    }

    // Every section: each one behind a marker naming it, blank-line separated. This
    // is the form somebody reads with `curl`, and it is self-describing so that the
    // keys the `section` parameter accepts can be discovered by asking for none --
    // except a section whose size is not the fleet's, which is asked for by name
    // (`FleetSectionRow::inWhole`); the refusal naming the keys lists it.
    bool firstSection = true;
    for (auto const& row: FleetSectionTable)
    {
        if (!row.inWhole)
            continue;
        if (!std::exchange(firstSection, false))
            out += '\n';
        out += std::format("# {}\n", row.key);
        AppendSectionText(out, snapshot, history, row.section);
    }
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
.chip--memory { background:var(--accent-soft); color:var(--accent); }
.chip--scratch { background:var(--crit-soft); color:var(--crit); }
.chip--cordoned { background:var(--sunk); color:var(--ink); border-color:var(--line); }
.chip--registered { background:var(--ok-soft); color:var(--ok); }
.chip--notice { background:var(--accent-soft); color:var(--accent); }
.chip--warning { background:var(--warn-soft); color:var(--warn); }
.chip--alert { background:var(--crit-soft); color:var(--crit); }
.chip--raised { background:var(--warn-soft); color:var(--warn); }
.prose { display:inline-block; white-space:normal; min-width:24ch; max-width:64ch; }
.pill--latched { background:var(--sunk); color:var(--ink); border-color:var(--line); }
.pill--live { background:var(--accent-soft); color:var(--accent); }
.conditions { padding:1rem 1.2rem 1.1rem; border-left:3px solid var(--warn); }
.conditions--quiet { border-left-color:var(--ok); }
.conditions--absent { border-left-color:var(--inert); }
.machine-conditions + .machine-conditions { margin-top:1rem; padding-top:.9rem; border-top:1px solid var(--hairline); }
.machine-conditions h3 { margin:0 0 .4rem; font:600 13px/1.3 ui-monospace,monospace; }
.condition { margin:.35rem 0 0; display:flex; flex-wrap:wrap; align-items:baseline; gap:.3rem .6rem; }
.condition-id { font:600 12.5px/1.3 ui-monospace,monospace; }
.condition-detail, .condition-remedy { flex-basis:100%; margin:0; font-size:12.5px; max-width:96ch; }
.condition-remedy { color:var(--muted); }
details summary { cursor:pointer; color:var(--muted); font-size:12.5px; margin-top:.8rem; }
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
.reason-m { display:block; font:400 10.5px/1.4 ui-monospace,monospace; color:var(--muted);
            margin-top:.45rem; word-break:break-all; opacity:.85; }
.range { display:inline-flex; border:1px solid var(--line); border-radius:3px; overflow:hidden;
         background:var(--surface); }
.range a { display:block; padding:.25rem .6rem; font:600 10.5px/1.5 ui-monospace,monospace;
           letter-spacing:.08em; color:var(--muted); text-decoration:none; }
.range a + a { border-left:1px solid var(--line); }
.range a:hover { color:var(--ink); background:var(--sunk); }
.range a.on { background:var(--accent); color:#FFF; }
.range a:focus-visible { outline:2px solid var(--accent); outline-offset:-2px; }
.charts { display:grid; grid-template-columns:repeat(auto-fit,minmax(330px,1fr)); gap:1rem; }
.chart { padding:.95rem 1.1rem 1rem; display:flex; flex-direction:column; }
.chart-head { display:flex; align-items:baseline; justify-content:space-between; gap:.75rem; }
.chart h3 { margin:0; font:600 12.5px/1.3 ui-sans-serif,system-ui,sans-serif; }
.chart-now { font:500 12px/1 ui-monospace,monospace; color:var(--muted); white-space:nowrap; }
.chart-cap { margin:.1rem 0 .55rem; font-size:11.5px; color:var(--muted); line-height:1.45; }
.chart-keys { display:flex; flex-wrap:wrap; gap:.2rem .9rem; margin-top:.5rem;
              font-size:11.5px; color:var(--muted); }
/* `height:auto` and not a fixed height: a viewBox squashed to a panel's width
   distorts every glyph in it, so the axis labels stop being readable at exactly
   the width the grid actually gives a chart. */
.chart img { display:block; width:100%; height:auto; }
.spark { display:block; width:74px; height:24px; margin-top:.1rem; }
.spark svg { display:block; width:100%; height:100%; }
.note a { color:var(--accent); }
/* One class per palette token rather than an inline style: a raw string literal
   ends at `)"`, which is exactly how `style="background:var(--accent)"` ends. */
.tone { display:inline-block; width:9px; height:9px; border-radius:2px;
        transform:translateY(1px); margin-right:.35rem; }
.tone--accent { background:var(--accent); }
.tone--ok { background:var(--ok); }
.tone--warn { background:var(--warn); }
.tone--crit { background:var(--crit); }
.tone--inert { background:var(--inert); }
.tone--muted { background:var(--muted); }
.follower { padding:1.1rem 1.2rem 1.2rem; border-left:3px solid var(--warn); }
.leader { font-family:ui-monospace,monospace; font-weight:600; color:var(--accent); }
footer { margin-top:2.4rem; padding-top:1rem; border-top:1px solid var(--line);
         color:var(--faint); font-size:12px; }
)CSS";

    /// How wide one bucket of the selected range is.
    [[nodiscard]] std::int64_t BucketSecondsOf(FleetHistoryView const& history) noexcept
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   FleetRangeTable[static_cast<std::size_t>(history.range)].bucket)
            .count();
    }

    /// What a URL calls the selected range, e.g. `24h`.
    [[nodiscard]] std::string_view RangeKeyOf(FleetHistoryView const& history) noexcept
    {
        return FleetRangeTable[static_cast<std::size_t>(history.range)].key;
    }

    /// One series folded over the selected range.
    /// @param key The series' key.
    /// @param history What was recorded.
    /// @return The folded value, or nullopt when the range holds nothing to fold.
    [[nodiscard]] std::optional<double> FoldedSeries(std::string_view key, FleetHistoryView const& history)
    {
        auto const* const series = FleetSeriesFromKey(key);
        if (series == nullptr)
            return std::nullopt;
        return RangeValueOf(*series, history.buckets, BucketSecondsOf(history));
    }

    /// The share of dispatch decisions that were refusals, over the range.
    ///
    /// The four reasons are read off the Refusals chart's own row rather than listed
    /// again here, so a fifth reason reaches this figure by being added to the series
    /// table once -- which is the whole reason these are tables.
    [[nodiscard]] std::optional<double> RefusedShare(FleetHistoryView const& history)
    {
        auto const* const granted = FleetSeriesFromKey("dispatched");
        if (granted == nullptr)
            return std::nullopt;
        auto const seconds = BucketSecondsOf(history);
        auto const grantedTotal = RangeValueOf(*granted, history.buckets, seconds);

        auto const& refusals = FleetChartTable[static_cast<std::size_t>(FleetChartId::Refusals)];
        double refused = 0.0;
        bool known = grantedTotal.has_value();
        for (auto const offset: std::views::iota(std::size_t { 0 }, refusals.count))
            if (auto const one = RangeValueOf(FleetSeriesTable[refusals.first + offset], history.buckets, seconds);
                one.has_value())
            {
                refused += *one;
                known = true;
            }
        if (!known)
            return std::nullopt;

        auto const total = refused + grantedTotal.value_or(0.0);
        // The fleet was asked for nothing at all, so no proportion of it was refused.
        // Zero would read as "everything went through", which nothing did.
        if (total <= 0.0)
            return std::nullopt;
        return 100.0 * refused / total;
    }

    /// The figure beside a chart's title: what it reads *right now*.
    ///
    /// A stacked chart's headline is the sum of its bands, because the question a
    /// stack answers is how much in total -- while an overlay's is its first series,
    /// which is the one the panel is named after. Both fall out of `shape`, so a
    /// fifth chart needs no rule of its own.
    /// @param chart Which chart.
    /// @param history What was recorded.
    /// @return The figure with its unit, or the dash when nothing is known.
    [[nodiscard]] std::string HeadlineOf(FleetChartRow const& chart, FleetHistoryView const& history)
    {
        auto const seconds = BucketSecondsOf(history);
        auto const newest = [&](std::size_t offset) {
            return LatestOf(FleetSeriesTable[chart.first + offset], history.buckets, seconds);
        };

        if (chart.shape == FleetChartShape::Stacked)
        {
            // A stack answers "how much in total", so its headline is the total.
            double sum = 0.0;
            bool known = false;
            for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count))
                if (auto const value = newest(offset); value.has_value())
                {
                    sum += *value;
                    known = true;
                }
            return known ? std::format("{:.1f}{} now", sum, chart.nowUnit) : std::string { AbsentText };
        }

        // An overlay of more than one series is a comparison -- the last series
        // against the first, which is the ceiling it is measured under. Reporting
        // only the ceiling would answer a question nobody asked of a panel titled
        // "X vs. Y".
        auto const headline = newest(chart.count - 1);
        if (!headline.has_value())
            return std::string { AbsentText };
        if (chart.count == 1)
            return std::format("{:.1f}{} now", *headline, chart.nowUnit);
        auto const ceiling = newest(0);
        if (!ceiling.has_value())
            return std::format("{:.0f} / {} now", *headline, AbsentText);
        return std::format("{:.0f} / {:.0f}{} now", *headline, *ceiling, chart.nowUnit);
    }

    /// What one readout on the strip says.
    ///
    /// **A NUMBER and its scale, never the formatted text** (#1302). This used to be
    /// three display strings -- `"12"`, `"/ 32 slots"`, `"this fleet's own work"` --
    /// which is fine for the one surface that had them and useless to the two that
    /// now do: a consumer handed `"12"` and `"/ 32 slots"` has to parse the figure
    /// back out of a label, which is a receiver recomputing by string-scraping. What
    /// must not be re-implemented is the DERIVATION -- the hit-rate arithmetic, the
    /// oldest-heartbeat fold -- and not the formatting, so one projection feeds three
    /// renderings and the page keeps its own dressing.
    ///
    /// `sub` and `unit` are the PAGE's alone and reach no machine-readable surface.
    /// They are prose about a figure the machine already has.
    struct KpiReadout
    {
        FleetCell value;   ///< The figure a machine reads; `Nothing()` when unanswerable.
        FleetCell of;      ///< Its denominator, or `Nothing()` for a tile that has none.
        CellFormat format; ///< The scale `value` is in.
        std::string unit;  ///< The small suffix the PAGE puts beside it; empty for none.
        std::string sub;   ///< The line the PAGE puts under it.
    };

    /// A folded series as a counting cell.
    ///
    /// Rounded rather than truncated: the series is a sum of readings, so a fold that
    /// lands on `122.9999` is a 123 that floating point walked back, and truncating it
    /// reports one fewer compile than happened.
    /// @param value The folded series, absent when the window carries none.
    /// @return The cell.
    [[nodiscard]] FleetCell CountCell(std::optional<double> value)
    {
        if (!value.has_value())
            return FleetCell::Nothing();
        return FleetCell::Of(static_cast<std::uint64_t>(std::llround(*value)));
    }

    /// A percentage as a share cell.
    ///
    /// **Takes a percentage and stores a fraction**, which is the one conversion left:
    /// every caller here computes a percentage because that is what the page showed, and
    /// every machine surface publishes a fraction since #1445. Doing it once, here,
    /// is what stops a second convention appearing at a column.
    /// @param share The share as a percentage (0..100), absent when it cannot be computed.
    /// @return The cell.
    [[nodiscard]] FleetCell ShareCell(std::optional<double> share)
    {
        if (!share.has_value())
            return FleetCell::Nothing();
        return FleetCell::OfShare(*share / 100.0);
    }

    /// Whether this fleet has been asked to compile nothing at all.
    ///
    /// Dispatch is opt-in — a client asks for a lease only when
    /// `FASTCACHE_SCHEDULER` names a scheduler — so a node deployed as a shared
    /// *cache*, which is the common case, sits in this state permanently while the
    /// machine around it compiles at full tilt. The page has to say which of the
    /// two it is looking at, because the numbers are identical and the operator's
    /// reading of them is not.
    ///
    /// **Three conditions, not one, because the grant counter is process-local.**
    /// `DispatchLeasesGranted` counts what THIS process granted since it started,
    /// and leadership moves: a scheduler that has just taken over from a failed
    /// leader has granted nothing while heartbeats have already repopulated
    /// `inFlight`. Asking the counter alone would put "no compile has been handed
    /// to any of them" on a page whose own bar shows twelve running, and displace
    /// the withheld reading the operator needs. So live work in either spelling —
    /// jobs in flight, or a lease outstanding — disqualifies the claim, and the
    /// note itself is careful to say *since this scheduler took over* rather than
    /// *ever*.
    ///
    /// **Absent is not zero.** A snapshot carrying no lease figures is making no
    /// claim about dispatch, and reading that as a zero would put the strongest
    /// sentence on this page in front of a caller who never said so.
    /// @param snapshot The snapshot.
    /// @param totals Its capacity split.
    /// @return True when machines are registered and none of them has been given
    ///         work this scheduler can account for.
    [[nodiscard]] bool NeverDispatched(FleetSnapshot const& snapshot, FleetTotals const& totals)
    {
        if (totals.registered == 0 || !HasLeaseFigures(snapshot))
            return false;
        return CountAt(snapshot, GrantedLeaseIndex) == 0 && totals.inFlight == 0 && snapshot.liveLeases == 0;
    }

    // `FigureOr` stood here and formatted a figure or the dash for the strip. Every one
    // of its callers now produces a CELL instead and the page formats it through
    // `CellAsText`, so it is removed rather than left for a reader to wonder which of
    // two spellings is the live one -- superseded code goes out, it does not get a
    // deprecation.

    /// What `compiling-now`'s denominator counts: the page's `/ 32 slots` and a terminal's `of 32 slots`.
    constexpr std::string_view SlotsNoun = "slots";

    /// What `never-picked`'s denominator counts.
    constexpr std::string_view ToolchainsNoun = "toolchain(s)";

    /// What an outstanding lease is, in the words the page's sub-line and a terminal tile share.
    ///
    /// "Not yet resolved", not "not yet claimed". Nothing claimed a lease and nothing ever could -- the
    /// only way one left this figure was by expiring, ten minutes after the job it named had finished,
    /// so on a busy fleet it read as a backlog that did not exist. A client now hands its lease back
    /// when its job ends (#212), which is what makes this a live number.
    constexpr std::string_view LeasesNote = "not yet resolved";

    KpiReadout KpiDispatched(FleetSnapshot const& /*snapshot*/, FleetHistoryView const& history)
    {
        return KpiReadout { .value = CountCell(FoldedSeries("dispatched", history)),
                            .of = FleetCell::Nothing(),
                            .format = CellFormat::Count,
                            .unit = {},
                            .sub = std::format("compiles in the last {}", RangeKeyOf(history)) };
    }

    KpiReadout KpiCompilingNow(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        auto const totals = TotalsFor(snapshot);
        // The sub-line names WHICH zero this is. "This fleet's own work" over a 0
        // reads as an idle fleet, and on a node nothing dispatches to it is the
        // tile an operator stares at while their build saturates the machine.
        return KpiReadout { .value = FleetCell::Of(totals.inFlight),
                            // The denominator is a NUMBER of its own rather than text
                            // inside the unit: `/ 32 slots` is a label a machine would
                            // have to take `32` back out of.
                            .of = FleetCell::Of(totals.registered),
                            .format = CellFormat::Count,
                            .unit = std::format("/ {} {}", totals.registered, SlotsNoun),
                            .sub = NeverDispatched(snapshot, totals) ? "nothing dispatched yet" : "this fleet's own work" };
    }

    KpiReadout KpiHitRate(FleetSnapshot const& /*snapshot*/, FleetHistoryView const& history)
    {
        // The `%` moved out of the page's `<small>` and into the cell, because
        // `CellAsText` already renders a per-mille cell as `85.3 %` and two places
        // spelling one suffix is the drift this table exists to prevent.
        return KpiReadout { .value = ShareCell(FoldedSeries("hit-rate", history)),
                            .of = FleetCell::Nothing(),
                            .format = CellFormat::Share,
                            .unit = {},
                            .sub = std::format("over the last {}", RangeKeyOf(history)) };
    }

    KpiReadout KpiRefused(FleetSnapshot const& /*snapshot*/, FleetHistoryView const& history)
    {
        return KpiReadout { .value = ShareCell(RefusedShare(history)),
                            .of = FleetCell::Nothing(),
                            .format = CellFormat::Share,
                            .unit = {},
                            .sub = "of dispatch decisions" };
    }

    KpiReadout KpiLeases(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        return KpiReadout { .value = FleetCell::Of(snapshot.liveLeases),
                            .of = FleetCell::Nothing(),
                            .format = CellFormat::Count,
                            .unit = {},
                            .sub = std::format("granted, {}", LeasesNote) };
    }

    KpiReadout KpiOldestHeartbeat(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        if (snapshot.nodes.empty())
            // Absent, never zero. A fleet with no machine has no oldest heartbeat, and
            // a `0` there reads as every machine answering this instant.
            return KpiReadout { .value = FleetCell::Nothing(),
                                .of = FleetCell::Nothing(),
                                .format = CellFormat::Millis,
                                .unit = {},
                                .sub = "no machine registered" };
        auto const oldest = std::ranges::max(snapshot.nodes, {}, &NodeReport::heartbeatAge).heartbeatAge;
        // The *oldest*, not the mean: one machine that stopped answering an hour ago
        // is the fact worth surfacing, and an average over a healthy fleet buries it.
        // Which is also why the DERIVATION has to travel rather than the reading: a
        // consumer handed the rows and left to re-derive reaches for a mean.
        return KpiReadout { .value = FleetCell::Of(static_cast<std::uint64_t>(oldest.count())),
                            .of = FleetCell::Nothing(),
                            .format = CellFormat::Millis,
                            .unit = {},
                            .sub = std::format("across {} machine(s)", snapshot.nodes.size()) };
    }

    /// How many of the fleet's toolchains nothing has ever been sent to.
    ///
    /// The one number on this strip that answers *is the fleet reaching everything
    /// it registered* (#1297). Every other tile here counts something that happened;
    /// this one counts something that did NOT, which is why it had no home before: a
    /// toolchain the scheduler never selects produces no event on either machine, so
    /// no counter moves and every other reading on this page stays healthy.
    ///
    /// Absent rather than `0` on an empty fleet, and the delegation is deliberate --
    /// `PickCoverageFor` owns that decision so the page and any later reader cannot
    /// answer it differently.
    ///
    /// **Expect a non-zero reading for a while after a failover or a rolling
    /// restart, and do not chase it.** The record is this leader's and is per
    /// registration, so a scheduler that has just taken over has chosen nobody yet,
    /// and a node that has just re-registered starts again with nothing recorded --
    /// both clear as soon as one job goes to each toolchain. A count that PERSISTS
    /// while the fleet is building is the finding; the worker table's
    /// `registered-age` beside `last-picked-age` is what separates the two.
    KpiReadout KpiNeverPicked(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        auto const coverage = PickCoverageFor(snapshot);
        if (!coverage.has_value())
            return KpiReadout { .value = FleetCell::Nothing(),
                                .of = FleetCell::Nothing(),
                                .format = CellFormat::Count,
                                .unit = {},
                                .sub = "no worker registered" };
        return KpiReadout { .value = FleetCell::Of(coverage->neverPicked),
                            .of = FleetCell::Of(coverage->toolchains),
                            .format = CellFormat::Count,
                            .unit = {},
                            .sub = std::format("of {} {} registered", coverage->toolchains, ToolchainsNoun) };
    }

    /// One readout on the strip.
    ///
    /// A table rather than six calls: the strip is the part of this page most likely
    /// to grow a seventh tile, and a tile added as a seventh call is one whose label
    /// case, absent spelling and order are checked by nothing.
    struct KpiRow
    {
        std::string_view label; ///< What the tile is called on the page.

        /// What a machine-readable surface calls it.
        ///
        /// A SECOND column rather than one spelling doing both, unlike `FleetColumn`,
        /// and the reason is the label: *Cache hit rate* is a heading with spaces and
        /// a capital, which is a poor JSON key, and `cache-hit-rate` is a poor heading.
        /// What the one-spelling rule buys is that they cannot be attached to
        /// DIFFERENT tiles -- they are two fields of one row, so a tile renamed on the
        /// page and a key left behind is one edit rather than two files.
        ///
        /// Stable across a relabelling: this is what a scraper keys on, and the label
        /// above is what a reader sees, so the page is free to be reworded.
        std::string_view key;

        KpiReadout (*project)(FleetSnapshot const&, FleetHistoryView const&); ///< What it reads.
        std::string_view ofNoun {}; ///< What the denominator counts; empty for a figure with none.
        std::string_view note {};   ///< Words for a figure with no denominator; empty for none.
        bool sparkline;             ///< Whether it carries one.
        bool alertAboveZero {};     ///< Whether any value above zero is worth an operator's eye.
    };

    /// The strip, in the order it is read. The mockup's six, in the mockup's order,
    /// and one added after them rather than among them: the mockup's order is what a
    /// reader of this page already knows, and an insertion would move six tiles to
    /// place one.
    constexpr std::array<KpiRow, 7> KpiTable {
        KpiRow { .label = "Dispatched", .key = "dispatched", .project = KpiDispatched, .sparkline = true },
        KpiRow { .label = "Compiling now",
                 .key = "compiling-now",
                 .project = KpiCompilingNow,
                 .ofNoun = SlotsNoun,
                 .sparkline = false },
        KpiRow { .label = "Cache hit rate", .key = "cache-hit-rate", .project = KpiHitRate, .sparkline = false },
        KpiRow { .label = "Refused", .key = "refused", .project = KpiRefused, .sparkline = false, .alertAboveZero = true },
        KpiRow { .label = "Leases outstanding",
                 .key = "leases-outstanding",
                 .project = KpiLeases,
                 .note = LeasesNote,
                 .sparkline = false },
        KpiRow { .label = "Oldest heartbeat", .key = "oldest-heartbeat", .project = KpiOldestHeartbeat, .sparkline = false },
        KpiRow { .label = "Never picked",
                 .key = "never-picked",
                 .project = KpiNeverPicked,
                 .ofNoun = ToolchainsNoun,
                 .sparkline = false },
    };

    /// Every tile states a key, no two share one, and each is kebab-case.
    ///
    /// A row that forgets `key` is still a row, at the right position, with a label --
    /// it just renders a machine-readable figure under an empty name, which collides
    /// with the next row that forgets one. Neither `RowsInEnumeratorOrder` nor any
    /// render-time check can see that, and there is no reason a tile could have for
    /// being unnamed, so the type system answers it. The spelling is the one every other
    /// name a program reads is held to (`IsKebabName`, #1445), and it implies non-empty.
    /// @return True when every key is kebab-case and distinct.
    [[nodiscard]] consteval bool EveryKpiIsKeyed()
    {
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, KpiTable.size()), [](std::size_t outer) {
            if (!IsKebabName(KpiTable[outer].key))
                return false;
            return std::ranges::none_of(std::views::iota(outer + 1, KpiTable.size()),
                                        [outer](std::size_t inner) { return KpiTable[outer].key == KpiTable[inner].key; });
        });
    }
    static_assert(EveryKpiIsKeyed(), "every KpiTable row needs its own kebab-case machine key");

    /// Each row's words, so `FleetKpis` can hand out a view of something static.
    constexpr auto KpiTexts = [] {
        std::array<FleetKpiText, KpiTable.size()> texts {};
        for (auto const index: std::views::iota(std::size_t { 0 }, KpiTable.size()))
            texts[index] = FleetKpiText { .key = KpiTable[index].key,
                                          .label = KpiTable[index].label,
                                          .ofNoun = KpiTable[index].ofNoun,
                                          .note = KpiTable[index].note,
                                          .sparkline = KpiTable[index].sparkline,
                                          .alertAboveZero = KpiTable[index].alertAboveZero };
        return texts;
    }();

    /// The keys alone, so `FleetKpiKeys` can hand out a view of something static.
    constexpr auto KpiKeys = [] {
        std::array<std::string_view, KpiTable.size()> keys {};
        for (auto const index: std::views::iota(std::size_t { 0 }, KpiTable.size()))
            keys[index] = KpiTable[index].key;
        return keys;
    }();

    /// Append the headline figures as a table.
    ///
    /// One row per `KpiTable` row, under the key its own row names. The page's `unit`
    /// and `sub` do NOT appear: they are prose about a figure this table already
    /// carries, and a reader piping into `cut` wants the number.
    /// @param out Appended to.
    /// @param snapshot What to read.
    /// @param history The window the history-derived figures answer for.
    void AppendKpiJson(std::string& out, FleetSnapshot const& snapshot, FleetHistoryView const& history)
    {
        AppendJsonString(out, "kpi");
        out += ":{";
        bool firstKpi = true;
        for (auto const& row: KpiTable)
        {
            if (!std::exchange(firstKpi, false))
                out += ',';
            auto const readout = row.project(snapshot, history);
            AppendJsonString(out, row.key);
            out += ":{";
            AppendJsonString(out, "value");
            out += ':';
            AppendCellAsJson(out, readout.value);
            out += ',';
            // The scale, because the key alone cannot say whether 853 is a count or
            // eight-hundred-and-fifty-three thousandths.
            AppendJsonString(out, "unit");
            out += ':';
            AppendJsonString(out, CellFormatTable[static_cast<std::size_t>(readout.format)].name);
            out += ',';
            AppendJsonString(out, "of");
            out += ':';
            AppendCellAsJson(out, readout.of);
            out += '}';
        }
        out += '}';
    }

    void AppendKpiText(std::string& out, FleetSnapshot const& snapshot, FleetHistoryView const& history)
    {
        bool firstColumn = true;
        for (auto const name: KpiTextColumns)
        {
            if (!std::exchange(firstColumn, false))
                out += '\t';
            out += name;
        }
        out += '\n';

        for (auto const& row: KpiTable)
        {
            auto const readout = row.project(snapshot, history);
            out += EscapeDelimited(row.key);
            out += '\t';
            AppendCellAsText(out, readout.value);
            out += '\t';
            out += EscapeDelimited(CellFormatTable[static_cast<std::size_t>(readout.format)].name);
            out += '\t';
            AppendCellAsText(out, readout.of);
            out += '\n';
        }
    }

    /// The sentence a split of numbers needs beside it.
    constexpr std::string_view LeaseNote =
        "Do not add these together. An empty fleet, a busy one, machines somebody else is using and an object "
        "already being built are four different problems with four different fixes, and a total hides all of them.";
    /// What to say about windows the range only partly observed, or nothing.
    ///
    /// The NEWEST window is excluded, always: it is still filling and is partly
    /// covered by definition, so counting it would put "1 window partly observed" on
    /// every live page and make the number mean nothing. What is left is downtime --
    /// a window this fleet was only watched for part of -- which is worth a reader's
    /// attention precisely because the chart cannot show it: a bucket observed for
    /// one minute of five is drawn exactly like one observed for all five.
    ///
    /// A BACKFILLED window is excluded too, and for a sharper reason: its `coverage`
    /// is not the same quantity. `BackfillInto` takes the best contributing machine's
    /// sample count, which is compared here against a FLEET sample denominator -- so
    /// fifteen workstations each up eight hours a day would report every one of a
    /// year's backfilled days as partly observed, permanently, which is exactly the
    /// noise excluding the newest bucket exists to avoid.
    /// @param history What is being drawn.
    /// @return The note, or empty when every settled window was fully observed.
    [[nodiscard]] std::string CoverageNote(FleetHistoryView const& history)
    {
        if (history.buckets.empty())
            return {};

        auto const full = FullCoverageOf(history.range);
        auto partial = std::size_t { 0 };
        auto settled = std::size_t { 0 };
        for (auto const& bucket: std::span { history.buckets }.first(history.buckets.size() - 1))
        {
            if (!bucket.present || bucket.backfilled)
                continue;
            ++settled;
            if (bucket.coverage < full)
                ++partial;
        }
        if (partial == 0)
            return {};
        return std::format("{} of {} windows partly observed", partial, settled);
    }

    /// Append the whole "Over time" section: the range control, the charts, the note.
    ///
    /// Its own function rather than more of `RenderFleetHtml`, and not only for
    /// length: that function sits right at clang-tidy's cognitive-complexity
    /// ceiling, which is the tool saying a decision has spread too far. Everything
    /// here turns on one thing -- which range is in view -- and nothing above it does.
    /// @param out Where to append.
    /// @param history What was recorded, and what is being drawn.
    void AppendOverTime(std::string& out, FleetHistoryView const& history)
    {
        auto const rangeKey = RangeKeyOf(history);
        auto const& rangeRow = FleetRangeTable[static_cast<std::size_t>(history.range)];

        out += R"(<section><div class="sec-head"><h2>Over time</h2><span class="range">)";
        for (auto const& row: FleetRangeTable)
            // Links, not buttons: the control is two URLs, so it works with no script,
            // survives a bookmark and is what the auto-refresh comes back to.
            out += std::format(R"(<a{} href="?range={}">{}</a>)",
                               row.range == history.range ? R"( class="on")" : "",
                               EscapeHtml(row.key),
                               EscapeHtml(row.label));
        auto const coverage = CoverageNote(history);
        out += std::format(R"(</span><span class="rule"></span><span class="meta">{}{}{} &middot; {}</span></div>)",
                           EscapeHtml(rangeRow.bucketLabel),
                           coverage.empty() ? "" : " &middot; ",
                           coverage,
                           history.durable ? "kept on disk" : "kept in memory only");
        if (std::ranges::none_of(history.buckets, [](auto const& bucket) { return bucket.present; }))
            // A frame with nothing in it reads as a broken chart. Absent is not zero
            // here either: nobody was watching, which is not a fleet that did nothing.
            out += R"(<div class="panel occ"><p class="note">Nothing has been recorded for this range yet. )"
                   R"(The fleet-wide numbers are sampled once a minute <strong>while this node leads</strong> &mdash; )"
                   R"(a follower's registry holds only what registered against it, so sampling there would record )"
                   R"(a fraction as though it were the whole. Windows another node was leading for are filled in )"
                   R"(from the records the machines themselves keep and hand over, so they carry what each machine )"
                   R"(can answer for and not the scheduler's own counters. The first points appear a minute after )"
                   R"(this node won the election.</p></div>)";
        else
        {
            out += R"(<div class="charts">)";
            for (auto const& chart: FleetChartTable)
            {
                out += std::format(R"(<div class="panel chart"><div class="chart-head"><h3>{}</h3>)"
                                   R"(<span class="chart-now">{}</span></div><p class="chart-cap">{}</p>)",
                                   EscapeHtml(chart.title),
                                   // NOT escaped, and the same reasoning
                                   // `KpiReadout` already carries: this yields
                                   // either a number it formatted itself or
                                   // `AbsentText`, which IS markup -- the entity
                                   // for the dash. Escaping turned its `&` into
                                   // `&amp;`, so a chart with nothing to report
                                   // rendered the literal text `&ndash;`. Every
                                   // other absent path interpolates it raw.
                                   HeadlineOf(chart, history),
                                   EscapeHtml(chart.caption));
                // Its own resource, and the URL carries no cache-buster on purpose: a
                // generation in the query would make every bucket a new URL and the
                // conditional GET would never fire. Stable URL, `ETag`, `304`.
                out += std::format(R"(<img src="{}{}.svg?range={}" width="640" height="150" alt="{}">)",
                                   FleetChartPrefix,
                                   EscapeHtml(chart.key),
                                   EscapeHtml(rangeKey),
                                   EscapeHtml(std::format("{}, {}", chart.title, rangeRow.bucketLabel)));
                out += R"(<span class="chart-keys">)";
                for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count))
                {
                    auto const& series = FleetSeriesTable[chart.first + offset];
                    out += std::format(R"(<span><span class="tone tone--{}"></span>{}</span>)",
                                       EscapeHtml(series.colour),
                                       EscapeHtml(series.label));
                }
                out += "</span></div>";
            }
            out += "</div>";
        }
        out += std::format(R"(<p class="note"><strong>This history is the leader's.</strong> It is sampled by )"
                           R"(whichever node currently leads, so a failover moves this page to a machine with a )"
                           R"(different past. A bucket nobody sampled draws a <em>gap</em>, never a zero &mdash; )"
                           R"(zero says the fleet did nothing, a gap says nobody was watching. A window another )"
                           R"(node was leading for is filled in from the records the machines themselves keep, so )"
                           R"(it carries what each machine can answer for &mdash; its cache and its slots &mdash; )"
                           R"(and leaves the dispatch charts a gap, because no machine produces a scheduler's )"
                           R"(counters. {} The same series )"
                           R"(are available as JSON at <a href="{}?range={}">{}</a>, and for anything you would )"
                           R"(alert on, /metrics remains the source of truth.</p></section>)",
                           history.durable ? "It is written to disk, so it survives a restart."
                                           : "This node has no --cluster-dir or --cache-dir to write it to, so a restart "
                                             "starts the history again.",
                           FleetSeriesPath,
                           EscapeHtml(rangeKey),
                           FleetSeriesPath);
    }

    /// One reading of the capacity bar, and the sentence it needs beside it.
    ///
    /// A table rather than an `if`/`else` ladder, for the reason `KpiTable` is one:
    /// the note is the part of this panel that grows a state, and a state added as
    /// another arm is one whose *precedence* is written nowhere. Order is the
    /// contract here — the first row that applies wins — because the readings are
    /// not exclusive: a fleet nothing was ever dispatched to also has slots
    /// withheld, and saying so second would bury the fact that explains it.
    struct CapacityNoteRow
    {
        /// Whether this reading applies. Evaluated in table order.
        bool (*applies)(FleetSnapshot const&, FleetTotals const&);
        /// The note, already escaped-safe: every one is a literal plus numbers.
        ///
        /// Same parameters as `applies`, deliberately. A row whose two halves take
        /// different arguments is one that cannot grow a note needing what its
        /// predicate already had, and `KpiRow` sets the precedent.
        std::string (*render)(FleetSnapshot const&, FleetTotals const&);
    };

    /// The note for a fleet that has registered machines and been given no work.
    /// @param totals Its capacity split.
    /// @return The paragraph.
    [[nodiscard]] std::string NoteNeverDispatched(FleetSnapshot const& /*snapshot*/, FleetTotals const& totals)
    {
        // The sentence this panel was missing. Without it the reading below runs,
        // and it attributes the operator's OWN compiles to a third party: the host
        // CPU is busy, none of it is work this fleet was handed, so every slot the
        // ceiling withdraws is reported as somebody else's. An operator watching
        // their build saturate this machine reads "0 compiling" and a bar that says
        // the load is not theirs.
        return std::format(R"(<p class="note"><strong>Nothing has been dispatched to this fleet.</strong> )"
                           R"({} slots are registered, no compile has been handed to any of them since this )"
                           R"(scheduler took over, and none is running now &mdash; so what is drawn below is an )"
                           R"(<em>unused</em> fleet rather than an idle one. A client asks for a lease only when )"
                           R"(<code>FASTCACHE_SCHEDULER</code> names this scheduler; without it every compile )"
                           R"(runs locally, this machine's own build is what loads it, and <em>compiling</em> )"
                           R"(can only ever read zero. Until then, read the numbers below as capacity nobody )"
                           R"(has asked for.</p>)",
                           totals.registered);
    }

    /// The note for a fleet whose ceilings are holding slots back.
    /// @param totals Its capacity split.
    /// @return The paragraph.
    [[nodiscard]] std::string NoteWithheld(FleetSnapshot const& /*snapshot*/, FleetTotals const& totals)
    {
        // The sentence the split exists for. Which of the two shortages a fleet
        // has decides what an operator buys, and a single "utilisation" number
        // answers neither.
        return std::format(R"(<p class="note"><strong>Read the hatching first.</strong> {} of the {} slots )"
                           R"(these machines registered are not offerable right now, and that is not this )"
                           R"(fleet being busy: a ceiling withdrew them, because the host CPU is doing )"
                           R"(somebody else's work or the scratch filesystem is nearly full. Buying machines )"
                           R"(fixes a full blue bar. It does not fix this one.</p>)",
                           totals.withheld,
                           totals.registered);
    }

    /// The note for a fleet offering everything it registered.
    /// @return The paragraph.
    [[nodiscard]] std::string NoteNothingWithheld(FleetSnapshot const& /*snapshot*/, FleetTotals const& /*totals*/)
    {
        return R"(<p class="note">Nothing is being withheld: every registered slot is offerable, so what is )"
               R"(not blue is genuinely idle. A fleet that refuses work in this state needs more machines, )"
               R"(not quieter ones.</p>)";
    }

    /// Every reading, in precedence order. The last row applies unconditionally.
    constexpr std::array<CapacityNoteRow, 3> CapacityNoteTable {
        CapacityNoteRow { .applies = NeverDispatched, .render = NoteNeverDispatched },
        CapacityNoteRow { .applies = [](FleetSnapshot const&, FleetTotals const& totals) { return totals.withheld > 0; },
                          .render = NoteWithheld },
        CapacityNoteRow { .applies = [](FleetSnapshot const&, FleetTotals const&) { return true; },
                          .render = NoteNothingWithheld },
    };

} // namespace

std::span<std::string_view const> FleetKpiKeys() noexcept
{
    return KpiKeys;
}

std::vector<MachineNameTable> FleetMachineNameTables()
{
    // Every section's names with every tier composed, as the renderers compose them: a tier no member runs has no
    // column, so the snapshot says they all do.
    auto snapshot = FleetSnapshot {};
    snapshot.tiersPresent.fill(true);

    auto tables = std::vector<MachineNameTable> {};
    for (auto const& row: FleetSectionTable)
        tables.push_back(MachineNameTable { .table = row.key, .names = FleetColumnNames(row.section, snapshot) });

    auto const keysOf = [](std::string_view table, auto const& keys) {
        auto named = MachineNameTable { .table = table, .names = {} };
        for (auto const key: keys)
            named.names.emplace_back(key);
        return named;
    };
    tables.push_back(keysOf("fleet-sections", FleetSectionTable | std::views::transform(&FleetSectionRow::key)));
    tables.push_back(keysOf("kpi-keys", FleetKpiKeys()));
    tables.push_back(keysOf("lease-outcomes", LeaseOutcomeTable | std::views::transform(&LeaseOutcomeRow::key)));
    return tables;
}

std::span<FleetKpiText const> FleetKpis() noexcept
{
    return KpiTexts;
}

namespace
{
    /// How the page dresses one persistence: its pill, and what the word means to somebody hovering.
    struct PersistenceDress
    {
        CompileCacheWire::ConditionPersistence persistence; ///< The enumerator this row describes.
        std::string_view pillClass;                         ///< The pill's class.
        std::string_view meaning;                           ///< The pill's title.
    };

    /// One row per persistence, in enumerator order.
    ///
    /// **Two looks for two different promises** (#1364): a latched row cannot clear while that
    /// process runs, so an operator waiting for it to clear is waiting for nothing; a live one can,
    /// and watching it is watching progress. The WORD is on the pill either way, so the page does not
    /// depend on colour to say which.
    constexpr EnumTable<CompileCacheWire::ConditionPersistence, PersistenceDress> PersistenceDressTable { {
        { .persistence = CompileCacheWire::ConditionPersistence::Latched,
          .pillClass = "pill--latched",
          .meaning = "fixed for the life of this process: only a restart on a different build or configuration "
                     "clears it" },
        { .persistence = CompileCacheWire::ConditionPersistence::Live,
          .pillClass = "pill--live",
          .meaning = "can clear while this process runs" },
    } };
    static_assert(RowsInEnumeratorOrder(PersistenceDressTable, &PersistenceDress::persistence),
                  "PersistenceDressTable must hold one row per ConditionPersistence, in enumerator order");

    /// One condition a machine raised, as the panel lists it.
    /// @param out Appended to.
    /// @param row The row, as the machine sent it.
    void AppendRaisedCondition(std::string& out, CompileCacheWire::NodeConditionFields const& row)
    {
        auto const severity = CompileCacheWire::ConditionSeverityNamed(row.severity);
        auto const persistence = CompileCacheWire::ConditionPersistenceNamed(row.persistence);
        // A word this build cannot name is shown as sent, undressed: a colour or a meaning for it
        // would be this leader's guess about a newer node's vocabulary.
        auto const chip = severity.has_value() ? SeverityDressTable[static_cast<std::size_t>(*severity)].chipClass : "";
        auto const dress = persistence.has_value()
                               ? PersistenceDressTable[static_cast<std::size_t>(*persistence)]
                               : PersistenceDress { .persistence = CompileCacheWire::ConditionPersistence::Last,
                                                    .pillClass = "",
                                                    .meaning = "" };
        out += std::format(R"(<div class="condition"><span class="chip {}">{}</span>)"
                           R"(<span class="condition-id">{}</span>)"
                           R"(<span class="pill pill--value {}" title="{}">{}</span>)",
                           chip,
                           EscapeHtml(row.severity),
                           EscapeHtml(row.id),
                           dress.pillClass,
                           EscapeHtml(dress.meaning),
                           EscapeHtml(row.persistence));
        // The state only where it is not the ordinary `raised`: an `undecided` row, or a word from a
        // newer node, is exactly the one a reader must not take for a plain raise.
        if (CompileCacheWire::ConditionStateNamed(row.state) != CompileCacheWire::ConditionState::Raised)
            out += std::format(R"(<span class="chip chip--raised">{}</span>)", EscapeHtml(row.state));
        if (!row.detail.empty())
            out += std::format(R"(<p class="condition-detail">{}</p>)", EscapeHtml(row.detail));
        out += std::format(R"(<p class="condition-remedy">{}</p></div>)", EscapeHtml(row.remedy));
    }

    /// The panel's modifier class: none while some machine asks for attention, and otherwise which of the two
    /// quiet readings the fleet is in -- every machine said nothing is raised, or some machine said nothing at all.
    /// The second is dressed apart because it is the one nothing here can vouch for.
    /// @param anyAsking Whether some machine has a condition asking for attention.
    /// @param anyAbsent Whether some machine sent no conditions at all.
    /// @return The class suffix, with its leading space, or empty.
    [[nodiscard]] std::string_view ConditionsPanelModifier(bool anyAsking, bool anyAbsent) noexcept
    {
        if (anyAsking)
            return {};
        return anyAbsent ? " conditions--absent" : " conditions--quiet";
    }

    /// What is wrong across the fleet, before anything else on the page (#1364).
    ///
    /// **A panel, not a column**: forty machines each with a cell of ids would put the one thing an
    /// operator came to find in the widest table on the page. Every machine whose conditions ask for
    /// attention gets its rows here, with its severity, its persistence and its remedy; the rest are
    /// COUNTED in words, apart -- a machine reporting nothing raised and a machine reporting nothing
    /// at all are opposite readings, and a bare *everything is fine* would hide the second.
    ///
    /// Every row every machine sent follows behind a disclosure, walked from `ConditionColumns` like
    /// every other table: the panel is a reading of the same rows, never a second list of them.
    /// @param out Appended to.
    /// @param snapshot What to render.
    void AppendConditionsPanel(std::string& out, FleetSnapshot const& snapshot)
    {
        // A machine that asks is held with the rows it sent, taken where the list was found present, so
        // nothing below reads an optional it has not just checked.
        struct Asking
        {
            NodeReport const* node;                                         ///< The machine.
            std::vector<CompileCacheWire::NodeConditionFields> const* rows; ///< What it sent.
        };
        std::vector<Asking> asking;
        std::size_t quiet = 0;
        std::vector<std::string> absent;
        for (auto const& node: snapshot.nodes)
        {
            if (!node.conditions.has_value())
            {
                absent.push_back(node.endpoint);
                continue;
            }
            auto const& rows = *node.conditions;
            if (std::ranges::any_of(rows, &CompileCacheWire::AsksForAttention))
                asking.push_back(Asking { .node = &node, .rows = &rows });
            else
                ++quiet;
        }

        auto const modifier = ConditionsPanelModifier(!asking.empty(), !absent.empty());
        out += std::format(R"(<section><div class="sec-head"><h2>Conditions</h2><span class="rule"></span>)"
                           R"(<span class="meta">what each machine says is wrong with it</span></div>)"
                           R"(<div class="panel conditions{}">)",
                           modifier);

        for (auto const& [node, rows]: asking)
        {
            out += std::format(R"(<div class="machine-conditions"><h3>{}{}</h3>)",
                               EscapeHtml(node->endpoint),
                               node->displayName.empty() ? std::string {}
                                                         : std::format(" &middot; {}", EscapeHtml(node->displayName)));
            for (auto const& row: *rows)
                if (CompileCacheWire::AsksForAttention(row))
                    AppendRaisedCondition(out, row);
            out += "</div>";
        }

        if (snapshot.nodes.empty())
            out += R"(<p class="note">No machine has announced itself, so no machine has anything to report.</p>)";
        else if (asking.empty() && absent.empty())
            out += std::format(R"(<p class="note"><strong>No conditions raised</strong> on any of the {} machine(s).</p>)",
                               quiet);
        else if (quiet != 0)
            // "other" only beside machines listed above it: with none listed, the word points at nothing.
            out += std::format(R"(<p class="note">{} {}machine(s) report no conditions raised.</p>)",
                               quiet,
                               asking.empty() ? "" : "other ");

        if (!absent.empty())
        {
            std::string names;
            for (auto const& endpoint: absent)
                names += std::format("{}{}", names.empty() ? "" : ", ", EscapeHtml(endpoint));
            out += std::format(R"(<p class="note"><strong>{} machine(s) report no conditions at all</strong> &mdash; )"
                               R"(a build older than them, or announcements that have stopped reaching this leader. )"
                               R"(That is not the same as none raised: nothing here can vouch for them. {}</p>)",
                               absent.size(),
                               names);
        }

        auto const rows = ConditionRowsOf(snapshot);
        out += std::format(R"(<details><summary>every row every machine reported ({})</summary><div class="wrap">)",
                           rows.size());
        AppendHtmlRows(out, ConditionColumns, rows);
        out += "</div></details></div></section>";
    }
} // namespace

std::string RenderFleetHtml(FleetSnapshot const& snapshot, FleetHistoryView const& history, unsigned refreshSeconds)
{
    std::string out;
    out.reserve(16384);
    out += HtmlDocumentPrologue;
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

    // ---- what is wrong, before anything else ---------------------------------
    AppendConditionsPanel(out, snapshot);

    // ---- the readouts, before any table ------------------------------------
    //
    // Free and withheld are deliberately *not* here: they are the capacity meter's
    // two segments directly below, and a number repeated a hand's width from the
    // picture of itself is a number that will one day disagree with it.
    out += R"(<section><div class="kpis">)";
    for (auto const& row: KpiTable)
    {
        auto const readout = row.project(snapshot, history);
        // `CellAsText` rather than a preformatted string: the projection carries a
        // number and its scale now, so the page dresses it the same way it dresses
        // every other cell and an absent figure renders the same dash as an absent
        // column does.
        out += std::format(R"(<div class="kpi"><span class="kpi-label">{}</span>)"
                           R"(<span class="kpi-value">{}<small>{}</small></span>)",
                           EscapeHtml(row.label),
                           CellAsText(readout.value, readout.format),
                           EscapeHtml(readout.unit));
        // Inlined rather than a seventh request: it is part of the tile's layout at
        // roughly two hundred bytes, and being inside the page is also what lets it
        // resolve the page's own custom properties instead of carrying a palette.
        if (row.sparkline && !history.buckets.empty())
            out += std::format(R"(<span class="spark">{}</span>)", RenderSparklineSvg(history.buckets));
        out += std::format(R"(<span class="kpi-sub">{}</span></div>)", EscapeHtml(readout.sub));
    }
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
                           R"(<b>{}</b> <span>withheld &mdash; CPU, memory or scratch, not us</span></span></div>)",
                           totals.inFlight,
                           totals.free,
                           totals.withheld);
        // The first reading that applies, from `CapacityNoteTable`. Its last row is
        // unconditional, so this always appends exactly one paragraph.
        for (auto const& row: CapacityNoteTable)
        {
            if (!row.applies(snapshot, totals))
                continue;
            out += row.render(snapshot, totals);
            break;
        }
    }
    out += "</div></section>";

    AppendOverTime(out, history);

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

    // ---- leases outstanding -------------------------------------------------
    // Placed after Workers because the `worker` column joins to it, and before the
    // refusal counters because this is the section somebody reaches for when the
    // fleet has stopped moving rather than when it is refusing.
    out += std::format(R"(<section><div class="sec-head"><h2>Leases outstanding</h2><span class="rule"></span>)"
                       R"(<span class="meta">{}</span></div><div class="panel wrap">)",
                       EscapeHtml(OutstandingLeaseMeta(snapshot)));
    AppendHtmlRows(out, LeaseColumns, snapshot.outstandingLeases);
    out += "</div>";
    out += R"(<p class="note">A client hands its lease back when the job ends, however it ended, so one that has )"
           R"(been outstanding for minutes is not waiting to age out: it is a client that died mid-build, and the )"
           R"(endpoint beside it is where its work was going. A row with no endpoint is a lease against a worker )"
           R"(that is no longer registered &mdash; dropping one releases its leases, but that happens when the next )"
           R"(lease is asked for, so an idle fleet can show these until somebody compiles again.</p></section>)";

    // ---- why requests were refused -----------------------------------------
    out += R"(<section><div class="sec-head"><h2>Why requests were refused</h2><span class="rule"></span>)"
           R"(<span class="meta">since this leader started</span></div><div class="reasons">)";
    for (auto const index: std::views::iota(std::size_t { 0 }, LeaseOutcomeTable.size()))
    {
        auto const& row = LeaseOutcomeTable[index];
        // **The series name, so the page and the counters are one vocabulary.** A tile
        // and its counter are the same fact under two names, and the mapping was
        // derivable but never stated -- an operator reading `no capacity` here had to
        // guess `fastcached_dispatch_leases_no_capacity_total` before they could grep
        // for it. Worth noting that #1306 itself guessed that name wrong (it wrote
        // `fastcache_`, which is in no tree), which is the argument for the change
        // made by the person making it.
        //
        // Read through `DescriptorOf` rather than stored beside the row: the typed
        // `counter` already IS the fact, and the header's static_assert is what makes
        // this dereference safe without a runtime check.
        out += std::format(R"(<div class="reason reason--{}"><div class="reason-n">{}</div>)"
                           R"(<div class="reason-k">{}</div><p class="reason-d">{}</p>)"
                           R"(<code class="reason-m">{}</code></div>)",
                           EscapeHtml(row.key),
                           CountAt(snapshot, index),
                           EscapeHtml(row.label),
                           EscapeHtml(row.meaning),
                           EscapeHtml(DescriptorOf(row.counter)->prometheusName));
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
        out += std::format(R"(<div class="panel wrap"><table><thead><tr><th>{}</th>)", TierEndpointColumn);
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

    // ---- forgotten clients --------------------------------------------------
    out += R"(<section><div class="sec-head"><h2>Forgotten clients</h2><span class="rule"></span>)"
           R"(<span class="meta">replicated tombstones</span></div>)";
    if (snapshot.cluster.has_value())
    {
        // The table is drawn whether or not the set is empty, exactly as Members is: a
        // section that renders its columns only when it has rows is one whose columns
        // reach the page in some states and not others, which is a coverage hole no test
        // over the tables can see.
        out += R"(<div class="panel wrap">)";
        AppendHtmlRows(out, ForgottenColumns, snapshot.cluster->forgotten);
        out += "</div>";
        if (snapshot.cluster->forgotten.empty())
            // And an empty set is a READING, so it says so: a bare empty table reads as a
            // section that failed to render.
            out += R"(<p class="note">The cluster has forgotten nobody. A forget is a positive act &mdash; )"
                   R"(--cluster-forget-client records a tombstone, which outranks every admission route )"
                   R"(including --fleet-open.</p>)";
    }
    else
        out += R"(<p class="note">This node runs no cluster: it leads itself, and has no replicated state.</p>)";
    out += "</section>";

    out += R"(<footer>/metrics remains the source of truth for anything alertable)";
    if (refreshSeconds != 0)
        out +=
            std::format(" &middot; this page refreshes every {}", FormatDuration(std::chrono::seconds { refreshSeconds }));
    out += "</footer>";

    out += "</div></body></html>";
    return out;
}

} // namespace FastCache::Distributed

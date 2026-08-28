// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetChart.hpp>
#include <FastCache/Distributed/FleetText.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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
        /// A lease age: the same pill on a far longer scale.
        ///
        /// Not `Freshness`, and the difference is the threshold rather than the
        /// drawing. A heartbeat is stale after fifteen seconds; a lease that old is
        /// an ordinary compile still running, so reusing that decor would paint
        /// every row amber and the colour would stop meaning anything at all.
        LeaseAge,
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

    /// The chip class per slot limit, in enumerator order.
    ///
    /// A table rather than the `if` ladder this used to be, and the ladder is why:
    /// it named `scratch` and `registered` and let everything else fall through to
    /// the CPU class, so a MEMORY-bound worker was dressed as "somebody else is
    /// using this machine" -- pointing an operator at the wrong remedy, which is
    /// the one thing `SlotLimitTraits::remedy` exists to get right. `EnumTable`
    /// takes its extent from the enum, so a fifth limit fails the build here
    /// instead of quietly inheriting a colour.
    constexpr EnumTable<SlotLimit, std::string_view> LimitChipClass {
        "chip--registered", "chip--cpu", "chip--memory", "chip--scratch"
    };

    // `EnumTable` fixes the extent, but aggregate initialization value-initializes
    // a row nobody wrote -- so an appended enumerator would silently get an EMPTY
    // class rather than a build error. This is the guard `RowsInEnumeratorOrder`
    // provides for tables whose rows name their own enumerator; these are bare
    // values, so the check is that none of them is missing.
    static_assert(std::ranges::none_of(LimitChipClass, [](std::string_view chipClass) { return chipClass.empty(); }),
                  "every SlotLimit needs a chip class");

    /// The chip class for a limit named the way `SlotLimitTable` spells it.
    ///
    /// Resolved through that table rather than by matching strings here, so the
    /// two cannot drift: the cell carries the limit's NAME, and its name is what
    /// the table already owns.
    ///
    /// @param limitName The cell's text.
    /// @return The chip class; `Registered`'s when nothing matches, which is the
    ///         reading that claims least.
    [[nodiscard]] std::string_view ChipClassFor(std::string_view limitName)
    {
        for (auto const& row: SlotLimitTable)
            if (row.name == limitName)
                return LimitChipClass[static_cast<std::size_t>(row.limit)];
        return LimitChipClass[static_cast<std::size_t>(SlotLimit::Registered)];
    }

    /// How the page spells "nobody reported this".
    ///
    /// The same dash `--cluster-status` prints, deliberately: an operator reading
    /// both surfaces should not have to learn that a blank, a zero and a dash are
    /// the same claim on one and different claims on the other.
    constexpr std::string_view AbsentText = "&ndash;";

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
        if (cell.kind == FleetCell::Kind::Absent)
            return std::string { AbsentText };
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
            .project =
                [](Cluster::ClusterMember const& m) {
                    return m.schedulerEndpoint.empty() ? FleetCell::Nothing() : FleetCell::Of(m.schedulerEndpoint);
                } },
    };

    /// What a node row shows, before its per-tier cache columns.
    constexpr std::array<FleetColumn<NodeReport>, 11> NodeColumns {
        FleetColumn<NodeReport> { .name = "endpoint",
                                  .help = "host:port the machine answers on.",
                                  .format = CellFormat::Text,
                                  .project = [](NodeReport const& n) { return FleetCell::Of(n.endpoint); } },
        FleetColumn<NodeReport> {
            .name = "version",
            .help = "Which build of fastcache-compile-node this machine is running. Absent "
                    "when the node predates the field and cannot report one.",
            .format = CellFormat::Text,
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
                                  .project = [](NodeReport const& n) { return FleetCell::Maybe(n.load.cpuBusyPermille); } },
        FleetColumn<NodeReport> {
            .name = "memory-available",
            .help = "Memory a new compile could get. Absent when unread.",
            .format = CellFormat::Bytes,
            .project = [](NodeReport const& n) { return FleetCell::Maybe(n.load.availableMemoryBytes); } },
        FleetColumn<NodeReport> { .name = "scratch-free",
                                  .help = "Room where compiles run. The limit that most often reaches zero.",
                                  .format = CellFormat::Bytes,
                                  .project = [](NodeReport const& n) { return FleetCell::Maybe(n.load.freeScratchBytes); } },
        FleetColumn<NodeReport> { .name = "cache-hit-rate",
                                  .help = "Reads this node's cache served. Absent when it has served none.",
                                  .format = CellFormat::Permille,
                                  .project = [](NodeReport const& n) { return HitRateOf(n.load.cache); } },
        FleetColumn<NodeReport> {
            .name = "heartbeat-age",
            .help = "Since this machine last reported. Everything on its row is that old.",
            .format = CellFormat::Millis,
            .decor = CellDecor::Freshness,
            // The column that tells "this cache is empty" from "this node stopped
            // answering an hour ago and these are its last figures" -- which look
            // identical without it, and lead to opposite conclusions.
            .project =
                [](NodeReport const& n) { return FleetCell::Of(static_cast<std::uint64_t>(n.heartbeatAge.count())); } },
    };

    /// What a worker row shows.
    constexpr std::array<FleetColumn<WorkerReport>, 9> WorkerColumns {
        FleetColumn<WorkerReport> { .name = "id",
                                    .help = "The id this leader assigned at registration.",
                                    .format = CellFormat::Text,
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
        FleetColumn<WorkerReport> {
            .name = "heartbeat-age",
            .help = "Since this entry last reported. A worker unheard-from is dropped.",
            .format = CellFormat::Millis,
            .decor = CellDecor::Freshness,
            .project =
                [](WorkerReport const& w) { return FleetCell::Of(static_cast<std::uint64_t>(w.heartbeatAge.count())); } },
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

        auto const ceilings = SlotCeilingsFor(node.capacity, node.registeredSlots, machineLoad);
        totals.registered += node.registeredSlots;
        totals.inFlight += node.fleetJobsInFlight;

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
        auto const ceiling = std::min(ceilings.available, node.registeredSlots);
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

    /// An age, as a pill that goes amber past `staleAfter`.
    ///
    /// One implementation for both age decors, which differ by their threshold and
    /// by nothing else. Two near-identical `case` bodies is how the pair comes to
    /// draw differently for no reason anybody intended.
    /// @param text The already-formatted value.
    /// @param millis The age, for the comparison.
    /// @param staleAfter Where the pill turns amber.
    /// @return The cell's inner HTML.
    [[nodiscard]] std::string AgePill(std::string_view text, std::uint64_t millis, std::uint64_t staleAfter)
    {
        auto const* const tone = millis >= staleAfter ? "pill--warn" : "pill--ok";
        return std::format(R"(<span class="pill pill--value {}"><span class="dot"></span>{}</span>)", tone, text);
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
                return AgePill(text, cell.number, HeartbeatStaleAfterMillis);
            case CellDecor::LeaseAge:
                return AgePill(text, cell.number, LeaseOldAfterMillis);
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
    /// `value` is already safe to interpolate: every projector below produces either
    /// a number it formatted itself or `AbsentText`, and neither can carry markup.
    struct KpiReadout
    {
        std::string value; ///< The figure, or the dash.
        std::string unit;  ///< The small suffix beside it; empty for none.
        std::string sub;   ///< The line under it.
    };

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

    /// A figure or the dash, at one decimal place with a unit.
    [[nodiscard]] std::string FigureOr(std::optional<double> value, int decimals)
    {
        if (!value.has_value())
            return std::string { AbsentText };
        return decimals == 0 ? std::format("{:.0f}", *value) : std::format("{:.1f}", *value);
    }

    KpiReadout KpiDispatched(FleetSnapshot const& /*snapshot*/, FleetHistoryView const& history)
    {
        return KpiReadout { .value = FigureOr(FoldedSeries("dispatched", history), 0),
                            .unit = {},
                            .sub = std::format("compiles in the last {}", RangeKeyOf(history)) };
    }

    KpiReadout KpiCompilingNow(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        auto const totals = TotalsFor(snapshot);
        // The sub-line names WHICH zero this is. "This fleet's own work" over a 0
        // reads as an idle fleet, and on a node nothing dispatches to it is the
        // tile an operator stares at while their build saturates the machine.
        return KpiReadout { .value = std::to_string(totals.inFlight),
                            .unit = std::format("/ {} slots", totals.registered),
                            .sub = NeverDispatched(snapshot, totals) ? "nothing dispatched yet" : "this fleet's own work" };
    }

    KpiReadout KpiHitRate(FleetSnapshot const& /*snapshot*/, FleetHistoryView const& history)
    {
        auto const share = FoldedSeries("hit-rate", history);
        return KpiReadout { .value = FigureOr(share, 1),
                            .unit = share.has_value() ? "%" : "",
                            .sub = std::format("over the last {}", RangeKeyOf(history)) };
    }

    KpiReadout KpiRefused(FleetSnapshot const& /*snapshot*/, FleetHistoryView const& history)
    {
        auto const share = RefusedShare(history);
        return KpiReadout { .value = FigureOr(share, 1),
                            .unit = share.has_value() ? "%" : "",
                            .sub = "of dispatch decisions" };
    }

    KpiReadout KpiLeases(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        // "Not yet resolved", not "not yet claimed". Nothing claimed a lease and
        // nothing ever could -- the only way one left this figure was by expiring,
        // ten minutes after the job it named had finished, so on a busy fleet it
        // read as a backlog that did not exist. A client now hands its lease back
        // when its job ends (#212), which is what makes this a live number.
        return KpiReadout { .value = std::to_string(snapshot.liveLeases), .unit = {}, .sub = "granted, not yet resolved" };
    }

    KpiReadout KpiOldestHeartbeat(FleetSnapshot const& snapshot, FleetHistoryView const& /*history*/)
    {
        if (snapshot.nodes.empty())
            return KpiReadout { .value = std::string { AbsentText }, .unit = {}, .sub = "no machine registered" };
        auto const oldest = std::ranges::max(snapshot.nodes, {}, &NodeReport::heartbeatAge).heartbeatAge;
        // The *oldest*, not the mean: one machine that stopped answering an hour ago
        // is the fact worth surfacing, and an average over a healthy fleet buries it.
        return KpiReadout { .value = HumanMillis(static_cast<std::uint64_t>(oldest.count())),
                            .unit = {},
                            .sub = std::format("across {} machine(s)", snapshot.nodes.size()) };
    }

    /// One readout on the strip.
    ///
    /// A table rather than six calls: the strip is the part of this page most likely
    /// to grow a seventh tile, and a tile added as a seventh call is one whose label
    /// case, absent spelling and order are checked by nothing.
    struct KpiRow
    {
        std::string_view label;                                               ///< What the tile is called.
        KpiReadout (*project)(FleetSnapshot const&, FleetHistoryView const&); ///< What it reads.
        bool sparkline;                                                       ///< Whether it carries one.
    };

    /// The strip, in the order it is read. The mockup's six, in the mockup's order.
    constexpr std::array<KpiRow, 6> KpiTable {
        KpiRow { .label = "Dispatched", .project = KpiDispatched, .sparkline = true },
        KpiRow { .label = "Compiling now", .project = KpiCompilingNow, .sparkline = false },
        KpiRow { .label = "Cache hit rate", .project = KpiHitRate, .sparkline = false },
        KpiRow { .label = "Refused", .project = KpiRefused, .sparkline = false },
        KpiRow { .label = "Leases outstanding", .project = KpiLeases, .sparkline = false },
        KpiRow { .label = "Oldest heartbeat", .project = KpiOldestHeartbeat, .sparkline = false },
    };

    /// The sentence a split of numbers needs beside it.
    constexpr std::string_view LeaseNote =
        "Do not add these together. An empty fleet, a busy one, machines somebody else is using and an object "
        "already being built are four different problems with four different fixes, and a total hides all of them.";
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
        out += std::format(R"(</span><span class="rule"></span><span class="meta">{} &middot; {}</span></div>)",
                           EscapeHtml(rangeRow.bucketLabel),
                           history.durable ? "kept on disk" : "kept in memory only");
        if (std::ranges::none_of(history.buckets, [](auto const& bucket) { return bucket.present; }))
            // A frame with nothing in it reads as a broken chart. Absent is not zero
            // here either: nobody was watching, which is not a fleet that did nothing.
            out += R"(<div class="panel occ"><p class="note">Nothing has been recorded for this range yet. )"
                   R"(A node samples the fleet once a minute <strong>while it leads</strong> &mdash; a follower's )"
                   R"(registry holds only what registered against it, so sampling there would record a fraction )"
                   R"(as though it were the whole. The first points appear a minute after this node won the )"
                   R"(election.</p></div>)";
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
                           R"(zero says the fleet did nothing, a gap says nobody was watching. {} The same series )"
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

std::string RenderFleetHtml(FleetSnapshot const& snapshot, FleetHistoryView const& history, unsigned refreshSeconds)
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
    //
    // Free and withheld are deliberately *not* here: they are the capacity meter's
    // two segments directly below, and a number repeated a hand's width from the
    // picture of itself is a number that will one day disagree with it.
    out += R"(<section><div class="kpis">)";
    for (auto const& row: KpiTable)
    {
        auto const readout = row.project(snapshot, history);
        out += std::format(R"(<div class="kpi"><span class="kpi-label">{}</span>)"
                           R"(<span class="kpi-value">{}<small>{}</small></span>)",
                           EscapeHtml(row.label),
                           readout.value,
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

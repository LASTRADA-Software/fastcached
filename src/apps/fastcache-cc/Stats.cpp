// SPDX-License-Identifier: Apache-2.0
#include "Stats.hpp"

#include <FastCache/Core/EnumTable.hpp>
// Header-only and std-only, so it adds no row to `_fc_cc_core` -- which the
// launcher's CMakeLists is strict about, since it does not link the library.
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Platform/Environment.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace FastCache::Cc
{
namespace
{

    constexpr char FieldSeparator = '\t';

    /// SGR escapes for the terminal report's hit/miss/warning cues.
    ///
    /// A dedicated palette rather than reusing `Cli::UsagePalette`: that one
    /// names *usage-doc* roles (heading, term) that have nothing to do with
    /// outcome coloring, and folding unrelated concepts into one struct would
    /// make neither caller's intent legible. Every field is empty in the plain
    /// palette, the same trick `UsagePalette` uses, so one render path serves
    /// both and color never disturbs the sparkline/column alignment.
    struct StatsPalette
    {
        std::string_view reset;
        std::string_view good;    ///< Hits, direct hits — the cache doing its job.
        std::string_view bad;     ///< Unavailable / "CACHE NOT REACHED".
        std::string_view neutral; ///< Misses, uncacheable — expected, not alarming.
    };

    constexpr StatsPalette ColoredStatsPalette {
        .reset = "\x1b[0m",
        .good = "\x1b[32m",    // green
        .bad = "\x1b[31m",     // red
        .neutral = "\x1b[33m", // yellow
    };
    constexpr StatsPalette PlainStatsPalette {};

    /// @param color Whether to emit ANSI SGR escapes.
    /// @return The matching palette.
    [[nodiscard]] constexpr StatsPalette const& StatsPaletteFor(UsageColor color) noexcept
    {
        return color == UsageColor::Colored ? ColoredStatsPalette : PlainStatsPalette;
    }

    /// Wrap `text` in `color`/`reset` when `color` is non-empty; otherwise
    /// return it unchanged. Centralizes the "only colorize when there is a
    /// color to apply" check every call site would otherwise repeat.
    [[nodiscard]] std::string Colorize(std::string_view text, std::string_view color, std::string_view reset)
    {
        if (color.empty())
            return std::string { text };
        return std::string { color } + std::string { text } + std::string { reset };
    }

    /// Replace characters that would corrupt the one-record-per-line format.
    [[nodiscard]] std::string Sanitize(std::string_view text)
    {
        std::string out { text };
        std::ranges::replace(out, '\t', ' ');
        std::ranges::replace(out, '\r', ' ');
        std::ranges::replace(out, '\n', ' ');
        return out;
    }

    /// Directory holding the log, created on demand. Empty on failure.
    [[nodiscard]] std::filesystem::path StateDirectoryImpl()
    {
        std::error_code ec;
        // The platform `#if` stays: these are genuinely different variables per
        // platform, not a different way of reading them. The reading itself goes
        // through the one seam that knows about getenv_s.
        std::string base;
#if defined(_WIN32)
        if (auto const local = FastCache::ReadEnvironmentVariable("LOCALAPPDATA"); local.has_value())
            base = *local;
#else
        // XDG_STATE_HOME wins when set; otherwise the spec's default location
        // under $HOME. Neither present means we have nowhere to record.
        auto const xdg = FastCache::ReadEnvironmentVariable("XDG_STATE_HOME");
        auto const home = FastCache::ReadEnvironmentVariable("HOME");
        if (xdg.has_value() && !xdg->empty())
            base = *xdg;
        else if (home.has_value())
            base = *home + "/.local/state";
#endif
        if (base.empty())
            return {};
        std::filesystem::path dir = std::filesystem::path { base } / "fastcache-cc";
        std::filesystem::create_directories(dir, ec);
        return ec ? std::filesystem::path {} : dir;
    }

    /// How far one `DispatchOutcome` got towards a worker.
    ///
    /// **Ordered, and the ordering is the whole of it.** Every question either
    /// report asks about a state is a threshold on this axis, so a pair of
    /// independent booleans could spell a row that asked the fleet without having
    /// one — a state with no meaning, which an assertion can only reject after
    /// somebody has written it. One ordered column makes it unrepresentable.
    enum class FleetReach : std::uint8_t
    {
        /// No fleet is in evidence: a launcher with no scheduler, or a record
        /// written before this axis existed. Counted nowhere and printed nowhere,
        /// which is what makes "render no distribution section at all" the answer
        /// for a machine that does not distribute — absent is not zero, and a
        /// launcher that never distributes must not read as one that failed to.
        NoFleet,
        /// Distribution was configured and this compile did not reach a worker: the
        /// cache served it, or this launcher refused to send it. Printed, but never
        /// in the denominator — it says nothing about the fleet.
        NotAsked,
        /// The fleet was asked. `Dispatched` is rated against the sum of these, so a
        /// build that mostly hit the cache does not read as a fleet that mostly
        /// refused.
        Asked,
    };

    /// How one `DispatchOutcome` is written to the log and rendered in the reports.
    ///
    /// A row rather than a switch per question: the log token, the report label, the
    /// colour and the reach are four facts that have to move together, and a state
    /// added with only three of them filled in is exactly the half-registration this
    /// tree keeps paying for. The two byte-wide members sit together at the end,
    /// which is this tree's layout rule wherever a struct mixes them with pointers.
    struct DispatchRow
    {
        std::string_view token; ///< The stable log token, written and parsed back.
        std::string_view label; ///< How the terminal and HTML reports name this state.
        /// Which palette entry colours it. A member pointer rather than a colour per
        /// branch, so the renderer stays one loop over the table; the palette itself
        /// is chosen at run time, so the row cannot hold the escape directly.
        std::string_view StatsPalette::* tone;
        DispatchOutcome outcome {};
        FleetReach reach {}; ///< How far this state got; see `FleetReach`.
    };

    constexpr EnumTable<DispatchOutcome, DispatchRow> DispatchTable { {
        DispatchRow { .token = "UNKNOWN",
                      .label = "unknown",
                      .tone = &StatsPalette::neutral,
                      .outcome = DispatchOutcome::Unknown,
                      .reach = FleetReach::NoFleet },
        DispatchRow { .token = "NOT_CONFIGURED",
                      .label = "no scheduler",
                      .tone = &StatsPalette::neutral,
                      .outcome = DispatchOutcome::NotConfigured,
                      .reach = FleetReach::NoFleet },
        DispatchRow { .token = "NOT_ATTEMPTED",
                      .label = "not attempted",
                      .tone = &StatsPalette::neutral,
                      .outcome = DispatchOutcome::NotAttempted,
                      .reach = FleetReach::NotAsked },
        DispatchRow { .token = "REFUSED",
                      .label = "refused here",
                      .tone = &StatsPalette::neutral,
                      .outcome = DispatchOutcome::Refused,
                      .reach = FleetReach::NotAsked },
        DispatchRow { .token = "DECLINED",
                      .label = "fleet declined",
                      .tone = &StatsPalette::neutral,
                      .outcome = DispatchOutcome::Declined,
                      .reach = FleetReach::Asked },
        DispatchRow { .token = "UNREACHABLE",
                      .label = "unreachable",
                      .tone = &StatsPalette::bad,
                      .outcome = DispatchOutcome::Unreachable,
                      .reach = FleetReach::Asked },
        DispatchRow { .token = "MISMATCHED",
                      .label = "crossed reply",
                      .tone = &StatsPalette::bad,
                      .outcome = DispatchOutcome::Mismatched,
                      .reach = FleetReach::Asked },
        DispatchRow { .token = "DISCARDED",
                      .label = "result discarded",
                      .tone = &StatsPalette::bad,
                      .outcome = DispatchOutcome::Discarded,
                      .reach = FleetReach::Asked },
        DispatchRow { .token = "DISPATCHED",
                      .label = "dispatched",
                      .tone = &StatsPalette::good,
                      .outcome = DispatchOutcome::Dispatched,
                      .reach = FleetReach::Asked },
    } };
    static_assert(RowsInEnumeratorOrder(DispatchTable, &DispatchRow::outcome),
                  "DispatchTable must hold exactly one row per DispatchOutcome, in enumerator order");

    /// @param outcome The dispatch outcome. @return Its row.
    [[nodiscard]] constexpr DispatchRow const& RowFor(DispatchOutcome outcome) noexcept
    {
        return DispatchTable[static_cast<std::size_t>(outcome)];
    }

    /// How one `DispatchStatus` — what `Dispatch` actually returned — is recorded on
    /// the axis above.
    ///
    /// A table rather than a `switch`, so a fifth dispatch status is a row and cannot
    /// arrive without somebody deciding what an operator reads about it, which is
    /// exactly how `Mismatched` would otherwise be tallied as a network blip.
    struct DispatchRecordingRow
    {
        /// The FIXED tally reason, and deliberately NOT `DispatchResult::detail`:
        /// `Dispatch` formats the worker's endpoint into its declined and unreachable
        /// messages, so forwarding that text would produce one tally row per machine
        /// in the fleet rather than one per cause. The endpoint reaches an operator on
        /// the verbose line beside the call instead.
        std::string_view reason;
        DispatchStatus status {};   ///< Which status this row describes.
        DispatchOutcome outcome {}; ///< How the reports bucket it.
    };

    constexpr EnumTable<DispatchStatus, DispatchRecordingRow> DispatchRecordingTable { {
        DispatchRecordingRow { .reason = {}, .status = DispatchStatus::Compiled, .outcome = DispatchOutcome::Dispatched },
        DispatchRecordingRow { .reason = "the fleet declined this compile",
                               .status = DispatchStatus::Declined,
                               .outcome = DispatchOutcome::Declined },
        DispatchRecordingRow { .reason = "the fleet could not be reached",
                               .status = DispatchStatus::Unavailable,
                               .outcome = DispatchOutcome::Unreachable },
        // No reason, and here that is a decision rather than the absence of a
        // failure. The launcher already puts #280's sentence on the CACHE axis, which
        // that rule requires -- a copy here would print the identical sentence under
        // two headings of one report, and two rankings of one event read as two
        // events. The state says it: `crossed reply` is a line no other state emits.
        DispatchRecordingRow { .reason = {}, .status = DispatchStatus::Mismatched, .outcome = DispatchOutcome::Mismatched },
    } };
    static_assert(RowsInEnumeratorOrder(DispatchRecordingTable, &DispatchRecordingRow::status),
                  "DispatchRecordingTable must hold exactly one row per DispatchStatus, in enumerator order");

    /// Widest label the distribution section can print.
    ///
    /// Over the rows that are actually printed: a `NoFleet` row never is, so folding
    /// its label in would pad every line for text that never appears. Derived rather
    /// than counted by eye, or a longer state added later silently breaks the
    /// alignment of every line above it.
    [[nodiscard]] consteval std::size_t WidestDispatchLabel()
    {
        std::size_t widest = 0;
        for (auto const& row: DispatchTable)
            if (row.reach != FleetReach::NoFleet)
                widest = (std::max) (widest, row.label.size());
        return widest;
    }
    constexpr std::size_t DispatchLabelWidth = WidestDispatchLabel();

    /// Decode a log line's dispatch token.
    ///
    /// `FindOrNull` rather than `std::ranges::find`: the table is a `std::array`,
    /// whose iterator is a raw pointer on libc++ and a class type on the MSVC STL,
    /// and every spelling of the local either trips `readability-qualified-auto` on
    /// one or `modernize-use-auto` on the other. `Core/Ranges.hpp` exists to resolve
    /// that once — this file learnt it the way its header says people do.
    ///
    /// @param token The token as written.
    /// @return The state it names, or `Unknown` for anything this build does not
    ///         recognise — a token from a LATER build says nothing this one can
    ///         report on, and guessing at it would be a claim about a fleet.
    [[nodiscard]] DispatchOutcome ParseDispatchOutcome(std::string_view token)
    {
        auto const* const row = FindOrNull(DispatchTable, token, &DispatchRow::token);
        return row != nullptr ? row->outcome : DispatchOutcome::Unknown;
    }

    /// Per-group tallies folded from the log.
    struct Tally
    {
        std::uint64_t hits {};
        std::uint64_t misses {};
        std::uint64_t uncacheable {};
        std::uint64_t unavailable {};

        // Full sample sets, not running sums: the point of the distribution is the
        // shape (a bimodal miss profile means something an average hides).
        std::vector<std::uint64_t> hitMs;
        std::vector<std::uint64_t> missMs;

        // Phase split of the hit path. A hit still preprocesses to derive its key,
        // so total hit latency alone cannot say whether the cache or the compiler
        // front end is the slow part.
        std::vector<std::uint64_t> hitPreprocessMs;
        std::vector<std::uint64_t> hitCacheMs;

        /// Hits served through the manifest shortcut, and how long validating it
        /// took. Split out because these hits skip preprocessing entirely, so
        /// averaging them with preprocessed hits would hide the improvement.
        std::uint64_t directHits {};
        std::vector<std::uint64_t> directMs;

        /// Fall-back reason -> count, so an "unavailable" figure is actionable.
        std::map<std::string, std::uint64_t> reasons;

        /// One counter per `DispatchOutcome`, indexed by the enumerator itself.
        ///
        /// An array rather than a named member per state: every consumer wants the
        /// whole vocabulary at once — the report walks it, and both "does this
        /// machine distribute at all" and "what fraction of what we asked for came
        /// back" are subset sums over it — so a member each would be one more place
        /// to forget when the next state is added.
        EnumTable<DispatchOutcome, std::uint64_t> dispatch {};

        /// Dispatch fall-back reason -> count, kept apart from `reasons` above
        /// because a fleet failure and a cache failure are fixed in different
        /// places; see `Record::dispatchDetail`.
        std::map<std::string, std::uint64_t> dispatchReasons;

        [[nodiscard]] std::uint64_t Total() const noexcept
        {
            return hits + misses + uncacheable + unavailable;
        }

        /// Sum the per-state dispatch counters whose row has @p column set.
        ///
        /// One function taking the threshold rather than one per question: the two
        /// callers differ only by where they cut the axis, which is the copy-paste
        /// this project's guidelines name outright.
        /// @param atLeast How far a state must have got to be counted.
        /// @return The total across every state that got at least that far.
        [[nodiscard]] std::uint64_t DispatchTotal(FleetReach atLeast) const noexcept
        {
            std::uint64_t total = 0;
            for (auto const& row: DispatchTable)
                if (row.reach >= atLeast)
                    total += dispatch[static_cast<std::size_t>(row.outcome)];
            return total;
        }
    };

    /// One line of the distribution section: a state and how many compiles it holds.
    struct DispatchLine
    {
        /// Non-owning, into the `constexpr` table above, which has static storage
        /// duration and outlives every caller.
        DispatchRow const* row;
        std::uint64_t count;
    };

    /// Which rows a distribution report shows, and with what counts.
    ///
    /// **The selection lives here and only the formatting lives in each renderer.**
    /// The two reports differ in how they print and must not differ in what they
    /// print — written out per renderer, that is a promise nothing enforces, and the
    /// zero-suppression rule below would have to be changed in two places to stay
    /// true in one.
    ///
    /// Empty when no record came from a launcher that had a fleet to ask, which is
    /// how both reports spell "render nothing at all". That is the load-bearing
    /// half: a launcher with no scheduler must report NO fleet, never a fleet that
    /// failed every time — an absence rendered as a total failure is the defect
    /// `NoUpstream`'s honest `false` produced when a node with no shared cache
    /// reported a 100% upstream failure rate.
    ///
    /// @param tally The folded records.
    /// @return The lines to print, in enumerator order.
    [[nodiscard]] std::vector<DispatchLine> VisibleDispatchLines(Tally const& tally)
    {
        if (tally.DispatchTotal(FleetReach::NotAsked) == 0)
            return {};

        std::vector<DispatchLine> lines;
        for (auto const& row: DispatchTable)
        {
            if (row.reach == FleetReach::NoFleet)
                continue;
            auto const count = tally.dispatch[static_cast<std::size_t>(row.outcome)];
            // A zero state is dropped rather than printed: with this many of them,
            // listing every zero buries the one or two an operator came to read.
            // `Dispatched` is the deliberate exception -- a fleet that dispatched
            // NOTHING is precisely the case this section exists to make visible, and
            // omitting its line would read as "no data" rather than as "none".
            if (count == 0 && row.outcome != DispatchOutcome::Dispatched)
                continue;
            lines.push_back(DispatchLine { .row = &row, .count = count });
        }
        return lines;
    }

    [[nodiscard]] std::uint64_t ParseUnsigned(std::string_view text)
    {
        std::uint64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }

    [[nodiscard]] Outcome ParseOutcome(std::string_view token)
    {
        if (token == "HIT")
            return Outcome::Hit;
        if (token == "MISS")
            return Outcome::Miss;
        if (token == "UNCACHEABLE")
            return Outcome::Uncacheable;
        return Outcome::Unavailable;
    }

    /// Rebuild a `Record` from one log line's already-split fields.
    ///
    /// Every trailing field predates the one before it in the log's history —
    /// the phase columns, the direct-hit flag, the timestamp — so each is only
    /// read when present, exactly mirroring what `AppendRecord` would have
    /// written at that time. This is the one place that decodes a line; both
    /// `ParseLog` and (through it) `FormatReport` fold over its output rather
    /// than re-reading fields themselves.
    [[nodiscard]] Record DecodeFields(std::vector<std::string_view> const& fields)
    {
        Record record;
        record.outcome = ParseOutcome(fields[0]);
        record.prefetchGroup = std::string { fields[1] };
        record.valueBytes = ParseUnsigned(fields[2]);
        record.elapsedMs = ParseUnsigned(fields[3]);
        if (fields.size() >= 5)
            record.source = std::string { fields[4] };
        if (fields.size() >= 6)
            record.detail = std::string { fields[5] };
        if (fields.size() >= 8)
        {
            record.preprocessMs = ParseUnsigned(fields[6]);
            record.cacheMs = ParseUnsigned(fields[7]);
            record.hasPhaseColumns = true;
        }
        if (fields.size() >= 9)
            record.directMs = ParseUnsigned(fields[8]);
        if (fields.size() >= 10)
            record.directHit = fields[9] == "1";
        if (fields.size() >= 11)
            record.timestampUnixSeconds = ParseUnsigned(fields[10]);
        // The dispatch axis is the newest pair of columns. A line without them
        // leaves `dispatch` at `Unknown`, which is the point: a pre-upgrade record
        // is silent about distribution, and reading that silence as `NotConfigured`
        // would turn a missing column into a claim that the operator has no fleet.
        if (fields.size() >= 12)
            record.dispatch = ParseDispatchOutcome(fields[11]);
        if (fields.size() >= 13)
            record.dispatchDetail = std::string { fields[12] };
        return record;
    }

    /// Split a log line into its tab-separated fields.
    [[nodiscard]] std::vector<std::string_view> SplitFields(std::string_view line)
    {
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (start <= line.size())
        {
            auto const end = line.find(FieldSeparator, start);
            if (end == std::string_view::npos)
            {
                fields.emplace_back(line.substr(start));
                break;
            }
            fields.emplace_back(line.substr(start, end - start));
            start = end + 1;
        }
        return fields;
    }

    /// Rank a name -> count tally most-frequent first.
    ///
    /// Five call sites want exactly this — the cache's fall-back reasons, the
    /// dispatch axis's, and the never-cached attribution, each in both reports — and
    /// it was written out at each of them.
    /// @param counts The tally.
    /// @return The pairs, descending by count.
    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> RankedByCount(
        std::map<std::string, std::uint64_t> const& counts)
    {
        std::vector<std::pair<std::string, std::uint64_t>> ranked { counts.begin(), counts.end() };
        std::ranges::sort(ranked, [](auto const& a, auto const& b) { return a.second > b.second; });
        return ranked;
    }

    /// Render a percentage with one decimal, guarding the empty case.
    [[nodiscard]] std::string Percent(std::uint64_t part, std::uint64_t whole)
    {
        if (whole == 0)
            return "n/a";
        double const pct = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
        std::array<char, 32> buffer {};
        auto const written = std::snprintf(buffer.data(), buffer.size(), "%.1f%%", pct);
        return written > 0 ? std::string { buffer.data(), static_cast<std::size_t>(written) } : "n/a";
    }

    /// Format one millisecond boundary compactly: sub-second in ms, above in s with a
    /// decimal only where the value is not a whole second.
    [[nodiscard]] std::string FormatMs(std::uint64_t ms)
    {
        std::array<char, 32> buffer {};
        int written = 0;
        if (ms < 1000)
            written = std::snprintf(buffer.data(), buffer.size(), "%llums", static_cast<unsigned long long>(ms));
        else if (ms % 1000 == 0)
            written = std::snprintf(buffer.data(), buffer.size(), "%llus", static_cast<unsigned long long>(ms / 1000));
        else
            written = std::snprintf(buffer.data(), buffer.size(), "%.1fs", static_cast<double>(ms) / 1000.0);
        return written > 0 ? std::string { buffer.data(), static_cast<std::size_t>(written) } : "?";
    }

    /// The percentile of a sorted sample set, by nearest-rank.
    [[nodiscard]] std::uint64_t Percentile(std::vector<std::uint64_t> const& sorted, double fraction)
    {
        if (sorted.empty())
            return 0;
        auto const scaled = fraction * static_cast<double>(sorted.size() - 1);
        auto const index = static_cast<std::size_t>(std::lround(scaled));
        return sorted[index < sorted.size() ? index : sorted.size() - 1];
    }

    /// Ramp of ASCII characters, ordered by visual weight, so one character conveys a
    /// bucket's height.
    ///
    /// Deliberately ASCII rather than Unicode block elements: the launcher writes to
    /// whatever console the build runs in, and a Windows console using a legacy ANSI
    /// code page renders UTF-8 block bytes as mojibake (`▄` arrives as `Γûä`), which
    /// made the report unreadable in exactly the place it is most used.
    constexpr std::array<char, 8> RampGlyphs { '.', ':', '-', '=', '+', '*', '#', '@' };

    /// Render one latency sample set as a single-line sparkline over buckets derived
    /// from the data itself.
    ///
    /// The bucket edges are linear across the observed min..max rather than a fixed
    /// ladder: a fixed ladder's widest bucket swallowed whatever it could not
    /// resolve, so a 2 s hit and a 240 s hit both read as ">10s" and the shape of a
    /// tight cluster collapsed into one bar. Deriving the edges keeps the resolution
    /// on the range that actually occurred.
    void AppendHistogram(std::ostringstream& out,
                         std::string_view title,
                         std::vector<std::uint64_t> const& samples,
                         std::string_view indent)
    {
        if (samples.empty())
            return;

        std::vector<std::uint64_t> sorted { samples };
        std::ranges::sort(sorted);
        auto const low = sorted.front();
        auto const high = sorted.back();

        // A degenerate range (every sample identical) has no shape to plot; the
        // summary line still carries the value, so report it without a sparkline.
        if (low == high)
        {
            out << indent << title << "  " << samples.size() << " samples  all " << FormatMs(low) << '\n';
            return;
        }

        constexpr std::size_t BucketCount = 24;
        std::array<std::uint64_t, BucketCount> counts {};
        auto const span = high - low;
        for (std::uint64_t const sample: samples)
        {
            // Scale into [0, BucketCount-1]; the max sample lands in the last bucket.
            auto const offset = static_cast<double>(sample - low) / static_cast<double>(span);
            auto index = static_cast<std::size_t>(offset * static_cast<double>(BucketCount));
            if (index >= BucketCount)
                index = BucketCount - 1;
            ++counts[index];
        }

        auto const peak = *std::ranges::max_element(counts);
        if (peak == 0)
            return;

        std::string spark;
        for (auto const count: counts)
        {
            if (count == 0)
            {
                spark += ' ';
                continue;
            }
            // Non-empty buckets start at glyph 1, so a single outlier stays visible
            // next to a tall neighbour instead of rendering as the faintest mark.
            auto const scaled =
                (static_cast<double>(count) / static_cast<double>(peak)) * static_cast<double>(RampGlyphs.size() - 1);
            auto const glyph = static_cast<std::size_t>(std::lround(scaled));
            spark += RampGlyphs[glyph < RampGlyphs.size() ? glyph : RampGlyphs.size() - 1];
        }

        out << indent << title << "  " << samples.size() << " samples  " << FormatMs(low) << '-' << FormatMs(high) << '\n';
        out << indent << "  " << spark << "  p50=" << FormatMs(Percentile(sorted, 0.50))
            << " p95=" << FormatMs(Percentile(sorted, 0.95)) << " max=" << FormatMs(high) << '\n';
    }

    /// Render the distribution axis — or nothing at all when there is no fleet.
    ///
    /// Which rows appear and why is `VisibleDispatchLines`; this is only how they
    /// look in a terminal.
    /// @param out Report stream.
    /// @param tally The folded records.
    /// @param palette Colours, or the empty palette for plain output.
    void AppendDispatchLines(std::ostringstream& out, Tally const& tally, StatsPalette const& palette)
    {
        auto const lines = VisibleDispatchLines(tally);
        if (lines.empty())
            return;

        auto const asked = tally.DispatchTotal(FleetReach::Asked);
        out << "  distribution\n";
        for (auto const& [row, count]: lines)
        {
            out << "    " << std::format("{:<{}}", row->label, DispatchLabelWidth) << ": "
                << Colorize(std::to_string(count), palette.*row->tone, palette.reset);
            if (row->outcome == DispatchOutcome::Dispatched)
                out << "  (" << Percent(count, asked) << " of " << asked << " asked of the fleet)";
            out << '\n';
        }

        if (!tally.dispatchReasons.empty())
        {
            out << "    why distribution did not help\n";
            for (auto const& [reason, count]: RankedByCount(tally.dispatchReasons))
                out << "      " << Colorize(std::to_string(count) + "x", palette.bad, palette.reset) << "  " << reason
                    << '\n';
        }
    }

    void AppendTallyLines(std::ostringstream& out, Tally const& tally, StatsPalette const& palette)
    {
        // Rate the cache against the compiles it could actually serve. Dividing by
        // every invocation blends "the cache did not have it" with "the cache was
        // unreachable", so an outage reads as a poor hit rate and hides its own cause.
        auto const servable = tally.hits + tally.misses;

        out << "  compiles     : " << tally.Total() << '\n'
            << "  hits         : " << Colorize(std::to_string(tally.hits), palette.good, palette.reset) << "  ("
            << Percent(tally.hits, servable) << " of " << servable << " cacheable)\n";
        if (tally.directHits > 0)
            out << "    via direct : " << tally.directHits << "  (" << Percent(tally.directHits, tally.hits)
                << " of hits, no preprocess)\n";
        out << "  misses       : " << Colorize(std::to_string(tally.misses), palette.neutral, palette.reset) << '\n';
        if (tally.uncacheable > 0)
            out << "  uncacheable  : " << Colorize(std::to_string(tally.uncacheable), palette.neutral, palette.reset)
                << '\n';
        if (tally.unavailable > 0)
            out << "  unavailable  : " << Colorize(std::to_string(tally.unavailable), palette.bad, palette.reset) << "  ("
                << Percent(tally.unavailable, tally.Total()) << " of all compiles -- "
                << Colorize("CACHE NOT REACHED", palette.bad, palette.reset) << ")\n";

        if (!tally.reasons.empty())
        {
            out << "  fall-back reasons\n";
            for (auto const& [reason, count]: RankedByCount(tally.reasons))
                out << "    " << Colorize(std::to_string(count) + "x", palette.bad, palette.reset) << "  " << reason << '\n';
        }

        AppendDispatchLines(out, tally, palette);

        if (!tally.hitMs.empty() || !tally.missMs.empty())
        {
            out << '\n';
            AppendHistogram(out, "hit latency  ", tally.hitMs, "  ");
            // Attribute a slow hit: preprocessing to derive the key is unavoidable
            // work the cache cannot remove, so showing it beside the cache time says
            // whether tuning the daemon can help at all.
            AppendHistogram(out, "  preprocess ", tally.hitPreprocessMs, "  ");
            AppendHistogram(out, "  cache i/o  ", tally.hitCacheMs, "  ");
            AppendHistogram(out, "  direct val ", tally.directMs, "  ");
            if (!tally.hitMs.empty() && !tally.missMs.empty())
                out << '\n';
            AppendHistogram(out, "miss latency ", tally.missMs, "  ");
        }
    }

} // namespace

std::string_view ToStringView(Outcome outcome) noexcept
{
    switch (outcome)
    {
        case Outcome::Hit:
            return "HIT";
        case Outcome::Miss:
            return "MISS";
        case Outcome::Uncacheable:
            return "UNCACHEABLE";
        case Outcome::Unavailable:
            break;
    }
    return "UNAVAILABLE";
}

DispatchRecording RecordingFor(DispatchStatus status) noexcept
{
    // `Last` is an ordinary enumerator as far as the language is concerned, so a
    // caller can name it and every in-range promise here is prose. Answering
    // `Unknown` rather than reading past the table is the same answer this file
    // already gives one layer down, where a token from a later build says nothing
    // this build can report on -- an unnameable status is that situation reached
    // from the other side.
    if (static_cast<std::size_t>(status) >= EnumeratorCount<DispatchStatus>)
        return DispatchRecording { .reason = {}, .outcome = DispatchOutcome::Unknown };
    auto const& row = DispatchRecordingTable[static_cast<std::size_t>(status)];
    return DispatchRecording { .reason = row.reason, .outcome = row.outcome };
}

std::string_view ToStringView(DispatchOutcome outcome) noexcept
{
    // Read off the same table the parser searches, so a token cannot be written in
    // one spelling and read back in another -- which is a whole axis of the report
    // silently reading as `Unknown` while every test that checks one direction
    // passes.
    //
    // Out of range answers with the `Unknown` token rather than reading past the
    // table: `Last` is an ordinary enumerator to the language, and a log line is the
    // wrong place to discover that. Round-trips honestly, since that token parses
    // back to `Unknown`.
    if (static_cast<std::size_t>(outcome) >= EnumeratorCount<DispatchOutcome>)
        return RowFor(DispatchOutcome::Unknown).token;
    return RowFor(outcome).token;
}

std::filesystem::path StateDirectory()
{
    return StateDirectoryImpl();
}

std::string LogPath()
{
    auto const dir = StateDirectoryImpl();
    if (dir.empty())
        return {};
    return (dir / "invocations.log").string();
}

void AppendRecord(Record const& record)
{
    auto const path = LogPath();
    if (path.empty())
        return;

    std::string line;
    line += ToStringView(record.outcome);
    line += FieldSeparator;
    line += Sanitize(record.prefetchGroup);
    line += FieldSeparator;
    line += std::to_string(record.valueBytes);
    line += FieldSeparator;
    line += std::to_string(record.elapsedMs);
    line += FieldSeparator;
    line += Sanitize(record.source);
    line += FieldSeparator;
    line += Sanitize(record.detail);
    line += FieldSeparator;
    line += std::to_string(record.preprocessMs);
    line += FieldSeparator;
    line += std::to_string(record.cacheMs);
    line += FieldSeparator;
    line += std::to_string(record.directMs);
    line += FieldSeparator;
    line += record.directHit ? "1" : "0";
    line += FieldSeparator;
    line += std::to_string(record.timestampUnixSeconds);
    line += FieldSeparator;
    line += ToStringView(record.dispatch);
    line += FieldSeparator;
    line += Sanitize(record.dispatchDetail);
    line += '\n';

#if defined(_WIN32)
    // FILE_APPEND_DATA without FILE_WRITE_DATA makes each write atomically land
    // at end-of-file, so the ~500 concurrent compilers of one build interleave
    // whole lines instead of shredding each other's. A plain seek-then-write
    // (std::ofstream::app) has no such guarantee across processes.
    HANDLE handle = ::CreateFileA(path.c_str(),
                                  FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    ::WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ::CloseHandle(handle);
#else
    // O_APPEND gives the same atomicity for writes under PIPE_BUF on POSIX.
    // Opened through the raw syscall rather than std::ofstream: only O_APPEND
    // on a single write() guarantees whole lines from the hundreds of compilers
    // in one build interleave instead of shredding each other.
    int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    auto const written = ::write(fd, line.data(), line.size());
    static_cast<void>(written); // recording failures never break a build
    ::close(fd);
#endif
}

std::vector<Record> ParseLog(std::string_view groupFilter)
{
    auto const path = LogPath();
    if (path.empty())
        return {};

    std::ifstream input { path, std::ios::binary };
    if (!input)
        return {};

    std::vector<Record> records;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        auto const fields = SplitFields(line);
        if (fields.size() < 4)
            continue;

        if (!groupFilter.empty() && fields[1] != groupFilter)
            continue;

        records.push_back(DecodeFields(fields));
    }
    return records;
}

namespace
{
    /// Fold parsed records into the overall tally, the per-group tallies, and
    /// the never-cached attribution — the three views every report renders.
    struct FoldedLog
    {
        Tally overall;
        std::map<std::string, Tally> byGroup;
        std::map<std::string, std::uint64_t> neverCached;
    };

    [[nodiscard]] FoldedLog FoldRecords(std::vector<Record> const& records)
    {
        FoldedLog folded;
        for (auto const& record: records)
        {
            for (Tally* tally: { &folded.overall, &folded.byGroup[record.prefetchGroup] })
            {
                switch (record.outcome)
                {
                    case Outcome::Hit:
                        ++tally->hits;
                        tally->hitMs.push_back(record.elapsedMs);
                        // Older log lines predate the phase columns; leave the phase
                        // histograms empty for them rather than skew them with a false
                        // zero (see Record::hasPhaseColumns).
                        if (record.hasPhaseColumns)
                        {
                            tally->hitPreprocessMs.push_back(record.preprocessMs);
                            tally->hitCacheMs.push_back(record.cacheMs);
                        }
                        if (record.directHit)
                        {
                            ++tally->directHits;
                            tally->directMs.push_back(record.directMs);
                        }
                        break;
                    case Outcome::Miss:
                        ++tally->misses;
                        tally->missMs.push_back(record.elapsedMs);
                        break;
                    case Outcome::Uncacheable:
                        ++tally->uncacheable;
                        break;
                    case Outcome::Unavailable:
                        ++tally->unavailable;
                        break;
                }
                if (!record.detail.empty())
                    ++tally->reasons[record.detail];

                // The dispatch axis is folded separately, and its reason goes in its
                // own map -- never beside a cache reason.
                //
                // Every record lands in exactly one STATE, including `Unknown`, so
                // the section can tell "no fleet" from "nothing recorded". Its
                // REASON is folded only when the state is one the section will
                // print, which keeps the two lists reconciled by construction rather
                // than by coincidence. Without that, a line written by a LATER build
                // -- two launcher versions appending to one log is what a rolling
                // upgrade looks like -- contributes a sentence under "why
                // distribution did not help" with no state line accounting for it,
                // which is the claim-about-a-fleet-from-a-word-this-build-cannot-
                // read that `ParseDispatchOutcome` refuses one layer down.
                ++tally->dispatch[static_cast<std::size_t>(record.dispatch)];
                if (!record.dispatchDetail.empty() && RowFor(record.dispatch).reach != FleetReach::NoFleet)
                    ++tally->dispatchReasons[record.dispatchDetail];
            }

            // Attribute the never-cached translation units so a permanently
            // uncacheable file is visible rather than hidden in a percentage.
            if ((record.outcome == Outcome::Uncacheable || record.outcome == Outcome::Unavailable) && !record.source.empty())
                ++folded.neverCached[record.source];
        }
        return folded;
    }
} // namespace

std::string FormatReport(std::string_view groupFilter, UsageColor color)
{
    auto const path = LogPath();
    if (path.empty())
        return "fastcache-cc: no state directory available; statistics are disabled.\n";

    std::ifstream const probe { path, std::ios::binary };
    if (!probe)
        return "fastcache-cc: no statistics recorded yet (" + path + ").\n";

    auto const records = ParseLog(groupFilter);
    auto const [overall, byGroup, neverCached] = FoldRecords(records);

    if (overall.Total() == 0)
    {
        if (groupFilter.empty())
            return "fastcache-cc: no statistics recorded yet (" + path + ").\n";
        return "fastcache-cc: no records for prefetch group '" + std::string { groupFilter } + "'.\n";
    }

    auto const& palette = StatsPaletteFor(color);
    std::ostringstream out;
    out << "fastcache-cc statistics (" << path << ")\n\n";
    if (groupFilter.empty())
        out << "all prefetch groups\n";
    else
        out << "prefetch group " << groupFilter << '\n';
    AppendTallyLines(out, overall, palette);

    if (groupFilter.empty() && byGroup.size() > 1)
    {
        out << "\nper prefetch group\n";
        for (auto const& [prefetchGroup, tally]: byGroup)
        {
            out << "\n  " << (prefetchGroup.empty() ? "(unset)" : prefetchGroup) << '\n';
            std::ostringstream nested;
            AppendTallyLines(nested, tally, palette);
            std::istringstream lines { nested.str() };
            std::string nestedLine;
            while (std::getline(lines, nestedLine))
                out << "  " << nestedLine << '\n';
        }
    }

    if (!neverCached.empty())
    {
        out << "\nnever cached (" << neverCached.size() << " translation units)\n";
        auto const ranked = RankedByCount(neverCached);
        std::size_t shown = 0;
        for (auto const& [source, count]: ranked)
        {
            if (shown++ >= 10)
            {
                out << "  ... and " << (ranked.size() - 10) << " more\n";
                break;
            }
            out << "  " << count << "x  " << source << '\n';
        }
    }

    return out.str();
}

bool ResetLog()
{
    auto const path = LogPath();
    if (path.empty())
        return false;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !std::filesystem::exists(path);
}

namespace
{
    /// Escape the five characters HTML gives meaning to. Every value folded
    /// into the dashboard — a prefetch group name, a fall-back reason, a translation
    /// unit path — comes from the invocations log, which a compile can steer
    /// (a path or a fallback detail string), so nothing is trusted verbatim.
    [[nodiscard]] std::string EscapeHtml(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (char const c: text)
        {
            switch (c)
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
                    out += c;
            }
        }
        return out;
    }

    /// One rendered SVG bar, in the `<rect>` attributes the template writes
    /// verbatim: `x`/`y`/`w`(idth)/`h`(eight), already formatted as compact
    /// decimal text so the renderer never round-trips through iostream twice.
    struct Bar
    {
        std::string x, y, w, h;
    };

    /// Render a double as fixed decimal text with a bounded fractional part,
    /// trimming a trailing ".0" so whole pixel coordinates stay short.
    [[nodiscard]] std::string FormatCoord(double value)
    {
        std::array<char, 32> buffer {};
        auto const written = std::snprintf(buffer.data(), buffer.size(), "%.1f", value);
        return written > 0 ? std::string { buffer.data(), static_cast<std::size_t>(written) } : "0";
    }

    /// Bucket one latency sample set into `binCount` bars spanning its own
    /// min..max, mirroring AppendHistogram's data-derived-range principle so
    /// the HTML chart and the terminal sparkline never disagree about shape.
    /// @param samples Raw millisecond samples; may be empty.
    /// @param binCount Number of bars to emit.
    /// @param chartWidth Total SVG width the bars are laid out across.
    /// @param chartHeight Total SVG height; each bar grows up from the bottom.
    /// @return The bars, plus the low/high/p50/p95/max labels for the caption.
    struct HistogramSvg
    {
        std::vector<Bar> bars;
        std::uint64_t low {}, high {}, p50 {}, p95 {};
    };

    [[nodiscard]] HistogramSvg BuildHistogramSvg(std::vector<std::uint64_t> const& samples,
                                                 std::size_t binCount,
                                                 double chartWidth,
                                                 double chartHeight)
    {
        HistogramSvg result;
        if (samples.empty())
            return result;

        std::vector<std::uint64_t> sorted { samples };
        std::ranges::sort(sorted);
        result.low = sorted.front();
        result.high = sorted.back();
        result.p50 = Percentile(sorted, 0.50);
        result.p95 = Percentile(sorted, 0.95);

        std::vector<std::uint64_t> counts(binCount, 0);
        auto const span = result.high - result.low;
        for (auto const sample: samples)
        {
            std::size_t index = 0;
            if (span > 0)
            {
                auto const offset = static_cast<double>(sample - result.low) / static_cast<double>(span);
                index = static_cast<std::size_t>(offset * static_cast<double>(binCount));
                if (index >= binCount)
                    index = binCount - 1;
            }
            ++counts[index];
        }

        auto const peak = *std::ranges::max_element(counts);
        if (peak == 0)
            return result;

        constexpr double Gap = 2.0;
        auto const barWidth = (chartWidth - (Gap * static_cast<double>(binCount - 1))) / static_cast<double>(binCount);
        constexpr double MinBarHeight = 3.0; // a present-but-empty bucket still shows a sliver
        for (std::size_t i = 0; i < binCount; ++i)
        {
            // Parenthesized to defeat windows.h's function-style max() macro:
            // this TU is not built with NOMINMAX (it deliberately avoids
            // linking the FastCache library, which is where that define lives).
            auto const scaledHeight = chartHeight * (static_cast<double>(counts[i]) / static_cast<double>(peak));
            auto const barHeight = counts[i] == 0 ? 0.0 : (std::max) (MinBarHeight, scaledHeight);
            result.bars.push_back({
                .x = FormatCoord(static_cast<double>(i) * (barWidth + Gap)),
                .y = FormatCoord(chartHeight - barHeight),
                .w = FormatCoord(barWidth),
                .h = FormatCoord(barHeight),
            });
        }
        return result;
    }

    /// One day's worth of trend data: the hit rate among that day's cacheable
    /// compiles, and the raw compile volume.
    struct TrendDay
    {
        std::uint64_t hits {}, servable {}, volume {};
    };

    /// Bucket every record with a known timestamp (Record::timestampUnixSeconds
    /// != 0) into UTC calendar days, oldest first. Records with no timestamp —
    /// pre-upgrade log lines — are excluded rather than plotted at a false
    /// "day zero" (see Record::timestampUnixSeconds).
    /// @param records The records to bucket.
    /// @return Days in chronological order, each holding that day's tally.
    [[nodiscard]] std::vector<std::pair<std::int64_t, TrendDay>> BucketByDay(std::vector<Record> const& records)
    {
        constexpr std::int64_t SecondsPerDay = 86400;
        std::map<std::int64_t, TrendDay> byDay;
        for (auto const& record: records)
        {
            if (record.timestampUnixSeconds == 0)
                continue;
            auto const day = static_cast<std::int64_t>(record.timestampUnixSeconds) / SecondsPerDay;
            auto& bucket = byDay[day];
            ++bucket.volume;
            if (record.outcome == Outcome::Hit)
            {
                ++bucket.hits;
                ++bucket.servable;
            }
            else if (record.outcome == Outcome::Miss)
                ++bucket.servable;
        }
        return { byDay.begin(), byDay.end() };
    }

    /// Render the trend chart's `<polyline>`/`<rect>` markup, or an empty
    /// string when there is no timestamped data to plot (every record predates
    /// the timestamp column, or the log is otherwise empty of it).
    [[nodiscard]] std::string RenderTrendSvg(std::vector<Record> const& records)
    {
        auto const days = BucketByDay(records);
        if (days.empty())
            return "<p class=\"trend-empty\">No timestamped compiles yet.</p>";

        constexpr double Width = 960;
        constexpr double Height = 220;
        constexpr double LeftPad = 26;
        constexpr double RightPad = 10;
        constexpr double TopPad = 14;
        constexpr double BottomPad = 46;
        auto const plotWidth = Width - LeftPad - RightPad;
        auto const lineHeight = Height - TopPad - BottomPad;
        auto const n = days.size();

        auto const xAt = [&](std::size_t i) {
            return LeftPad + (n <= 1 ? 0.0 : (plotWidth * static_cast<double>(i)) / static_cast<double>(n - 1));
        };
        auto const yAt = [&](double rate) {
            return TopPad + (lineHeight * (1.0 - (rate / 100.0)));
        };

        std::ostringstream svg;
        svg << R"(<svg viewBox="0 0 )" << FormatCoord(Width) << ' ' << FormatCoord(Height)
            << R"(" class="trend-chart" preserveAspectRatio="none">)";

        auto const maxVolume =
            std::ranges::max_element(days, {}, [](auto const& entry) { return entry.second.volume; })->second.volume;
        constexpr double BarBand = 30.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const volume = days[i].second.volume;
            auto const barHeight =
                maxVolume == 0 ? 0.0 : BarBand * (static_cast<double>(volume) / static_cast<double>(maxVolume));
            constexpr double BarWidthFraction = 0.5;
            auto const barWidth = (n == 0 ? 0.0 : plotWidth / static_cast<double>(n)) * BarWidthFraction;
            svg << R"(<rect class="trend-volume" x=")" << FormatCoord(xAt(i) - (barWidth / 2.0)) << R"(" y=")"
                << FormatCoord(Height - 8.0 - barHeight) << R"(" width=")" << FormatCoord(barWidth) << R"(" height=")"
                << FormatCoord(barHeight) << R"("/>)";
        }

        svg << R"(<polyline class="trend-line" points=")";
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const [hits, servable, volume] = days[i].second;
            auto const rate = servable == 0 ? 0.0 : (100.0 * static_cast<double>(hits)) / static_cast<double>(servable);
            svg << FormatCoord(xAt(i)) << ',' << FormatCoord(yAt(rate)) << ' ';
        }
        svg << R"("/>)";
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const [hits, servable, volume] = days[i].second;
            auto const rate = servable == 0 ? 0.0 : (100.0 * static_cast<double>(hits)) / static_cast<double>(servable);
            svg << R"(<circle class="trend-point" cx=")" << FormatCoord(xAt(i)) << R"(" cy=")" << FormatCoord(yAt(rate))
                << R"(" r="3"/>)";
        }
        svg << "</svg>";
        return svg.str();
    }

    /// Render one outcome tally card.
    void AppendTallyCard(std::ostringstream& out, std::string_view label, std::uint64_t value, std::string_view cssClass)
    {
        out << R"(<div class="card )" << cssClass << R"("><span class="card-label">)" << EscapeHtml(label)
            << R"(</span><span class="card-value">)" << value << "</span></div>";
    }

    /// Render one reason tally as the dashboard's ranked bar list.
    ///
    /// Both reason axes render identically; written out per axis, the dispatch one
    /// would have been the third copy of it in this file.
    /// @param out Document stream.
    /// @param reasons Reason -> count.
    void AppendReasonBars(std::ostringstream& out, std::map<std::string, std::uint64_t> const& reasons)
    {
        out << R"(<div class="reasons">)";
        auto const ranked = RankedByCount(reasons);
        auto const worst = ranked.empty() ? 0 : ranked.front().second;
        for (auto const& [reason, count]: ranked)
        {
            auto const pct = worst == 0 ? 0.0 : (100.0 * static_cast<double>(count)) / static_cast<double>(worst);
            out << R"(<div><div class="reason-row"><span>)" << EscapeHtml(reason) << R"(</span><span class="mono">)" << count
                << R"(&times;</span></div><div class="reason-bar"><div class="reason-fill" style="width:)"
                << FormatCoord(pct) << R"(%"></div></div></div>)";
        }
        out << "</div>";
    }

    /// Render the dashboard's big rate-and-caption block.
    ///
    /// Hoisted because there are two of them -- the hit rate and the dispatch rate --
    /// and the CSS that pins the markup has no compiler link to either, so a class
    /// name changed in one copy breaks the other silently.
    /// @param out Document stream.
    /// @param label What the rate is of.
    /// @param rate The formatted percentage.
    /// @param trailer What it is a percentage of.
    void AppendHeadline(std::ostringstream& out, std::string_view label, std::string_view rate, std::string_view trailer)
    {
        out << R"(<div class="headline"><span class="label">)" << EscapeHtml(label) << R"(</span><span class="rate">)"
            << rate << R"(</span><span class="label">)" << EscapeHtml(trailer) << "</span></div>";
    }

    /// Render one label/count row of a panel list.
    ///
    /// Hoisted for the same reason as `AppendHeadline`: the markup and the CSS class
    /// that pins it had two copies and now have three consumers, with nothing in the
    /// compiler linking them.
    /// @param out Document stream.
    /// @param label Left cell; escaped, because a never-cached row's label is a path
    ///        a compile chose.
    /// @param count Right cell.
    /// @param countSuffix Written after the count as MARKUP rather than text -- every
    ///        caller passes a literal entity, and a tally reads "12&times;" where a
    ///        state count reads plainly.
    void AppendCountRow(std::ostringstream& out,
                        std::string_view label,
                        std::uint64_t count,
                        std::string_view countSuffix = {})
    {
        out << R"(<div class="never-row"><span>)" << EscapeHtml(label) << R"(</span><span>)" << count << countSuffix
            << "</span></div>";
    }

    /// Render the dashboard's distribution panel, or nothing at all when there is no
    /// fleet.
    ///
    /// Which rows appear and why is `VisibleDispatchLines`, shared with the terminal
    /// report -- so a state cannot appear in one and not the other, which is a fact
    /// rather than a promise.
    /// @param out Document stream.
    /// @param tally The folded records.
    void AppendDistributionPanel(std::ostringstream& out, Tally const& tally)
    {
        auto const lines = VisibleDispatchLines(tally);
        if (lines.empty())
            return;

        auto const asked = tally.DispatchTotal(FleetReach::Asked);
        auto const dispatched = tally.dispatch[static_cast<std::size_t>(DispatchOutcome::Dispatched)];
        out << R"(<div class="panel"><div class="panel-title">distribution</div>)";
        AppendHeadline(out, "dispatched", Percent(dispatched, asked), std::format("of {} asked of the fleet", asked));
        for (auto const& [row, count]: lines)
            AppendCountRow(out, row->label, count);
        if (!tally.dispatchReasons.empty())
            AppendReasonBars(out, tally.dispatchReasons);
        out << "</div>";
    }

    /// Render one latency histogram section: title, SVG bars, and the
    /// p50/p95/max caption.
    void AppendHistogramSection(std::ostringstream& out, std::string_view title, std::vector<std::uint64_t> const& samples)
    {
        if (samples.empty())
            return;
        constexpr double ChartWidth = 480;
        constexpr double ChartHeight = 40;
        auto const hist = BuildHistogramSvg(samples, 24, ChartWidth, ChartHeight);
        out << R"(<div class="hist"><div class="hist-title">)" << EscapeHtml(title) << R"(<span class="hist-meta">)"
            << samples.size() << " samples, " << FormatMs(hist.low) << "-" << FormatMs(hist.high)
            << R"(</span></div><svg viewBox="0 0 )" << FormatCoord(ChartWidth) << ' ' << FormatCoord(ChartHeight)
            << R"(" class="hist-chart" preserveAspectRatio="none">)";
        for (auto const& bar: hist.bars)
            out << R"(<rect x=")" << bar.x << R"(" y=")" << bar.y << R"(" width=")" << bar.w << R"(" height=")" << bar.h
                << R"("/>)";
        out << R"(</svg><div class="hist-caption">p50 )" << FormatMs(hist.p50) << " &middot; p95 " << FormatMs(hist.p95)
            << " &middot; max " << FormatMs(hist.high) << "</div></div>";
    }

    /// The dashboard's embedded stylesheet: terminal-native dark palette,
    /// matching the approved design (headline hit rate, tally cards, trend
    /// chart, histograms, prefetch group table, never-cached list). Kept as one
    /// literal here rather than templated piece by piece, since none of it
    /// varies per report.
    constexpr std::string_view DashboardStyle = R"CSS(
:root{
  --bg:#0b0f0e; --panel:#101613; --border:#1e2b26; --border-soft:#172420;
  --text:#e7f0ec; --text-dim:#9fb3ac; --text-faint:#62766f;
  --mono:'SFMono-Regular',Consolas,monospace; --sans:system-ui,sans-serif;
  --hit:#4ade80; --miss:#fb923c; --bad:#f87171; --uncache:#9ca89f; --accent:#67e8f9;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font-family:var(--sans)}
.wrap{min-height:100vh;padding:40px 48px 64px;display:flex;flex-direction:column;gap:28px;max-width:1280px;margin:0 auto}
.mono{font-family:var(--mono)}
.header{display:flex;justify-content:space-between;align-items:flex-start;gap:24px;flex-wrap:wrap}
.title{font-family:var(--mono);font-size:15px;font-weight:600}
.logpath{font-family:var(--mono);font-size:12px;color:var(--text-faint)}
.headline{display:flex;align-items:baseline;gap:10px;font-family:var(--mono)}
.headline .label{font-size:12px;color:var(--text-faint);text-transform:uppercase;letter-spacing:.08em}
.headline .rate{font-size:40px;font-weight:700;color:var(--hit)}
.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}
.card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:18px 18px 16px;display:flex;flex-direction:column;gap:10px}
.card-label{font-size:12px;letter-spacing:.06em;text-transform:uppercase;color:var(--text-dim)}
.card-value{font-family:var(--mono);font-size:28px;font-weight:600}
.card.hit .card-value{color:var(--hit)} .card.miss .card-value{color:var(--miss)}
.card.uncache .card-value{color:var(--uncache)} .card.bad .card-value{color:var(--bad)}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:22px 24px}
.panel-title{font-size:13px;font-weight:600;margin-bottom:16px}
.trend-chart{width:100%;height:220px;display:block}
.trend-volume{fill:var(--border-soft)}
.trend-line{fill:none;stroke:var(--hit);stroke-width:2.5;stroke-linejoin:round;stroke-linecap:round}
.trend-point{fill:var(--bg);stroke:var(--hit);stroke-width:2}
.trend-empty{color:var(--text-faint);font-size:13px}
.two-col{display:grid;grid-template-columns:1.4fr 1fr;gap:16px;align-items:start}
.hist{display:flex;flex-direction:column;gap:6px;margin-bottom:14px}
.hist-title{font-family:var(--mono);font-size:12px;color:var(--text-dim);display:flex;justify-content:space-between}
.hist-meta{font-family:var(--mono);font-size:11px;color:var(--text-faint)}
.hist-chart{width:100%;height:40px;display:block}
.hist-chart rect{fill:var(--accent)}
.hist-caption{font-family:var(--mono);font-size:11px;color:var(--text-faint)}
.reasons{display:flex;flex-direction:column;gap:10px}
.reason-row{display:flex;justify-content:space-between;font-size:12px}
.reason-bar{height:5px;background:var(--border-soft);border-radius:3px;overflow:hidden;margin-top:5px}
.reason-fill{height:100%;background:var(--miss);border-radius:3px}
table{border-collapse:collapse;width:100%;font-size:13px}
th{text-align:left;font-weight:500;color:var(--text-faint);font-size:11px;text-transform:uppercase;letter-spacing:.05em;padding:0 14px 10px 0;border-bottom:1px solid var(--border)}
td{padding:12px 14px 12px 0;border-bottom:1px solid var(--border-soft);font-family:var(--mono)}
.bar-cell{min-width:120px}
.bar-track{height:6px;background:var(--border-soft);border-radius:3px;overflow:hidden;width:100px}
.bar-fill{height:100%;background:var(--hit);border-radius:3px}
.never-row{display:flex;justify-content:space-between;gap:16px;padding:9px 0;border-bottom:1px solid var(--border-soft);font-family:var(--mono);font-size:12px}
.footer{text-align:center;font-family:var(--mono);font-size:11px;color:var(--text-faint);padding-top:8px}
)CSS";

} // namespace

std::string FormatHtmlReport(std::string_view groupFilter)
{
    auto const path = LogPath();
    if (path.empty())
        return "fastcache-cc: no state directory available; statistics are disabled.\n";

    std::ifstream const probe { path, std::ios::binary };
    if (!probe)
        return "fastcache-cc: no statistics recorded yet (" + path + ").\n";

    auto const records = ParseLog(groupFilter);
    auto const [overall, byGroup, neverCached] = FoldRecords(records);

    if (overall.Total() == 0)
    {
        if (groupFilter.empty())
            return "fastcache-cc: no statistics recorded yet (" + path + ").\n";
        return "fastcache-cc: no records for prefetch group '" + std::string { groupFilter } + "'.\n";
    }

    auto const servable = overall.hits + overall.misses;
    auto const hitRate = Percent(overall.hits, servable);

    std::ostringstream out;
    out << R"(<!doctype html><html><head><meta charset="utf-8"><title>fastcache-cc stats</title>)"
        << "<style>" << DashboardStyle << R"(</style></head><body><div class="wrap">)";

    out << R"(<div class="header"><div><div class="title">fastcache-cc / stats</div>)"
        << R"(<div class="logpath">)" << EscapeHtml(path) << "</div></div>";
    AppendHeadline(out, "hit rate", hitRate, std::format("of {} cacheable", servable));
    out << "</div>";

    out << R"(<div class="cards">)";
    AppendTallyCard(out, "hits", overall.hits, "hit");
    AppendTallyCard(out, "misses", overall.misses, "miss");
    AppendTallyCard(out, "uncacheable", overall.uncacheable, "uncache");
    AppendTallyCard(out, "unavailable", overall.unavailable, "bad");
    out << "</div>";

    out << R"(<div class="panel"><div class="panel-title">hit rate over time</div>)" << RenderTrendSvg(records) << "</div>";

    out << R"(<div class="two-col"><div class="panel"><div class="panel-title">latency distributions</div>)";
    AppendHistogramSection(out, "hit latency", overall.hitMs);
    AppendHistogramSection(out, "preprocess", overall.hitPreprocessMs);
    AppendHistogramSection(out, "cache i/o", overall.hitCacheMs);
    AppendHistogramSection(out, "miss latency", overall.missMs);
    out << "</div>";

    out << R"(<div class="panel"><div class="panel-title">fall-back reasons</div>)";
    AppendReasonBars(out, overall.reasons);
    out << "</div></div>";

    AppendDistributionPanel(out, overall);

    if (byGroup.size() > 1 || (byGroup.size() == 1 && !groupFilter.empty()))
    {
        out << R"(<div class="panel"><div class="panel-title">per-group comparison</div>)"
            << R"(<table><thead><tr><th>prefetch group</th><th>compiles</th><th>hit rate</th><th></th><th>unavailable</th>)"
               "</tr></thead><tbody>";
        for (auto const& [prefetchGroup, tally]: byGroup)
        {
            auto const groupServable = tally.hits + tally.misses;
            auto const groupRate = Percent(tally.hits, groupServable);
            auto const ratePct =
                groupServable == 0 ? 0.0 : (100.0 * static_cast<double>(tally.hits)) / static_cast<double>(groupServable);
            // Custom delimiter (html(...)html): the attribute value contains a
            // literal `)"` (CSS var(--hit) followed by the closing quote),
            // which would otherwise terminate a plain R"(...)" early.
            out << "<tr><td>" << EscapeHtml(prefetchGroup.empty() ? "(unset)" : prefetchGroup) << "</td><td>"
                << tally.Total() << R"html(</td><td style="color:var(--hit)">)html" << groupRate
                << R"(</td><td class="bar-cell">)"
                << R"(<div class="bar-track"><div class="bar-fill" style="width:)" << FormatCoord(ratePct)
                << R"(%"></div></div></td><td>)" << tally.unavailable << "</td></tr>";
        }
        out << "</tbody></table></div>";
    }

    if (!neverCached.empty())
    {
        out << R"(<div class="panel"><div class="panel-title">never cached ()" << neverCached.size()
            << " translation units)</div>";
        auto const ranked = RankedByCount(neverCached);
        std::size_t shown = 0;
        for (auto const& [source, count]: ranked)
        {
            if (shown++ >= 10)
            {
                out << R"(<div class="never-row"><span>&hellip; and )" << (ranked.size() - 10) << " more</span></div>";
                break;
            }
            AppendCountRow(out, source, count, "&times;");
        }
        out << "</div>";
    }

    out << R"(<div class="footer">generated by fastcache-cc --html-stats</div>)";
    out << "</div></body></html>";
    return out.str();
}

} // namespace FastCache::Cc

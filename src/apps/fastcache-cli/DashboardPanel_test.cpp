// SPDX-License-Identifier: Apache-2.0
#include "DashboardGlyphs.hpp"
#include "DashboardPanel.hpp"
#include "DashboardPanels.hpp"
#include "DashboardRig.hpp"
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "LivePipedView.hpp"
#include "ScriptedCellWidth.hpp"
#include "ScriptedSixelEncoder.hpp"
#include "StatsSource.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

/// An absent marker no figure could be mistaken for.
constexpr std::string_view Absent = "n/a";

/// One field of a scripted reading.
///
/// A constructor rather than an aggregate so a reading reads as a list of `{ name, value }` pairs,
/// which is what a scrape is.
struct Series
{
    /// @param seriesName The series name, labels included.
    /// @param seriesValue Its value.
    Series(std::string seriesName, std::uint64_t seriesValue):
        name { std::move(seriesName) },
        value { seriesValue }
    {
    }

    std::string name;    ///< The series name, labels included.
    std::uint64_t value; ///< Its value.
};

/// A `Sample` whose one stats source answered with @p series, taken @p seconds in.
/// @param series The reading's fields.
/// @param seconds When it was taken.
/// @param origin Which source answered.
/// @return The event.
[[nodiscard]] DashboardEvent SampleOf(std::vector<Series> const& series,
                                      int seconds,
                                      StatsOrigin origin = StatsOrigin::Metrics)
{
    auto fields = std::vector<Field> {};
    for (auto const& one: series)
        fields.push_back(Field { .name = one.name, .value = NumberCell(one.value) });
    return DashboardEvent { .kind = DashboardEventKind::Sample,
                            .at = TimePoint { std::chrono::seconds { seconds } },
                            .attempts = { StatsAttempt {
                                .origin = origin, .asked = true, .record = RecordValue(std::move(fields)), .note = {} } } };
}

/// The name of the catalogue's accepted-connections counter.
[[nodiscard]] std::string ConnectionsTotal()
{
    return std::string { DescriptorOf(IMetricsSink::Counter::ConnectionsTotal)->prometheusName };
}

/// A full `/metrics` reading for the cache panel, every counter scaled by @p step.
/// @param step How far every counter has moved.
/// @param tiers Which tiers the reading carries.
/// @return The fields.
[[nodiscard]] std::vector<Series> CacheSeries(std::uint64_t step, std::vector<std::string_view> const& tiers)
{
    auto series = std::vector<Series> {
        { "fastcached_get_hits_total", 90 * step },
        { "fastcached_get_misses_total", 10 * step },
        { "fastcached_cmd_get_total", 100 * step },
        { "fastcached_cmd_set_total", 5 * step },
        { ConnectionsTotal(), 3 * step },
        { "fastcached_evictions_total", 2 * step },
        { "fastcached_evicted_unfetched_total", step },
        { "fastcached_expired_unfetched_total", step },
        { std::string { DescriptorOf(IMetricsSink::Counter::ExpiryKeysReclaimed)->prometheusName }, 4 * step },
        { "fastcached_items", 1284991 },
        { "fastcached_bytes_used", std::uint64_t { 3 } << 30U },
        { "fastcached_bytes_limit", std::uint64_t { 4 } << 30U },
    };
    for (auto const tier: tiers)
    {
        series.emplace_back(TierSeriesName("fastcached_tier_items", tier), 412003);
        series.emplace_back(TierSeriesName("fastcached_tier_bytes_used", tier), std::uint64_t { 800 } << 20U);
        series.emplace_back(TierSeriesName("fastcached_tier_bytes_limit", tier), std::uint64_t { 1 } << 30U);
        series.emplace_back(TierSeriesName("fastcached_tier_evictions_total", tier), step);
        series.emplace_back(TierSeriesName("fastcached_tier_index_bytes", tier), std::uint64_t { 248 } << 20U);
    }
    return series;
}

/// A line's code points, one string each.
/// @param line Valid UTF-8.
/// @return The code points.
[[nodiscard]] std::vector<std::string> CodePoints(std::string_view line)
{
    auto points = std::vector<std::string> {};
    for (auto const byte: line)
    {
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U || points.empty())
            points.emplace_back();
        points.back().push_back(byte);
    }
    return points;
}

/// @p count columns of @p line starting at column @p from.
/// @param line Valid UTF-8.
/// @param from The first column.
/// @param count How many.
/// @return Those columns' bytes.
[[nodiscard]] std::string Columns(std::string_view line, std::size_t from, std::size_t count)
{
    auto const points = CodePoints(line);
    auto text = std::string {};
    for (auto const index: std::views::iota(from, std::min(points.size(), from + count)))
        text += points[index];
    return text;
}

/// @p text without leading and trailing spaces.
/// @param text The text.
/// @return The trimmed text.
[[nodiscard]] std::string Trimmed(std::string_view text)
{
    auto const first = text.find_first_not_of(' ');
    if (first == std::string_view::npos)
        return {};
    auto const last = text.find_last_not_of(' ');
    return std::string { text.substr(first, last - first + 1) };
}

/// A frame's lines.
/// @param frame The frame.
/// @return Its lines.
[[nodiscard]] std::vector<std::string> Lines(std::string_view frame)
{
    auto lines = std::vector<std::string> {};
    for (auto const line: std::views::split(frame, '\n'))
        lines.emplace_back(line.begin(), line.end());
    return lines;
}

// Where a rate row's columns are, in the frame. Stated once here and read by every case. A wrong
// constant makes `RowLine` find no row, and every case REQUIREs the rows it reads.
constexpr auto LabelFrom = std::size_t { 3 }; // the edge and the indent
constexpr auto LabelWidth = std::size_t { 16 };
constexpr auto FigureWidth = std::size_t { 9 };
constexpr auto SparkFrom = LabelFrom + LabelWidth + FigureWidth + 2;

/// The frame line whose rate-row label is @p label, or nullopt.
/// @param frame The frame.
/// @param label The label.
/// @return The line.
[[nodiscard]] std::optional<std::string> RowLine(std::string_view frame, std::string_view label)
{
    for (auto const& line: Lines(frame))
        if (Trimmed(Columns(line, LabelFrom, LabelWidth)) == label)
            return line;
    return std::nullopt;
}

/// The figure a rate row shows.
/// @param line The row's line.
/// @return The figure's text.
[[nodiscard]] std::string FigureOf(std::string_view line)
{
    return Trimmed(Columns(line, LabelFrom + LabelWidth, FigureWidth));
}

/// The sparkline a rate row draws, ending where the first beside text begins.
/// @param line The row's line.
/// @param besideStart The first word written beside the figure.
/// @return The sparkline's cells.
[[nodiscard]] std::vector<std::string> SparkOf(std::string_view line, std::string_view besideStart)
{
    auto const points = CodePoints(line);
    auto const joined = Columns(line, SparkFrom, points.size());
    auto const end = CodePoints(joined.substr(0, joined.find(besideStart))).size();
    auto cells = CodePoints(joined);
    cells.resize(end >= 3 ? end - 3 : 0); // the three spaces before the beside text
    return cells;
}

/// Draw @p script through a cache panel on @p rung.
/// @param script The events.
/// @param rung The rung.
/// @return The frames.
[[nodiscard]] std::vector<std::string> CacheFrames(std::vector<DashboardEvent> script, RenderRung rung)
{
    auto view = PanelView { CachePanel(),
                            PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = rung } };
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    return sink.frames;
}

auto const Tick = DashboardEvent { .kind = DashboardEventKind::Tick };

} // namespace

TEST_CASE("the first panel frame has every rate absent and the second has every rate present", "[cli][dashboard][panel]")
{
    // §9.1 on the drawn panel. WHAT DISTINGUISHES: both directions, every rate row. A panel showing
    // `0` on frame one passes a check that frame two is present, and is the defect.
    auto const frames =
        CacheFrames({ SampleOf(CacheSeries(1, { "memory" }), 1), Tick, SampleOf(CacheSeries(2, { "memory" }), 2), Tick },
                    RenderRung::Unicode);
    REQUIRE(frames.size() == 2);

    auto found = std::size_t { 0 };
    for (auto const& row: CachePanel().rates)
    {
        INFO("row " << row.label);
        auto const first = RowLine(frames[0], row.label);
        auto const second = RowLine(frames[1], row.label);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(FigureOf(Unwrap(first)) == Absent);
        CHECK(FigureOf(Unwrap(second)) != Absent);
        ++found;
    }
    CHECK(found == CachePanel().rates.size());
    // A suffix belongs to a number: an absent figure is the marker alone, never `n/a/s`.
    CHECK(!frames[0].contains(std::string { Absent } + "/s"));
}

TEST_CASE("a gap and a real zero draw different middle cells in a panel's trend", "[cli][dashboard][panel]")
{
    // §9.2, literally, at the rendering level: the conns/sec trend ending `[10, absent, 10]` against
    // one ending `[10, 0, 10]`. The absent interval is a stalled stamp, which the fold refuses to
    // measure; the zero is a counter that did not move. WHAT DISTINGUISHES: the MIDDLE of the last
    // three cells differs, and is a space on one side and the lowest block on the other.
    auto const connections = [](std::uint64_t value) {
        return std::vector<Series> { { ConnectionsTotal(), value } };
    };

    auto const gapped = CacheFrames({ SampleOf(connections(0), 1),
                                      SampleOf(connections(10), 2),
                                      SampleOf(connections(20), 2),
                                      SampleOf(connections(30), 3),
                                      Tick },
                                    RenderRung::Unicode);
    auto const zeroed = CacheFrames({ SampleOf(connections(0), 1),
                                      SampleOf(connections(10), 2),
                                      SampleOf(connections(10), 3),
                                      SampleOf(connections(20), 4),
                                      Tick },
                                    RenderRung::Unicode);
    REQUIRE(gapped.size() == 1);
    REQUIRE(zeroed.size() == 1);

    auto const gappedLine = RowLine(gapped[0], "conns/sec");
    auto const zeroedLine = RowLine(zeroed[0], "conns/sec");
    REQUIRE(gappedLine.has_value());
    REQUIRE(zeroedLine.has_value());
    auto const gappedSpark = SparkOf(Unwrap(gappedLine), "accepted");
    auto const zeroedSpark = SparkOf(Unwrap(zeroedLine), "accepted");
    REQUIRE(gappedSpark.size() >= 3);
    REQUIRE(zeroedSpark.size() == gappedSpark.size());

    // Four readings, so four cells from the trend's start; the middle of their last three is the third.
    auto const middle = std::size_t { 2 };
    CHECK(gappedSpark[middle] != zeroedSpark[middle]);
    CHECK(gappedSpark[middle] == " ");
    CHECK(zeroedSpark[middle] == "\xe2\x96\x81");
    CHECK(gappedSpark[middle - 1] == zeroedSpark[middle - 1]);
    CHECK(gappedSpark[middle + 1] == zeroedSpark[middle + 1]);
}

TEST_CASE("a young session's trend starts where its figure ends and fills toward the right", "[cli][dashboard][panel]")
{
    // N5 and C4: three readings in a trend wider than three cells. WHAT DISTINGUISHES: the trend's FIRST cell
    // is a reading and every cell after the newest is blank -- a right-aligned window draws the blank run
    // first and the readings at the far end, which is the picture that read as a narrow trend pushed right.
    auto const frames = CacheFrames(
        { SampleOf(CacheSeries(1, {}), 1), SampleOf(CacheSeries(2, {}), 2), SampleOf(CacheSeries(3, {}), 3), Tick },
        RenderRung::Unicode);
    REQUIRE(frames.size() == 1);
    auto const row = RowLine(frames[0], "ops/sec");
    REQUIRE(row.has_value());
    auto const spark = SparkOf(Unwrap(row), "get");
    REQUIRE(spark.size() > 3);
    // The first reading has no interval before it, so the rate starts at the second.
    CHECK(spark[0] == " ");
    CHECK(spark[1] != " ");
    CHECK(spark[2] != " ");
    CHECK(std::ranges::all_of(spark | std::views::drop(3), [](std::string const& cell) { return cell == " "; }));
}

TEST_CASE("a counter that went down draws a gap and the next interval draws a rate", "[cli][dashboard][panel]")
{
    // §9.3 on the panel: no negative rate, no clamp to zero, and the FOLLOWING interval is present.
    auto const connections = [](std::uint64_t value) {
        return std::vector<Series> { { ConnectionsTotal(), value } };
    };
    auto const frames = CacheFrames(
        { SampleOf(connections(100), 1), SampleOf(connections(50), 2), Tick, SampleOf(connections(60), 3), Tick },
        RenderRung::Unicode);
    REQUIRE(frames.size() == 2);

    auto const restarted = RowLine(frames[0], "conns/sec");
    auto const after = RowLine(frames[1], "conns/sec");
    REQUIRE(restarted.has_value());
    REQUIRE(after.has_value());
    // The newest cell is the reading count's, from the trend's start: two readings, then three.
    CHECK(FigureOf(Unwrap(restarted)) == Absent);
    CHECK(SparkOf(Unwrap(restarted), "accepted").at(1) == " ");
    CHECK(FigureOf(Unwrap(after)) == "10");
    CHECK(SparkOf(Unwrap(after), "accepted").at(2) != " ");
}

TEST_CASE("a tier table keeps three blank cells between every pair of columns, heading and rows alike",
          "[cli][dashboard][panel]")
{
    // §3: `tier      items        used       limit    evict/s   index (RAM)`. WHAT DISTINGUISHES: the heading
    // splits at runs of THREE blanks into exactly one cell per column. With a one-cell gap the 11-cell
    // `index (RAM)` joins `evict/s` in a 12-cell column and the count comes out one short -- while every word is
    // still present, which a contains() check passes. MEASURED by neutering the gap to one: only the heading
    // fails. The rows are held to the same count so a row never loses a column, but their figures are nine
    // cells or fewer (a ten-cell item count follows the tier name's padding), so they do not discriminate.
    auto const cellsOf = [](std::string_view line) {
        auto cells = std::vector<std::string> {};
        auto cell = std::string {};
        auto blanks = std::size_t { 0 };
        for (auto const ch: line)
        {
            if (ch == ' ')
            {
                ++blanks;
                continue;
            }
            if (blanks >= 3 && !cell.empty())
                cells.push_back(std::exchange(cell, {}));
            else if (!cell.empty())
                cell.append(blanks, ' ');
            blanks = 0;
            cell += ch;
        }
        if (!cell.empty())
            cells.push_back(std::move(cell));
        return cells;
    };

    auto const frames = CacheFrames({ SampleOf(CacheSeries(1, { "memory", "disk" }), 1), Tick }, RenderRung::Unicode);
    REQUIRE(frames.size() == 1);
    auto const lines = Lines(frames.front());
    auto const columns = CachePanel().tierColumns.size();
    auto checked = std::size_t { 0 };
    for (auto const& line: lines)
    {
        auto const name = Trimmed(Columns(line, LabelFrom, 8));
        if (name != "tier" && name != "memory" && name != "disk")
            continue;
        INFO("line: " << line);
        auto const cells = cellsOf(Columns(line, LabelFrom, FakeCellWidth(line) - LabelFrom - 2));
        CHECK(cells.size() == columns + 1);
        if (name == "tier")
            CHECK(cells.back() == "index (RAM)");
        ++checked;
    }
    CHECK(checked == 3);
}

TEST_CASE("an 80x24 cache panel carries every label, qualifier and note section 3 draws, readings at column 15",
          "[cli][dashboard][panel]")
{
    // §3 at an SSH session's floor. WHAT DISTINGUISHES: the LEVEL readings start at column 15, right after a
    // twelve-cell label (`items       1 284 991`), and the `connected` row keeps its reason at 80 -- which a
    // reading right-aligned into twelve more cells pushed off the line. The rest is the mockup's content held in
    // one place: each rate's qualifier, the tier heading, the footnote and the source line, so a regression to
    // key names or to dropping a qualifier goes red here.
    auto const at = [](std::uint64_t step, int seconds) {
        auto sample = SampleOf(CacheSeries(step, { "memory", "disk" }), seconds);
        sample.attempts.front().where = "127.0.0.1:9464";
        return sample;
    };
    auto view = PanelView { CachePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "127.0.0.1:6379",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive(
        { DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 24 }, at(1, 1), at(2, 3), Tick },
        DashboardLimits {},
        view,
        sink);
    REQUIRE(sink.frames.size() == 1);
    auto const lines = Lines(sink.frames.front());
    CHECK(lines.size() <= 24);

    // The line whose twelve label cells read @p label, or nullopt.
    auto const row = [&lines](std::string_view label) -> std::optional<std::string> {
        for (auto const& line: lines)
            if (Trimmed(Columns(line, LabelFrom, 12)) == label)
                return line;
        return std::nullopt;
    };
    constexpr auto ReadingFrom = LabelFrom + 12;
    auto const reading = [&row](std::string_view label) {
        auto const line = row(label);
        return line.has_value() ? Columns(*line, ReadingFrom, 80) : std::string { "(no row)" };
    };
    INFO(sink.frames.front());
    CHECK(reading("connected").starts_with(std::format("{}  no level is exported; connections_total is a TALLY", Absent)));
    CHECK(reading("items").starts_with("1 284 991"));
    CHECK(reading("bytes").starts_with("3.00 GiB / 4.00 GiB  "));
    CHECK(reading("bytes").contains("75.0 %"));

    auto const frame = sink.frames.front();
    for (auto const& [label, qualifier]: std::initializer_list<std::pair<std::string_view, std::string_view>> {
             { "hit rate", "since start" },
             { "ops/sec", "get " },
             { "ops/sec", "set " },
             { "conns/sec", "accepted " },
             { "evictions/s", "evicted unfetched " },
             { CachePanel().rates.back().label, "expired unfetched " },
         })
    {
        INFO("rate " << label << " with " << qualifier);
        auto const line = std::ranges::find_if(
            lines, [label](std::string const& one) { return Trimmed(Columns(one, LabelFrom, 16)).starts_with(label); });
        REQUIRE(line != lines.end());
        CHECK(line->contains(qualifier));
    }
    CHECK(frame.contains("index (RAM)"));
    CHECK(frame.contains("tier bytes carry per-tier denominations and do not sum; no per-tier"));
    CHECK(frame.contains("hit rate is published, deliberately."));
    CHECK(frame.contains("source  metrics (/metrics at 127.0.0.1:9464)"));
}

TEST_CASE("a cache frame dresses its level and tier readings as figures, their labels and notes as labels, and an "
          "absent reading not at all",
          "[cli][dashboard][panel][tone]")
{
    // G1. WHAT DISTINGUISHES: each run covers exactly the words it names -- `items`, not `items       ` -- with the
    // tone of what they are, so a figure and its label are two runs rather than one run over both; and a reading
    // the source did not carry (`fastcached_items` removed) gets no run, where dressing the marker as a figure would
    // give weight to a number that is not there.
    auto series = CacheSeries(1, { "memory", "disk" });
    std::erase_if(series, [](Series const& one) { return one.name == "fastcached_items"; });
    auto sink = CollectingSink {};
    auto view = PanelView {
        CachePanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    (void) Drive(
        { DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 24 }, SampleOf(series, 1), Tick },
        DashboardLimits {},
        view,
        sink);
    REQUIRE(sink.frames.size() == 1);
    INFO(sink.frames.front());
    auto const lines = Lines(sink.frames.front());
    auto runs = std::set<std::pair<FrameTone, std::string>> {};
    for (auto const& span: sink.spans.front())
    {
        auto const text = lines.at(span.row - 1).substr(span.byte, span.length);
        CHECK_FALSE(text.starts_with(' '));
        CHECK_FALSE(text.ends_with(' '));
        runs.emplace(span.tone, text);
    }

    for (auto const& expected: std::initializer_list<std::pair<FrameTone, std::string>> {
             { FrameTone::Label, "connected" },
             { FrameTone::Label, "no level is exported; connections_total is a TALLY" },
             { FrameTone::Label, "items" },
             { FrameTone::Label, "bytes" },
             { FrameTone::Figure, "3.00 GiB" },
             { FrameTone::Figure, "/ 4.00 GiB" },
             { FrameTone::Figure, "75.0 %" },
             { FrameTone::Label, "tier" },
             { FrameTone::Label, "index (RAM)" },
             { FrameTone::Label, "memory" },
             { FrameTone::Label, "disk" },
             { FrameTone::Figure, "412 003" },
             { FrameTone::Label, "tier bytes carry per-tier denominations and do not sum; no per-tier" },
         })
    {
        INFO("run " << std::to_underlying(expected.first) << " " << expected.second);
        CHECK(runs.contains(expected));
    }
    // The absent readings: `items` is not carried and the connected row has no field, so neither marker is a run.
    CHECK_FALSE(std::ranges::any_of(runs, [](auto const& run) { return run.second == Absent; }));
    CHECK(std::ranges::count_if(lines, [](std::string const& line) { return line.contains(Absent); }) >= 2);
}

TEST_CASE("a level label as wide as the label column still leaves a blank before its reading, and moves the block",
          "[cli][dashboard][panel]")
{
    // WHAT DISTINGUISHES: a left-aligned reading is written straight after its label's column, so a twelve-cell
    // label in a twelve-cell column reads `scratch free1 284 991` unless the column grows. It grows for the whole
    // block, so the short label's reading starts in the same column rather than at the default one.
    static constexpr auto levels = std::array {
        LevelRow { .label = "scratch free", .key = "long", .value = { .field = { .metrics = "fastcached_items" } } },
        LevelRow { .label = "items", .key = "items", .value = { .field = { .metrics = "fastcached_items" } } },
    };
    static constexpr auto spec =
        PanelSpec { .title = "levels", .rates = {}, .levels = levels, .tierColumns = {}, .tierNote = {} };
    auto view = PanelView {
        spec, PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 24 },
                   SampleOf(CacheSeries(1, {}), 1),
                   Tick },
                 DashboardLimits {},
                 view,
                 sink);
    REQUIRE(sink.frames.size() == 1);
    INFO(sink.frames.front());
    auto const lines = Lines(sink.frames.front());
    auto const starting = [&lines](std::string_view text) {
        return std::ranges::count_if(
            lines, [text](std::string const& line) { return Columns(line, LabelFrom, 80).starts_with(text); });
    };
    CHECK(starting("scratch free 1 284 991") == 1);
    CHECK(starting("items        1 284 991") == 1);
}

TEST_CASE("a tier the endpoint does not run contributes no row", "[cli][dashboard][panel]")
{
    // §9.5. WHAT DISTINGUISHES: the disk row is missing for a memory-only reading AND present for a
    // reading that carries it -- without the second, "draw no tier rows" passes.
    auto const tierRows = [](std::string_view frame) {
        auto names = std::set<std::string> {};
        for (auto const& line: Lines(frame))
            for (auto const& tier: StorageTierTable)
                if (Trimmed(Columns(line, LabelFrom, 8)) == tier.name)
                    names.insert(std::string { tier.name });
        return names;
    };

    auto const memoryOnly = CacheFrames({ SampleOf(CacheSeries(1, { "memory" }), 1), Tick }, RenderRung::Unicode);
    auto const both = CacheFrames({ SampleOf(CacheSeries(1, { "memory", "disk" }), 1), Tick }, RenderRung::Unicode);
    REQUIRE(memoryOnly.size() == 1);
    REQUIRE(both.size() == 1);

    CHECK(tierRows(memoryOnly[0]) == std::set<std::string> { "memory" });
    CHECK(tierRows(both[0]) == std::set<std::string> { "disk", "memory" });
}

TEST_CASE("an absent figure reads the same bytes on the Unicode and ASCII rungs", "[cli][dashboard][panel]")
{
    // §9.6. One reading, missing `fastcached_items` and the active cycle's counter, drawn on both
    // rungs. WHAT DISTINGUISHES: every row's label-and-figure columns are byte-identical across the
    // two rungs, and the rows compared DO carry the marker -- or identical lines of numbers pass.
    auto series = CacheSeries(1, { "memory" });
    std::erase_if(series, [](Series const& one) {
        return one.name == "fastcached_items"
               || one.name == DescriptorOf(IMetricsSink::Counter::ExpiryKeysReclaimed)->prometheusName;
    });
    auto const script = std::vector<DashboardEvent> { SampleOf(series, 1), Tick };
    auto const unicode = CacheFrames(script, RenderRung::Unicode);
    auto const ascii = CacheFrames(script, RenderRung::Ascii);
    REQUIRE(unicode.size() == 1);
    REQUIRE(ascii.size() == 1);

    auto const unicodeLines = Lines(unicode[0]);
    auto const asciiLines = Lines(ascii[0]);
    REQUIRE(unicodeLines.size() == asciiLines.size());

    auto markers = std::size_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 1 }, unicodeLines.size() - 1))
    {
        auto const unicodeCells = Columns(unicodeLines[index], LabelFrom, LabelWidth + FigureWidth);
        CHECK(unicodeCells == Columns(asciiLines[index], LabelFrom, LabelWidth + FigureWidth));
        if (unicodeCells.contains(Absent))
            ++markers;
    }
    CHECK(markers >= 2);
    auto const items = std::ranges::find_if(
        asciiLines, [](std::string const& line) { return Trimmed(Columns(line, LabelFrom, 12)) == "items"; });
    REQUIRE(items != asciiLines.end());
    CHECK(items->contains(Absent));
}

TEST_CASE("the ASCII rung keeps every figure and every absent marker the Unicode rung draws", "[cli][dashboard][panel]")
{
    // §9.10. The trend column may vanish; no figure may. WHAT DISTINGUISHES: after dropping the
    // tokens that are drawing rather than data -- edges, sparkline cells, gauges -- the two rungs'
    // words are the same list, in the same order. The controls: that list holds numbers and absent
    // markers, the Unicode frame drew a trend, and the ASCII frame drew none.
    auto series = CacheSeries(2, { "memory", "disk" });
    std::erase_if(series, [](Series const& one) { return one.name == "fastcached_items"; });
    auto const script =
        std::vector<DashboardEvent> { SampleOf(CacheSeries(1, { "memory", "disk" }), 1), SampleOf(series, 2), Tick };
    auto const unicode = CacheFrames(script, RenderRung::Unicode);
    auto const ascii = CacheFrames(script, RenderRung::Ascii);
    REQUIRE(unicode.size() == 1);
    REQUIRE(ascii.size() == 1);

    auto const drawingPieces = [](RungGlyphs const& glyphs) {
        auto pieces = std::set<std::string> { std::string { glyphs.horizontal }, std::string { glyphs.vertical },
                                              std::string { glyphs.topLeft },    std::string { glyphs.topRight },
                                              std::string { glyphs.bottomLeft }, std::string { glyphs.bottomRight },
                                              std::string { glyphs.gaugeOpen },  std::string { glyphs.gaugeFilled },
                                              std::string { glyphs.gaugeEmpty }, std::string { glyphs.gaugeClose } };
        for (auto const level: glyphs.sparkLevels)
            pieces.insert(std::string { level });
        pieces.erase(std::string {});
        return pieces;
    };
    auto const words = [&drawingPieces](std::string_view frame, RungGlyphs const& glyphs) {
        auto const pieces = drawingPieces(glyphs);
        auto kept = std::vector<std::string> {};
        for (auto const& line: Lines(frame))
            for (auto const token: std::views::split(std::string_view { line }, ' '))
            {
                auto const text = std::string { token.begin(), token.end() };
                auto const points = CodePoints(text);
                if (!points.empty()
                    && !std::ranges::all_of(points, [&pieces](std::string const& point) { return pieces.contains(point); }))
                    kept.push_back(text);
            }
        return kept;
    };

    auto const unicodeWords = words(unicode[0], GlyphsFor(RenderRung::Unicode));
    auto const asciiWords = words(ascii[0], GlyphsFor(RenderRung::Ascii));
    CHECK(unicodeWords == asciiWords);

    CHECK(std::ranges::count(asciiWords, std::string { Absent }) >= 1);
    CHECK(
        std::ranges::count_if(
            asciiWords, [](std::string const& word) { return !word.empty() && word.front() >= '0' && word.front() <= '9'; })
        >= 10);
    CHECK(unicode[0].contains("\xe2\x96")); // a block element: a trend was drawn
    CHECK(std::ranges::all_of(ascii[0], [](char byte) { return static_cast<unsigned char>(byte) < 0x80U; }));
}

TEST_CASE("every panel line is the terminal's width and a wider terminal draws more trend", "[cli][dashboard][panel]")
{
    // §3: wider terminals get more sparkline history, never a different layout. WHAT DISTINGUISHES:
    // every line fills the reported width exactly on both widths, AND the trend is longer on the
    // wider one -- a panel that ignored the width would pass the first half at 80 alone.
    auto const drawAt = [](int columns) {
        return CacheFrames({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = 60 },
                             SampleOf(CacheSeries(1, { "memory" }), 1),
                             SampleOf(CacheSeries(2, { "memory" }), 2),
                             Tick },
                           RenderRung::Unicode);
    };
    auto const narrow = drawAt(80);
    auto const wide = drawAt(120);
    REQUIRE(narrow.size() == 1);
    REQUIRE(wide.size() == 1);

    for (auto const& [frame, columns]:
         { std::pair { narrow[0], std::size_t { 80 } }, std::pair { wide[0], std::size_t { 120 } } })
        for (auto const& line: Lines(frame))
            CHECK(FakeCellWidth(line) == columns);

    auto const narrowLine = RowLine(narrow[0], "conns/sec");
    auto const wideLine = RowLine(wide[0], "conns/sec");
    REQUIRE(narrowLine.has_value());
    REQUIRE(wideLine.has_value());
    CHECK(SparkOf(Unwrap(wideLine), "accepted").size() > SparkOf(Unwrap(narrowLine), "accepted").size());
}

TEST_CASE("every series a panel names is one the daemon's formatter emits", "[cli][dashboard][panel]")
{
    // The storage, tier and host series are named in `PrometheusFormatter.cpp`, file-local, so the
    // panels spell them again. This is what connects the two spellings: a real snapshot rendered by
    // the real formatter, parsed by the client's own parser, must carry every `/metrics` name a
    // panel reads -- tier columns once per tier -- and every `NodeMetrics` name must be a catalogue
    // name. The control is a name that must NOT be found, so a lookup that finds everything fails.
    auto sink = AtomicMetricsSink {};
    auto tiers = TieredStorageStats {};
    for (auto& tier: tiers)
        tier = StorageStats {};
    auto const body = RenderPrometheus(
        sink,
        MetricsSnapshot {
            .storage = StorageStats {}, .storageTiers = tiers, .host = HostCapacity {}, .uptime = Uptime { 0s } });
    auto const record = ParsePrometheus(body);
    auto const emitted = [&record](std::string_view name) {
        return FindField(record, name) != nullptr;
    };
    REQUIRE(!emitted("fastcached_no_such_series"));

    auto catalogue = std::set<std::string_view> {};
    for (auto const& row: CounterTable)
        catalogue.insert(row.prometheusName);

    auto checked = std::size_t { 0 };
    auto const check = [&](FieldNames const& names, std::string_view tier) {
        if (!names.metrics.empty())
        {
            auto const name = tier.empty() ? std::string { names.metrics } : TierSeriesName(names.metrics, tier);
            INFO("/metrics series " << name);
            CHECK(emitted(name));
            ++checked;
        }
        if (!names.nodeMetrics.empty())
        {
            INFO("NodeMetrics name " << names.nodeMetrics);
            CHECK(catalogue.contains(names.nodeMetrics));
            ++checked;
        }
    };
    auto const checkFigure = [&](FigureSpec const& figure, std::string_view tier) {
        check(figure.field, tier);
        check(figure.other, tier);
    };

    for (auto const* panel: { &CachePanel(), &NodePanel() })
    {
        for (auto const& row: panel->rates)
        {
            checkFigure(row.figure, {});
            for (auto const& beside: row.beside)
                checkFigure(beside.figure, {});
        }
        for (auto const& row: panel->levels)
        {
            checkFigure(row.value, {});
            if (row.limit.has_value())
                checkFigure(Unwrap(row.limit), {});
        }
        for (auto const& column: panel->tierColumns)
            for (auto const& tier: StorageTierTable)
                checkFigure(column.figure, tier.name);
    }
    CHECK(checked >= 30);
}

TEST_CASE("a cache that served no reads has no hit rate rather than zero percent", "[cli][dashboard][panel]")
{
    // A ratio over nothing is not a ratio. WHAT DISTINGUISHES: two readings with no new hit and no
    // new miss show the marker, and the control -- the same counters moving -- shows a percentage.
    auto const reads = [](std::uint64_t hits, std::uint64_t misses) {
        return std::vector<Series> { { "fastcached_get_hits_total", hits }, { "fastcached_get_misses_total", misses } };
    };
    auto const idle = CacheFrames({ SampleOf(reads(50, 50), 1), SampleOf(reads(50, 50), 2), Tick }, RenderRung::Unicode);
    auto const busy = CacheFrames({ SampleOf(reads(50, 50), 1), SampleOf(reads(80, 60), 2), Tick }, RenderRung::Unicode);
    REQUIRE(idle.size() == 1);
    REQUIRE(busy.size() == 1);

    auto const idleLine = RowLine(idle[0], "hit rate");
    auto const busyLine = RowLine(busy[0], "hit rate");
    REQUIRE(idleLine.has_value());
    REQUIRE(busyLine.has_value());
    CHECK(FigureOf(Unwrap(idleLine)) == Absent);
    CHECK(FigureOf(Unwrap(busyLine)) == "75.0 %");
}

TEST_CASE("a newest sample that failed shows the marker rather than the last value read", "[cli][dashboard][panel]")
{
    // §9.17 draws a failure as a gap, and a level frozen at its last reading is not a gap: it claims
    // the cache still holds what it held before it stopped answering. WHAT DISTINGUISHES: the same
    // reading, followed by a failure or not, gives the marker or the figure.
    auto const levelOf = [](std::string_view frame, std::string_view label) {
        for (auto const& line: Lines(frame))
            if (Trimmed(Columns(line, LabelFrom, 12)) == label)
                return Trimmed(Columns(line, LabelFrom + 12, 12));
        return std::string {};
    };

    auto const failed = CacheFrames({ SampleOf(CacheSeries(1, { "memory" }), 1),
                                      DashboardEvent { .kind = DashboardEventKind::SampleFailed, .at = TimePoint { 2s } },
                                      Tick },
                                    RenderRung::Unicode);
    auto const answered = CacheFrames({ SampleOf(CacheSeries(1, { "memory" }), 1), Tick }, RenderRung::Unicode);
    REQUIRE(failed.size() == 1);
    REQUIRE(answered.size() == 1);

    CHECK(levelOf(failed[0], "items") == Absent);
    CHECK(levelOf(answered[0], "items") == "1 284 991");
}

TEST_CASE("the node panel's per-minute rate and mean compile come from the catalogue counters", "[cli][dashboard][panel]")
{
    // A figure's SCALE and its quotient are data in the node table, and nothing else draws them.
    // Ten jobs over two seconds is 300 per minute, not 5; twenty thousand milliseconds over those
    // ten jobs is a 2 s mean, not 2000. Read from a `NodeMetrics` reading, whose names are the
    // catalogue's own, so the case also proves the node table resolves that source.
    auto const node = [](std::uint64_t jobs, std::uint64_t millis) {
        return std::vector<Series> {
            { std::string { DescriptorOf(IMetricsSink::Counter::WorkerJobsCompleted)->prometheusName }, jobs },
            { std::string { DescriptorOf(IMetricsSink::Counter::WorkerCompileMillisTotal)->prometheusName }, millis },
        };
    };
    auto view = PanelView {
        NodePanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ SampleOf(node(10, 1000), 1, StatsOrigin::NodeMetrics),
                   SampleOf(node(20, 21000), 3, StatsOrigin::NodeMetrics),
                   Tick },
                 DashboardLimits {},
                 view,
                 sink);
    REQUIRE(sink.frames.size() == 1);

    auto const compiles = RowLine(sink.frames[0], "compiles/min");
    auto const mean = RowLine(sink.frames[0], "mean compile");
    REQUIRE(compiles.has_value());
    REQUIRE(mean.has_value());
    CHECK(FigureOf(Unwrap(compiles)) == "300");
    CHECK(FigureOf(Unwrap(mean)) == "2.00 s");
}

namespace
{

/// Draw @p script through @p spec on @p rung, after a `Resize` to @p columns by @p rows.
/// @param spec The panel.
/// @param script The events after the resize.
/// @param rung The rung.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frames.
[[nodiscard]] std::vector<std::string> FramesAt(
    PanelSpec const& spec, std::vector<DashboardEvent> script, RenderRung rung, int columns, int rows)
{
    script.insert(script.begin(), DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = rows });
    auto view =
        PanelView { spec, PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = rung } };
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    return sink.frames;
}

/// The one model every size is drawn from: two readings of a cache with both tiers.
/// @return The events.
[[nodiscard]] std::vector<DashboardEvent> TwoTierScript()
{
    return { SampleOf(CacheSeries(1, { "memory", "disk" }), 1), SampleOf(CacheSeries(2, { "memory", "disk" }), 2), Tick };
}

/// The one frame @p script draws through the cache panel at @p columns by @p rows.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frame.
[[nodiscard]] std::string CacheFrameAt(int columns, int rows)
{
    auto const frames = FramesAt(CachePanel(), TwoTierScript(), RenderRung::Unicode, columns, rows);
    REQUIRE(frames.size() == 1);
    return frames.front();
}

/// The widest line of @p frame, in cells.
/// @param frame The frame.
/// @return The width.
[[nodiscard]] std::size_t WidestLine(std::string_view frame)
{
    auto widest = std::size_t { 0 };
    for (auto const& line: Lines(frame))
        widest = std::max(widest, FakeCellWidth(line));
    return widest;
}

/// The frame line whose content starts with @p prefix after the edge and indent, or nullopt.
/// @param frame The frame.
/// @param prefix The start.
/// @return The line.
[[nodiscard]] std::optional<std::string> LineStarting(std::string_view frame, std::string_view prefix)
{
    for (auto const& line: Lines(frame))
        if (Columns(line, LabelFrom, FakeCellWidth(prefix)) == prefix)
            return line;
    return std::nullopt;
}

} // namespace

TEST_CASE("a panel fits every size it is given and keeps its essential figures", "[cli][dashboard][panel][responsive]")
{
    // #134 decision 5. WHAT DISTINGUISHES, at every size over ONE model: no line is wider than the
    // terminal in CELLS, no frame taller than it, it is a frame rather than the minimum-size line,
    // and every essential figure is present and whole. The control is the widest size, where every
    // row and every tier column is present -- without it, a panel that always drew only its
    // essentials would pass the rest.
    struct Size
    {
        int columns;
        int rows;
    };
    auto sizesChecked = std::size_t { 0 };
    for (auto const size: { Size { .columns = 132, .rows = 40 },
                            Size { .columns = 80, .rows = 24 },
                            Size { .columns = 40, .rows = 24 },
                            Size { .columns = 80, .rows = 14 },
                            Size { .columns = 60, .rows = 9 } })
    {
        INFO("size " << size.columns << "x" << size.rows);
        auto const frame = CacheFrameAt(size.columns, size.rows);
        CHECK(frame.starts_with("\xe2\x94\x8c")); // a frame's corner, not the minimum-size line
        CHECK(WidestLine(frame) <= static_cast<std::size_t>(size.columns));
        CHECK(Lines(frame).size() <= static_cast<std::size_t>(size.rows));

        auto const hitRate = RowLine(frame, "hit rate");
        auto const ops = RowLine(frame, "ops/sec");
        REQUIRE(hitRate.has_value());
        REQUIRE(ops.has_value());
        CHECK(FigureOf(Unwrap(hitRate)) == "90.0 %");
        CHECK(FigureOf(Unwrap(ops)) == "105");
        auto const bytes = LineStarting(frame, "bytes");
        REQUIRE(bytes.has_value());
        CHECK(Unwrap(bytes).contains("3.00 GiB"));
        ++sizesChecked;
    }
    CHECK(sizesChecked == 5);

    auto const wide = CacheFrameAt(132, 40);
    for (auto const& row: CachePanel().rates)
        CHECK(RowLine(wide, row.label).has_value());
    for (auto const& column: CachePanel().tierColumns)
        CHECK(wide.contains(column.header));
    CHECK(LineStarting(wide, "memory").has_value());
    CHECK(LineStarting(wide, "disk").has_value());
    CHECK(!wide.contains(" more"));
}

TEST_CASE("a Resize between two frames lays out the next frame for the new size", "[cli][dashboard][panel][responsive]")
{
    // The size is model state, so the frame after a resize must be laid out for it. WHAT
    // DISTINGUISHES: the two frames' widths are each terminal's, the second fits the smaller height,
    // and the second is byte for byte what a view that only ever saw the new size draws -- so nothing
    // measured at the old size survives into the new layout.
    //
    // 45 columns, not any narrow width: here no beside text fits on any row, so the trends take all the
    // room the figures leave, and the beside reserve measured at 132 columns would narrow them to their
    // minimum. At 50 columns `get 100` still fits and keeps the trends at their minimum either way, so
    // a view that never reset its reserve drew the identical frame there and passed.
    auto script = TwoTierScript();
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 45, .rows = 12 });
    script.push_back(Tick);
    auto const frames = FramesAt(CachePanel(), std::move(script), RenderRung::Unicode, 132, 40);
    REQUIRE(frames.size() == 2);
    CHECK(WidestLine(frames[0]) == 132);
    CHECK(WidestLine(frames[1]) == 45);
    CHECK(Lines(frames[1]).size() <= 12);
    CHECK(Lines(frames[0]).size() > Lines(frames[1]).size());
    CHECK(frames[1] == CacheFrameAt(45, 12));
}

TEST_CASE("what does not fit goes in priority order, not position order", "[cli][dashboard][panel][responsive]")
{
    // The drop order is the Priority column. WHAT DISTINGUISHES: in both cases below the piece that
    // goes FIRST is not the rightmost one -- `evict/s` (Low) is left of `index (RAM)` (Normal), and a
    // row's gauge (Low) is left of its percentage (High) -- so dropping by position fails each.
    auto const heading = [](std::string_view frame) {
        return LineStarting(frame, "tier");
    };

    auto firstTierDrop = std::optional<int> {};
    auto firstBytesDrop = std::optional<int> {};
    for (auto const columns: std::views::iota(40, 133) | std::views::reverse)
    {
        auto const frame = CacheFrameAt(columns, 60);
        auto const tier = heading(frame);
        REQUIRE(tier.has_value());
        auto const allColumns = std::ranges::all_of(
            CachePanel().tierColumns, [&](TierColumn const& column) { return Unwrap(tier).contains(column.header); });
        if (!allColumns && !firstTierDrop.has_value())
        {
            firstTierDrop = columns;
            INFO("first tier drop at " << columns << ": " << Unwrap(tier));
            CHECK(!Unwrap(tier).contains("evict/s"));
            CHECK(Unwrap(tier).contains("index (RAM)"));
        }

        auto const bytes = LineStarting(frame, "bytes");
        REQUIRE(bytes.has_value());
        auto const gauge = Unwrap(bytes).contains("\xe2\x96\x88") || Unwrap(bytes).contains("\xe2\x96\x91");
        auto const percent = Unwrap(bytes).contains("75.0 %");
        if ((!gauge || !percent) && !firstBytesDrop.has_value())
        {
            firstBytesDrop = columns;
            INFO("first bytes drop at " << columns << ": " << Unwrap(bytes));
            CHECK(!gauge);
            CHECK(percent);
        }
    }
    // Both drops happened inside the range scanned, or the checks above asserted nothing.
    CHECK(firstTierDrop.has_value());
    CHECK(firstBytesDrop.has_value());
}

TEST_CASE("a trend narrows to its minimum before a figure beside it is dropped", "[cli][dashboard][panel][responsive]")
{
    // Sparklines narrow first. WHAT DISTINGUISHES: at the widest width where the beside figure has
    // gone, one column wider draws it again -- with the trends at exactly their minimum. A layout
    // that dropped a beside figure while the trend was still wider than that fails the last check.
    auto dropAt = std::optional<int> {};
    for (auto const columns: std::views::iota(40, 133) | std::views::reverse)
    {
        auto const frame = CacheFrameAt(columns, 60);
        auto const evictions = RowLine(frame, "evictions/s");
        REQUIRE(evictions.has_value());
        if (!Unwrap(evictions).contains("evicted unfetched"))
        {
            dropAt = columns;
            break;
        }
    }
    REQUIRE(dropAt.has_value());

    auto const wider = CacheFrameAt(Unwrap(dropAt) + 1, 60);
    auto const evictions = RowLine(wider, "evictions/s");
    REQUIRE(evictions.has_value());
    REQUIRE(Unwrap(evictions).contains("evicted unfetched"));
    CHECK(SparkOf(Unwrap(evictions), "evicted").size() == 8);
}

TEST_CASE("a table that does not fit vertically ends in a count of what it hides", "[cli][dashboard][panel][responsive]")
{
    // WHAT DISTINGUISHES: at the tallest height that cannot show both tier rows, the heading stays and
    // a `+2 more` line stands for the rows -- they are not silently gone -- while the lines of lower
    // priority (the blank separators, the `connected` note row, the tier notes) went first. One line
    // taller, both rows show and nothing says `more`.
    auto shrunkAt = std::optional<int> {};
    for (auto const rows: std::views::iota(5, 41) | std::views::reverse)
        if (CacheFrameAt(132, rows).contains("+2 more"))
        {
            shrunkAt = rows;
            break;
        }
    REQUIRE(shrunkAt.has_value());

    auto const shrunk = CacheFrameAt(132, Unwrap(shrunkAt));
    CHECK(Lines(shrunk).size() <= static_cast<std::size_t>(Unwrap(shrunkAt)));
    CHECK(LineStarting(shrunk, "tier").has_value());
    CHECK(!LineStarting(shrunk, "memory").has_value());
    CHECK(!LineStarting(shrunk, "connected").has_value());
    CHECK(!shrunk.contains("per-tier denominations"));
    CHECK(RowLine(shrunk, "evictions/s").has_value()); // Normal rows outlast the Normal table's rows

    auto const taller = CacheFrameAt(132, Unwrap(shrunkAt) + 1);
    CHECK(!taller.contains(" more"));
    CHECK(LineStarting(taller, "memory").has_value());
    CHECK(LineStarting(taller, "disk").has_value());
}

TEST_CASE("below its minimum size a panel is one line naming the minimum and the terminal",
          "[cli][dashboard][panel][responsive]")
{
    // A clipped panel is worse than none: it shows some figures and silently not others. WHAT
    // DISTINGUISHES: one column or one row under the minimum gives ONE line saying both sizes and no
    // wider than the terminal, and AT the minimum a real frame is drawn -- or "always draw the line"
    // passes the first half.
    auto const minimum = MinimumPanelSize(CachePanel(), &FakeCellWidth);
    auto const columns = static_cast<int>(minimum.columns);
    auto const rows = static_cast<int>(minimum.rows);

    auto const narrow = CacheFrameAt(columns - 1, 24);
    CHECK(Lines(narrow).size() == 1);
    CHECK(narrow.contains(std::format("needs {}x{}, have {}x24", columns, rows, columns - 1)));
    CHECK(FakeCellWidth(narrow) <= minimum.columns - 1);

    auto const shortTerminal = CacheFrameAt(80, rows - 1);
    CHECK(Lines(shortTerminal).size() == 1);
    CHECK(shortTerminal.contains(std::format("needs {}x{}, have 80x{} -- fastcached", columns, rows, rows - 1)));

    auto const exact = CacheFrameAt(columns, rows);
    CHECK(exact.starts_with("\xe2\x94\x8c"));
    CHECK(Lines(exact).size() <= minimum.rows);
    CHECK(WidestLine(exact) <= minimum.columns);
    CHECK(RowLine(exact, "hit rate").has_value());
}

TEST_CASE("a wide endpoint in the title never pushes the frame past the terminal", "[cli][dashboard][panel][responsive]")
{
    // Hostnames can be wide, and a width counted in bytes or code points misplaces the corner. WHAT
    // DISTINGUISHES: the endpoint's cells differ from its bytes AND its code points, and every line
    // is still exactly the terminal's width in cells.
    auto const endpoint = std::string { "\xe7\xb7\xa8\xe8\xad\xaf\xe6\xa9\x9f-07:7070" }; // three CJK characters
    REQUIRE(FakeCellWidth(endpoint) != endpoint.size());
    REQUIRE(FakeCellWidth(endpoint) == 14);

    auto view = PanelView { CachePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = endpoint,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    auto script = TwoTierScript();
    script.insert(script.begin(), DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 60, .rows = 30 });
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    REQUIRE(sink.frames.size() == 1);
    CHECK(sink.frames[0].contains(endpoint));
    for (auto const& line: Lines(sink.frames[0]))
        CHECK(FakeCellWidth(line) == 60);
}

namespace
{

/// How many machines the fleet fixtures describe: more than a short terminal shows.
constexpr auto FleetMachines = std::size_t { 12 };

/// A leader's whole `/fleet.txt`, written in its grammar by hand.
///
/// By hand rather than rendered, for one reason: its machines section carries `zeta-column`, a column
/// no table in this tree has, so a panel that drew from a column list of its own would lose it. The
/// markers and the `kpi` header still come from the library's own doors, so only the invented column
/// is invented. The third machine's name is `-`, the absent cell.
/// @param machines How many machine rows.
/// @return The document.
[[nodiscard]] std::string FleetText(std::size_t machines)
{
    auto const marker = [](Distributed::FleetSection section) {
        return std::format("# {}\n", Distributed::FleetSectionTable[static_cast<std::size_t>(section)].key);
    };
    auto text = marker(FleetSection::Kpi);
    auto const kpiColumns = Distributed::FleetColumnNames(FleetSection::Kpi, Distributed::FleetSnapshot {});
    for (auto const index: std::views::iota(std::size_t { 0 }, kpiColumns.size()))
        text += (index == 0 ? "" : "\t") + kpiColumns[index];
    text += "\n";
    // The first three figures carry what the tile cases read: a grouped count, a denominator, and a
    // per-mille share. The rest are ones.
    constexpr auto LeadingFigures =
        std::to_array<std::string_view>({ "12884\tcount\t-", "47\tcount\t192", "881\tpermille\t-" });
    auto const keys = Distributed::FleetKpiKeys();
    for (auto const index: std::views::iota(std::size_t { 0 }, keys.size()))
    {
        text += std::format("{}\t{}\n", keys[index], index < LeadingFigures.size() ? LeadingFigures[index] : "1\tcount\t-");
    }
    text += "\n" + marker(FleetSection::Machines) + "endpoint\tname\tzeta-column\tcores\n";
    for (auto const machine: std::views::iota(std::size_t { 1 }, machines + 1))
        text += std::format("build-{:02}:7070\t{}\tz{}\t64\n",
                            machine,
                            machine == 3 ? std::string { "-" } : std::format("ci-{:02}", machine),
                            machine);
    return text;
}

/// A `fleet` sample carrying @p document, taken @p seconds in.
/// @param seconds When.
/// @param document What the fetch produced.
/// @return The event.
[[nodiscard]] DashboardEvent FleetSampleOf(int seconds, std::expected<std::string, AdminError> document)
{
    return DashboardEvent { .kind = DashboardEventKind::Sample,
                            .at = TimePoint { std::chrono::seconds { seconds } },
                            .document = std::move(document) };
}

/// Draw @p script through the fleet panel, read by the fleet reader, after a `Resize`.
/// @param script The events after the resize.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frames.
[[nodiscard]] std::vector<std::string> FleetFramesAt(std::vector<DashboardEvent> script, int columns, int rows)
{
    script.insert(script.begin(), DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = rows });
    auto view = PanelView {
        FleetPanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink, &ReadFleetSample);
    return sink.frames;
}

/// The one frame a fleet of @p machines draws at @p columns by @p rows.
/// @param machines How many machines.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frame.
[[nodiscard]] std::string FleetFrameAt(std::size_t machines, int columns, int rows)
{
    auto const frames = FleetFramesAt({ FleetSampleOf(1, FleetText(machines)), Tick }, columns, rows);
    REQUIRE(frames.size() == 1);
    return frames.front();
}

/// What follows @p word on the line of @p frame that holds it, spaces trimmed; nullopt without one.
/// @param frame The frame.
/// @param word The word.
/// @return The rest of that line.
[[nodiscard]] std::optional<std::string> AfterWord(std::string_view frame, std::string_view word)
{
    for (auto const& line: Lines(frame))
        if (auto const at = line.find(word); at != std::string::npos)
            return Trimmed(std::string_view { line }.substr(at + word.size()));
    return std::nullopt;
}

/// How many lines of @p frame start, after the edge and indent, with @p prefix.
/// @param frame The frame.
/// @param prefix The start.
/// @return The count.
[[nodiscard]] std::size_t LinesStarting(std::string_view frame, std::string_view prefix)
{
    return static_cast<std::size_t>(std::ranges::count_if(Lines(frame), [prefix](std::string const& line) {
        return Columns(line, LabelFrom, FakeCellWidth(prefix)) == prefix;
    }));
}

} // namespace

TEST_CASE("a fleet panel before its first reading draws every tile absent, the strip, and no table",
          "[cli][dashboard][panel][fleet]")
{
    // WHAT DISTINGUISHES: every headline key has its tile and each reads the absent marker -- never a
    // missing tile or a zero -- and where the table goes there is the marker alone, never an empty
    // table, which would say the fleet has no machines.
    auto const frames = FleetFramesAt({ Tick }, 132, 40);
    REQUIRE(frames.size() == 1);
    auto const& frame = frames.front();

    auto tiles = std::size_t { 0 };
    for (auto const key: Distributed::FleetKpiKeys())
    {
        INFO("tile " << key);
        auto const after = AfterWord(frame, key);
        REQUIRE(after.has_value());
        CHECK(Unwrap(after).starts_with(Absent));
        ++tiles;
    }
    CHECK(tiles == Distributed::FleetKpiKeys().size());

    auto tabs = std::size_t { 0 };
    for (auto const& row: Distributed::FleetSectionTable)
        if (row.tabular)
        {
            CHECK(frame.contains(row.section == FleetSection::Machines ? std::format("[{}]", row.key)
                                                                       : std::format(" {} ", row.key)));
            ++tabs;
        }
    CHECK(tabs > 1);
    CHECK(std::ranges::count_if(
              Lines(frame),
              [](std::string const& line) { return Trimmed(Columns(line, 1, FakeCellWidth(line) - 2)) == Absent; })
          == 1);
    CHECK(WidestLine(frame) <= 132);
}

TEST_CASE("a fleet panel draws the section from the header line it was sent, a column it never heard of included",
          "[cli][dashboard][panel][fleet]")
{
    // #1320: no column list lives in this client. WHAT DISTINGUISHES: `zeta-column` is in no table in
    // this tree and it is drawn, in the order the leader sent it -- a renderer walking a list of its own
    // draws the columns it knows and loses this one without a word. The tiles are the kpi rows written
    // in their own units.
    auto const frame = FleetFrameAt(FleetMachines, 132, 40);

    auto const heading = LineStarting(frame, "endpoint");
    REQUIRE(heading.has_value());
    auto const& line = Unwrap(heading);
    CHECK(line.find("endpoint") < line.find("name"));
    CHECK(line.find("name") < line.find("zeta-column"));
    CHECK(line.find("zeta-column") < line.find("cores"));

    auto const second = LineStarting(frame, "build-02:7070");
    REQUIRE(second.has_value());
    CHECK(Unwrap(second).contains("ci-02"));
    CHECK(Unwrap(second).contains("z2"));
    CHECK(Unwrap(second).contains("64"));
    auto const third = LineStarting(frame, "build-03:7070");
    REQUIRE(third.has_value());
    CHECK(Unwrap(third).contains(Absent)); // the `-` cell, as the absent marker
    CHECK(LinesStarting(frame, "build-") == FleetMachines);

    auto const keys = Distributed::FleetKpiKeys();
    REQUIRE(keys.size() >= 3);
    CHECK(AfterWord(frame, keys[0]).value_or("").starts_with("12 884"));
    CHECK(AfterWord(frame, keys[1]).value_or("").starts_with("47"));
    CHECK(AfterWord(frame, keys[1]).value_or("").contains("of 192"));
    CHECK(AfterWord(frame, keys[2]).value_or("").starts_with("88.1 %"));
    CHECK(frame.contains(FleetReadingSource));
}

TEST_CASE("a fleet table's columns go from the right as the terminal narrows, and its first column never does",
          "[cli][dashboard][panel][fleet]")
{
    // Positional, and stated as data. WHAT DISTINGUISHES: at the first width a column goes, the one
    // gone is the LAST column the leader sent; and at the narrowest width that is still a frame the
    // first column is there -- one column narrower is the minimum-size line, never a table without it.
    auto firstDrop = std::optional<int> {};
    auto narrowestFrame = std::optional<int> {};
    for (auto const columns: std::views::iota(12, 133) | std::views::reverse)
    {
        auto const frame = FleetFrameAt(FleetMachines, columns, 60);
        if (!frame.starts_with("\xe2\x94\x8c"))
            break;
        narrowestFrame = columns;
        auto const heading = LineStarting(frame, "endpoint");
        REQUIRE(heading.has_value());
        if (!Unwrap(heading).contains("cores") && !firstDrop.has_value())
        {
            firstDrop = columns;
            INFO("first column drop at " << columns << ": " << Unwrap(heading));
            CHECK(Unwrap(heading).contains("zeta-column"));
            CHECK(Unwrap(heading).contains("name"));
        }
    }
    REQUIRE(firstDrop.has_value());
    REQUIRE(narrowestFrame.has_value());
    auto const below = FleetFrameAt(FleetMachines, Unwrap(narrowestFrame) - 1, 60);
    CHECK(Lines(below).size() == 1);
    CHECK(below.contains("needs "));
    CHECK(FakeCellWidth(below) <= static_cast<std::size_t>(Unwrap(narrowestFrame) - 1));
}

TEST_CASE("a fleet table taller than the terminal keeps its heading and counts what it hides, after all else has gone",
          "[cli][dashboard][panel][fleet]")
{
    // The table is what this panel is for. WHAT DISTINGUISHES: at the tallest height that cannot show
    // every machine, the strip, the tiles and the source line have ALL gone already, the heading stays,
    // and `+N more` counts exactly the machines not shown. One line taller shows every machine.
    auto shrunkAt = std::optional<int> {};
    for (auto const rows: std::views::iota(4, 41) | std::views::reverse)
        if (FleetFrameAt(FleetMachines, 132, rows).contains(" more"))
        {
            shrunkAt = rows;
            break;
        }
    REQUIRE(shrunkAt.has_value());

    auto const frame = FleetFrameAt(FleetMachines, 132, Unwrap(shrunkAt));
    CHECK(Lines(frame).size() <= static_cast<std::size_t>(Unwrap(shrunkAt)));
    CHECK(LineStarting(frame, "endpoint").has_value());
    CHECK(!frame.contains(Distributed::FleetKpiKeys().front()));
    CHECK(!frame.contains("[machines]"));
    CHECK(!frame.contains(FleetReadingSource));
    auto const shown = LinesStarting(frame, "build-");
    auto const more = AfterWord(frame, "+");
    REQUIRE(more.has_value());
    CHECK(Unwrap(more).starts_with(std::format("{} more ", FleetMachines - shown)));

    auto const taller = FleetFrameAt(FleetMachines, 132, Unwrap(shrunkAt) + 1);
    CHECK(!taller.contains(" more"));
    CHECK(LinesStarting(taller, "build-") == FleetMachines);
}

TEST_CASE("a fleet panel drawn after a failed sample shows no table from before it", "[cli][dashboard][panel][fleet]")
{
    // A leader that stopped answering is a gap, not its last fleet. WHAT DISTINGUISHES: the frame after
    // the refusal has no machine rows and the marker where the table goes, while the frame before it has
    // every row -- a panel keeping the last document passes the first half and fails the second.
    auto const frames = FleetFramesAt(
        { FleetSampleOf(1, FleetText(FleetMachines)),
          Tick,
          FleetSampleOf(2, std::unexpected(AdminError { .kind = AdminFailure::Refused, .detail = "not the leader" })),
          Tick },
        132,
        40);
    REQUIRE(frames.size() == 2);
    CHECK(LinesStarting(frames[0], "build-") == FleetMachines);
    CHECK(LinesStarting(frames[1], "build-") == 0);
    CHECK(AfterWord(frames[1], Distributed::FleetKpiKeys().front()).value_or("").starts_with(Absent));
}

namespace
{

/// A leading snapshot with one machine whose memory and CPU figures the unit cases read, rendered by
/// the leader's own renderer: the document a real leader sends, not one written by hand.
/// @return The `/fleet.txt` body.
[[nodiscard]] std::string OneMachineFleetText()
{
    auto snapshot = Distributed::FleetSnapshot {};
    snapshot.role = Distributed::SchedulerRole::Leader;
    auto machine = Distributed::NodeReport {};
    machine.endpoint = "build-01:7070";
    machine.fingerprints = { "gcc-13-abcdef" };
    machine.capacity.logicalCores = 32;
    machine.capacity.totalMemoryBytes = 100552671232ULL;
    // `memory-available` and `scratch-free` are left unsaid, so they arrive absent.
    machine.load.cpuBusyPermille = 715;
    snapshot.nodes = { machine };
    return RenderFleetText(snapshot, Distributed::FleetHistoryView {}, std::nullopt);
}

/// The cell under @p heading in the row of @p frame whose first cell starts with @p rowStart, trimmed.
///
/// Every column after the first aligns on its last cell, so a cell ends where its heading does, and it
/// starts after the two-space gap before it.
/// @param frame The frame.
/// @param heading The column's heading.
/// @param rowStart What the row's first cell starts with.
/// @return The cell, or nullopt without that heading or row.
[[nodiscard]] std::optional<std::string> CellUnder(std::string_view frame,
                                                   std::string_view heading,
                                                   std::string_view rowStart)
{
    auto const lines = Lines(frame);
    auto const headingPoints = CodePoints(heading).size();
    for (auto const& header: lines)
    {
        auto const points = CodePoints(header);
        for (auto const start: std::views::iota(std::size_t { 1 }, points.size()))
        {
            auto const end = start + headingPoints;
            if (Columns(header, start, headingPoints) != heading || points[start - 1] != " "
                || (end < points.size() && points[end] != " "))
                continue;
            auto const row = std::ranges::find_if(lines, [rowStart](std::string const& line) {
                return Trimmed(Columns(line, LabelFrom, FakeCellWidth(line))).starts_with(rowStart);
            });
            if (row == lines.end())
                return std::nullopt;
            // Back from the heading's last column to the gap before the cell.
            auto const cell = Columns(*row, 0, end);
            auto const gap = cell.rfind("  ");
            return Trimmed(gap == std::string::npos ? std::string_view { cell } : std::string_view { cell }.substr(gap));
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a fleet table writes each cell in the scale its column has in the leader's own tables",
          "[cli][dashboard][panel][fleet][units]")
{
    // The owner read `100552671232` for a machine's memory. WHAT DISTINGUISHES: the bytes column reads
    // `93.65 GiB` and the per-mille one `71.5 %`, exactly as the leader's page writes them -- a panel that
    // copied the document's integers draws `100552671232` and `715`, which every width-only case still
    // accepts -- and a figure nobody reported stays the absent marker rather than becoming `0 B`.
    auto const frames = FleetFramesAt({ FleetSampleOf(1, OneMachineFleetText()), Tick }, 240, 40);
    REQUIRE(frames.size() == 1);
    auto const& frame = frames.front();

    CHECK(CellUnder(frame, "memory", "build-01") == std::optional<std::string> { "93.65 GiB" });
    CHECK(CellUnder(frame, "cpu-busy", "build-01") == std::optional<std::string> { "71.5 %" });
    CHECK(CellUnder(frame, "memory-available", "build-01") == std::optional<std::string> { std::string { Absent } });
    CHECK(CellUnder(frame, "cores", "build-01") == std::optional<std::string> { "32" });
    CHECK_FALSE(frame.contains("100552671232"));
    CHECK(WidestLine(frame) <= 240);
    // The column is as wide as what is DRAWN: `93.65 GiB` is nine cells, so the heading is padded by
    // three after the two-cell gap. Measured on the leader's `100552671232` it would be padded by six,
    // and the width drop would be deciding on columns nobody sees.
    CHECK(frame.contains("cores     memory"));
}

TEST_CASE("every column a leader renders has a scale the panel finds, and a column it does not render has none",
          "[cli][dashboard][panel][fleet][units]")
{
    // The lookup is by (section, column name) in the leader's tables, so a column renamed there must
    // not silently start rendering raw here. WHAT DISTINGUISHES: every name `FleetColumnNames` gives for
    // every tabular section -- tier columns included, with every tier present -- finds a scale, and a
    // name no table has finds none, which is what makes `zeta-column` render exactly as it was sent.
    auto snapshot = Distributed::FleetSnapshot {};
    snapshot.tiersPresent.fill(true);
    auto known = std::size_t { 0 };
    auto unknown = std::vector<std::string> {};
    for (auto const& row: Distributed::FleetSectionTable)
    {
        if (!row.tabular)
            continue;
        for (auto const& name: Distributed::FleetColumnNames(row.section, snapshot))
        {
            if (Distributed::FleetColumnFormat(row.section, name).has_value())
                ++known;
            else
                unknown.push_back(std::format("{}.{}", row.key, name));
        }
    }
    INFO("columns with no scale: " << unknown.size());
    CHECK(unknown.empty());
    CHECK(known > 20);
    CHECK_FALSE(Distributed::FleetColumnFormat(FleetSection::Machines, "zeta-column").has_value());
    CHECK(Distributed::FleetColumnFormat(FleetSection::Machines, "memory") == Distributed::CellFormat::Bytes);
    CHECK(Distributed::FleetColumnFormat(FleetSection::Machines, "cpu-busy") == Distributed::CellFormat::Permille);
}

TEST_CASE("a piped fleet record carries the leader's integers, never the panel's written figures",
          "[cli][dashboard][panel][fleet][units]")
{
    // Machine-readable output never humanises: a script reading the stream parses `881`, and `88.1 %`
    // would be a second grammar for it. WHAT DISTINGUISHES: the same document that draws `88.1 %` and
    // `12 884` in the panel streams `881` and `12884` through the piped view.
    auto view = PipedRecordView { OutputFormat::Tsv, std::nullopt, &FleetKpiFigures };
    auto sink = CollectingSink {};
    (void) Drive({ FleetSampleOf(1, FleetText(FleetMachines)), Tick }, DashboardLimits {}, view, sink, &ReadFleetSample);
    auto stream = std::string {};
    for (auto const& frame: sink.frames)
        stream += frame;
    CHECK(stream.contains("\t881\t"));
    CHECK(stream.contains("\t12884\t"));
    CHECK_FALSE(stream.contains("%"));
    CHECK_FALSE(stream.contains("12 884"));

    auto const panel = FleetFrameAt(FleetMachines, 132, 40);
    CHECK(panel.contains("88.1 %"));
    CHECK(panel.contains("12 884"));
}

TEST_CASE("every unit a leader's headline figures carry is one the fleet panel knows how to write",
          "[cli][dashboard][panel][fleet]")
{
    // `Distributed::CellFormatFromName` is the one place this client reads a unit's name, which is spelled
    // once, in the leader's `CellFormatTable`. WHAT DISTINGUISHES: the units come from the leader's own renderer, so a unit
    // renamed or added there fails here instead of leaving a tile written raw. The control is that the
    // section has rows at all -- an empty one would pass every check below.
    auto snapshot = Distributed::FleetSnapshot {};
    snapshot.role = Distributed::SchedulerRole::Leader;
    auto const parsed = ParseFleetDocument(RenderFleetText(snapshot, Distributed::FleetHistoryView {}, std::nullopt));
    REQUIRE(parsed.has_value());
    auto const* kpi = parsed->Section(FleetSection::Kpi);
    REQUIRE(kpi != nullptr);
    REQUIRE(!kpi->rows.empty());

    auto const names = Distributed::FleetColumnNames(FleetSection::Kpi, snapshot);
    REQUIRE(names.size() >= 3);
    auto const unitAt = std::ranges::find(kpi->columns, names[2]);
    REQUIRE(unitAt != kpi->columns.end());
    auto const unitIndex = static_cast<std::size_t>(unitAt - kpi->columns.begin());

    auto checked = std::size_t { 0 };
    for (auto const& row: kpi->rows)
    {
        auto const& unit = row[unitIndex].lexical;
        INFO("unit " << unit);
        CHECK(Distributed::CellFormatFromName(unit).has_value());
        ++checked;
    }
    CHECK(checked == kpi->rows.size());
}

namespace
{

/// How many times `CountedFleetParse` has run. Reset by the case that reads it.
std::size_t fleetParses = 0;

/// `ParseFleetDocument`, counted.
/// @param document The text.
/// @return The parse.
[[nodiscard]] std::expected<FleetDocument, std::string> CountedFleetParse(std::string_view document)
{
    ++fleetParses;
    return ParseFleetDocument(document);
}

/// The fleet reader, parsing through the counter.
/// @param event The sample.
/// @return The reading.
[[nodiscard]] SampleReading CountedFleetReader(DashboardEvent const& event)
{
    return ReadFleetSampleThrough(event, &CountedFleetParse);
}

/// The panel and the piped record together, drawn from one model: every drawer a fleet session has.
class EveryFleetDrawer final: public IDashboardView
{
  public:
    [[nodiscard]] DashboardFrame PlacedFrame(DashboardModel const& model) override
    {
        auto frame = panel.PlacedFrame(model);
        records += FleetKpiFigures(model).fields.size();
        return frame;
    }

    PanelView panel { FleetPanel(),
                      PanelContext {
                          .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode } };
    std::size_t records { 0 };
};

} // namespace

TEST_CASE("a fleet session parses each sample once, however many frames and records draw it",
          "[cli][dashboard][panel][fleet]")
{
    // WHAT DISTINGUISHES: two frames are owed per sample here, and each frame draws both the panel and
    // the piped record -- so a drawer that parsed the document itself would add parses per FRAME, and
    // the count would be a multiple of the samples rather than the samples.
    constexpr auto Samples = 4;
    auto script = std::vector<DashboardEvent> {};
    for (auto const second: std::views::iota(1, Samples + 1))
    {
        script.push_back(FleetSampleOf(second, FleetText(3)));
        script.push_back(Tick);
        script.push_back(Tick);
    }

    fleetParses = 0;
    auto view = EveryFleetDrawer {};
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink, &CountedFleetReader);
    REQUIRE(sink.frames.size() == static_cast<std::size_t>(2 * Samples));
    CHECK(LinesStarting(sink.frames.back(), "build-") == 3); // the frames drew the document
    CHECK(view.records > 0);
    CHECK(fleetParses == static_cast<std::size_t>(Samples));
}

TEST_CASE("the fleet table drawn is the section the context names, and the strip brackets that one",
          "[cli][dashboard][panel][fleet]")
{
    // `PanelContext::section` is how a session chooses the table. WHAT DISTINGUISHES: one document,
    // which carries machines and no workers, drawn with `section = Workers` has no machine rows, the
    // marker where the workers table goes, and `[workers]` bracketed rather than `[machines]` -- a view
    // that always drew the machines passes none of the three.
    auto view = PanelView { FleetPanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .cellWidth = &FakeCellWidth,
                                           .section = FleetSection::Workers,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 132, .rows = 40 },
                   FleetSampleOf(1, FleetText(FleetMachines)),
                   Tick },
                 DashboardLimits {},
                 view,
                 sink,
                 &ReadFleetSample);
    REQUIRE(sink.frames.size() == 1);
    auto const& frame = sink.frames.front();
    CHECK(LinesStarting(frame, "build-") == 0);
    CHECK(frame.contains("[workers]"));
    CHECK(!frame.contains("[machines]"));
    CHECK(frame.contains(" machines "));
    CHECK(std::ranges::count_if(
              Lines(frame),
              [](std::string const& line) { return Trimmed(Columns(line, 1, FakeCellWidth(line) - 2)) == Absent; })
          == 1);
}

namespace
{

/// The cell size every chart case reports: small enough that a chart's pixels stay few.
constexpr auto ChartCell = CellPixelSize { .width = 4, .height = 8 };

/// A document with the headline strip and a machines section carrying the chart's two columns.
/// @param busy Each machine's `cpu-busy`, in thousandths; `-` for a machine nobody read.
/// @return The text.
[[nodiscard]] std::string ChartText(std::vector<std::string> const& busy)
{
    auto const& metric = FleetChartMetrics.front();
    auto text = FleetText(0);
    // `FleetText` ends with its machines heading; replace that section with one the chart reads.
    text = text.substr(0,
                       text.find(std::format(
                           "# {}", Distributed::FleetSectionTable[static_cast<std::size_t>(FleetSection::Machines)].key)));
    text += std::format("# {}\n{}\t{}\n",
                        Distributed::FleetSectionTable[static_cast<std::size_t>(metric.section)].key,
                        metric.subjectColumn,
                        metric.valueColumn);
    for (auto const index: std::views::iota(std::size_t { 0 }, busy.size()))
        text += std::format("build-{:02}:7070\t{}\n", index + 1, busy[index]);
    return text;
}

/// What a fleet session drew: the frames, and every image the encoder was asked for.
struct ChartDrawing
{
    std::vector<std::string> frames;                     ///< Each frame's text.
    std::vector<std::vector<FramePlacement>> placements; ///< Each frame's images.
    std::vector<SixelRequest> requests;                  ///< What the encoder was asked to draw.
};

/// Draw three fleet samples and one frame through the fleet panel.
/// @param rung The rung.
/// @param cellPixels The cell size the resize carries, or nullopt.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return What was drawn.
[[nodiscard]] ChartDrawing DrawChart(RenderRung rung, std::optional<CellPixelSize> cellPixels, int columns, int rows)
{
    auto encoder = ScriptedSixelEncoder {};
    auto view = PanelView {
        FleetPanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .sixel = &encoder, .rung = rung }
    };
    auto sink = CollectingSink {};
    (void) Drive(
        { DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = rows, .cellPixels = cellPixels },
          FleetSampleOf(1, ChartText({ "100", "900", "500" })),
          FleetSampleOf(2, ChartText({ "200", "-", "600" })),
          FleetSampleOf(3, ChartText({ "300", "800", "700" })),
          Tick },
        DashboardLimits {},
        view,
        sink,
        &ReadFleetSample);
    return ChartDrawing { .frames = sink.frames, .placements = sink.placements, .requests = encoder.Requests() };
}

} // namespace

TEST_CASE("at the Sixel rung the fleet chart is one image exactly the size of the cells it covers",
          "[cli][dashboard][panel][fleet][chart]")
{
    // #134's Sixel acceptance. WHAT DISTINGUISHES: one placement, and the encoder was asked for an image
    // of cellsWide times the cell width by cellsHigh times the cell height -- a chart sized in any other
    // unit spills over or leaves a band blank -- over cells the frame's text left blank, with some pixel
    // drawn, since an all-transparent image would pass every size check.
    auto const drawing = DrawChart(RenderRung::Sixel, ChartCell, 100, 60);
    REQUIRE(drawing.frames.size() == 1);
    REQUIRE(drawing.placements.size() == 1);
    REQUIRE(drawing.placements[0].size() == 1);
    REQUIRE(drawing.requests.size() == 1);
    auto const& placement = drawing.placements[0][0];
    auto const& request = drawing.requests[0];
    CHECK(request.width == placement.cellsWide * ChartCell.width);
    CHECK(request.height == placement.cellsHigh * ChartCell.height);
    CHECK(placement.cellsHigh == Unwrap(FleetPanel().document).chartCellsHigh);
    CHECK(placement.sixel == std::format("sixel:{}x{}", request.width, request.height));

    auto const lines = Lines(drawing.frames[0]);
    REQUIRE(placement.row + placement.cellsHigh - 1 <= lines.size());
    for (auto const row: std::views::iota(placement.row, placement.row + placement.cellsHigh))
    {
        INFO("frame row " << row);
        CHECK(Trimmed(Columns(lines[row - 1], placement.column - 1, placement.cellsWide)).empty());
    }
    auto drawn = std::size_t { 0 };
    for (auto const pixel: std::views::iota(std::size_t { 0 }, request.pixels.size() / 4))
        drawn += request.pixels[(pixel * 4) + 3] != 0 ? 1 : 0;
    CHECK(drawn > 0);
}

TEST_CASE("below the Sixel rung, or without a cell size, the fleet panel draws no chart and keeps no rows for one",
          "[cli][dashboard][panel][fleet][chart]")
{
    // WHAT DISTINGUISHES: the Unicode frame asks the encoder for nothing and places nothing, and is SHORTER
    // than the Sixel one by the chart's rows -- blank rows left behind would be the chart faked -- and a
    // Sixel rung with no cell size draws exactly the Unicode frame, since no size is ever guessed.
    auto const sixel = DrawChart(RenderRung::Sixel, ChartCell, 100, 60);
    auto const unicode = DrawChart(RenderRung::Unicode, ChartCell, 100, 60);
    auto const unmeasured = DrawChart(RenderRung::Sixel, std::nullopt, 100, 60);
    REQUIRE(unicode.frames.size() == 1);
    REQUIRE(unmeasured.frames.size() == 1);
    CHECK(unicode.requests.empty());
    CHECK(unicode.placements.at(0).empty());
    CHECK(Lines(unicode.frames[0]).size() + Unwrap(FleetPanel().document).chartCellsHigh + 1
          == Lines(sixel.frames.at(0)).size());
    CHECK(unmeasured.requests.empty());
    CHECK(unmeasured.placements.at(0).empty());
    CHECK(unmeasured.frames[0] == unicode.frames[0]);
}

TEST_CASE("the fleet chart goes for height before the tiles, and below its width it is not drawn",
          "[cli][dashboard][panel][fleet][chart]")
{
    // The chart is an item in the drop order like any other. WHAT DISTINGUISHES: at the tallest height
    // without the chart, the headline tiles are still there -- the chart went first -- and at a width
    // under the chart's minimum there is no image while the table still draws.
    auto shortest = std::optional<int> {};
    for (auto const rows: std::views::iota(8, 61) | std::views::reverse)
        if (DrawChart(RenderRung::Sixel, ChartCell, 100, rows).placements.at(0).empty())
        {
            shortest = rows;
            break;
        }
    REQUIRE(shortest.has_value());
    auto const without = DrawChart(RenderRung::Sixel, ChartCell, 100, Unwrap(shortest));
    CHECK(without.frames.at(0).contains(Distributed::FleetKpiKeys().front()));
    CHECK(DrawChart(RenderRung::Sixel, ChartCell, 100, Unwrap(shortest) + 1).placements.at(0).size() == 1);

    auto const narrow = DrawChart(RenderRung::Sixel, ChartCell, 24, 60);
    CHECK(narrow.placements.at(0).empty());
    CHECK(narrow.requests.empty());
    CHECK(LineStarting(narrow.frames.at(0), "endpoint").has_value());
}

TEST_CASE("the fleet section keys walk the strip's tabs in its order, wrapping, and a digit names one",
          "[cli][dashboard][panel][fleet]")
{
    auto tabs = std::vector<FleetSection> {};
    for (auto const& row: Distributed::FleetSectionTable)
        if (row.tabular)
            tabs.push_back(row.section);
    REQUIRE(tabs.size() >= 3);
    // The position past the last tab must still be a digit for the last check to mean anything.
    REQUIRE(tabs.size() < 9);

    CHECK(SectionForKey(tabs[0], "\t") == tabs[1]);
    CHECK(SectionForKey(tabs[0], "\x1b[C") == tabs[1]);
    CHECK(SectionForKey(tabs[1], "\x1b[D") == tabs[0]);
    // Both ends wrap.
    CHECK(SectionForKey(tabs.back(), "\t") == tabs.front());
    CHECK(SectionForKey(tabs.front(), "\x1b[D") == tabs.back());
    // A digit is a position on the strip, not an enumerator.
    CHECK(SectionForKey(tabs[0], "1") == tabs[0]);
    CHECK(SectionForKey(tabs[0], "3") == tabs[2]);
    CHECK(SectionForKey(tabs[0], std::string(1, static_cast<char>('0' + tabs.size()))) == tabs.back());

    // From a section the strip does not name, the first step lands on an end of it.
    CHECK(SectionForKey(FleetSection::Kpi, "\t") == tabs.front());
    CHECK(SectionForKey(FleetSection::Kpi, "\x1b[D") == tabs.back());

    // A key naming nothing, and a tab past the last.
    CHECK_FALSE(SectionForKey(tabs[0], "x").has_value());
    CHECK_FALSE(SectionForKey(tabs[0], "0").has_value());
    CHECK_FALSE(SectionForKey(tabs[0], "\x1b[A").has_value());
    CHECK_FALSE(SectionForKey(tabs[0], "\t\t").has_value());
    CHECK_FALSE(SectionForKey(tabs[0], std::string(1, static_cast<char>('1' + tabs.size()))).has_value());
}

TEST_CASE("a key on the fleet panel switches the table it draws, at once", "[cli][dashboard][panel][fleet]")
{
    // WHAT DISTINGUISHES: which tab is bracketed in each frame, and how many frames there are. A key
    // naming no tab draws no frame, so the tick and four keys draw four.
    auto view = PanelView { FleetPanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .cellWidth = &FakeCellWidth,
                                           .section = FleetSection::Machines,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 132, .rows = 40 },
                   FleetSampleOf(1, FleetText(FleetMachines)),
                   Tick,
                   DashboardEvent { .kind = DashboardEventKind::Key, .keys = "\t" },
                   DashboardEvent { .kind = DashboardEventKind::Key, .keys = "9" },
                   DashboardEvent { .kind = DashboardEventKind::Key, .keys = "3" },
                   DashboardEvent { .kind = DashboardEventKind::Key, .keys = "\x1b[D" } },
                 DashboardLimits {},
                 view,
                 sink,
                 &ReadFleetSample);
    REQUIRE(sink.frames.size() == 4);
    CHECK(sink.frames[0].contains("[machines]"));
    CHECK(LinesStarting(sink.frames[0], "build-") == FleetMachines);
    CHECK(sink.frames[1].contains("[workers]"));
    CHECK(LinesStarting(sink.frames[1], "build-") == 0);
    CHECK(sink.frames[2].contains("[leases]"));
    CHECK(sink.frames[3].contains("[workers]"));
    CHECK(!sink.frames[3].contains("[machines]"));
}

TEST_CASE("a panel without a fleet document acts on no key", "[cli][dashboard][panel]")
{
    auto view = PanelView {
        CachePanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    CHECK_FALSE(view.Key("\t"));
    CHECK_FALSE(view.Key("1"));
}

namespace
{

/// The admin address the chrome fixtures' cache readings answered at.
constexpr std::string_view ChromeAdmin = "127.0.0.1:9464";

/// @p frame's top edge.
/// @param frame The frame.
/// @return Its first line.
[[nodiscard]] std::string TopEdge(std::string_view frame)
{
    return Lines(frame).front();
}

/// @p frame's source line, or nullopt without one.
/// @param frame The frame.
/// @return The line.
[[nodiscard]] std::optional<std::string> SourceRow(std::string_view frame)
{
    constexpr std::string_view Label = "source  ";
    for (auto const& line: Lines(frame))
        if (Columns(line, LabelFrom, FakeCellWidth(Label)) == Label)
            return line;
    return std::nullopt;
}

/// How a top edge starts on the Unicode rung: the corner, one edge glyph, then @p left padded.
/// @param left The left half.
/// @return The start.
[[nodiscard]] std::string TopStart(std::string_view left)
{
    auto const& glyphs = GlyphsFor(RenderRung::Unicode);
    return std::format("{}{} {} {}", glyphs.topLeft, glyphs.horizontal, left, glyphs.horizontal);
}

/// How a top edge ends on the Unicode rung: an edge glyph, @p right padded, an edge glyph, the corner.
/// @param right The right half.
/// @return The end.
[[nodiscard]] std::string TopEnd(std::string_view right)
{
    auto const& glyphs = GlyphsFor(RenderRung::Unicode);
    return std::format("{} {} {}{}", glyphs.horizontal, right, glyphs.horizontal, glyphs.topRight);
}

/// The fields a `/metrics` body stating build @p version parses into: the server's own info series, read by the
/// parser the session reads with -- so a fixture cannot state the version in a spelling `/metrics` never sends.
/// @param version The version the build info names.
/// @return The fields.
[[nodiscard]] std::vector<Field> BuildInfoFields(std::string_view version)
{
    return ParsePrometheus(RenderInfoMetric(InfoTable.front(), version)).fields;
}

/// A cache reading carrying what §3's title bar states: a version, and an uptime of `6d04:12`.
/// @return The event.
[[nodiscard]] DashboardEvent CacheChromeSample()
{
    constexpr auto Uptime = std::uint64_t { (6 * 24 * 60 * 60) + (4 * 60 * 60) + (12 * 60) };
    auto sample = SampleOf(CacheSeries(1, { "memory" }), 1);
    auto& attempt = sample.attempts.front();
    auto fields = Unwrap(attempt.record).fields;
    fields.push_back(Field { .name = std::string { CacheUptimeField }, .value = NumberCell(Uptime) });
    std::ranges::copy(BuildInfoFields("0.4.1"), std::back_inserter(fields));
    attempt.record = RecordValue(std::move(fields));
    attempt.where = std::string { ChromeAdmin };
    return sample;
}

/// The frames a cache panel asking `127.0.0.1:6379` every two seconds draws for @p script, @p columns wide.
/// @param script The events after the resize.
/// @param columns The terminal's width.
/// @return The frames.
[[nodiscard]] std::vector<std::string> CacheChromeFrames(std::vector<DashboardEvent> script, int columns)
{
    script.insert(script.begin(), DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = 40 });
    auto view = PanelView { CachePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "127.0.0.1:6379",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    return sink.frames;
}

} // namespace

TEST_CASE("a cache panel's title bar reads as section 3 draws it, its facts ending at the corner",
          "[cli][dashboard][panel][chrome]")
{
    // §3: `fastcached 0.4.1 ───── 127.0.0.1:6379  up 6d04:12  every 2s  q quit ─┐`. WHAT DISTINGUISHES: the
    // facts END at the corner, which the same words drawn left-aligned do not; the version is beside the
    // subject rather than among the facts; and the uptime is days, then hours and minutes.
    auto const frames = CacheChromeFrames({ CacheChromeSample(), Tick }, 80);
    REQUIRE(frames.size() == 1);
    auto const top = TopEdge(frames.front());
    CHECK(FakeCellWidth(top) == 80);
    CHECK(top.starts_with(TopStart("fastcached 0.4.1")));
    CHECK(top.ends_with(TopEnd("127.0.0.1:6379  up 6d04:12  every 2s  q quit")));
}

TEST_CASE("a cache panel titles itself with the version where its source states it, and in no other spelling",
          "[cli][dashboard][panel][chrome]")
{
    // WHAT DISTINGUISHES: each source's reading is asked in that source's spelling. A /metrics reading states the
    // version as a LABEL of its build info and INFO as a field's VALUE; asking either in the other's spelling
    // titles the panel `fastcached -`, which is what every cache read over /metrics drew before (#134 C1).
    auto const titleOf = [](std::vector<Field> fields, StatsOrigin origin) {
        auto sample = SampleOf({}, 1, origin);
        sample.attempts.front().record = RecordValue(std::move(fields));
        auto const frames = CacheChromeFrames({ sample, Tick }, 80);
        REQUIRE(frames.size() == 1);
        return TopEdge(frames.front());
    };
    auto const infoField = [] {
        return std::vector { Field { .name = "fastcached_version", .value = TextCell("0.4.1") } };
    };

    CHECK(titleOf(BuildInfoFields("0.4.1"), StatsOrigin::Metrics).starts_with(TopStart("fastcached 0.4.1")));
    CHECK(titleOf(infoField(), StatsOrigin::Info).starts_with(TopStart("fastcached 0.4.1")));

    // The other source's spelling in each reading is not a version.
    CHECK(titleOf(infoField(), StatsOrigin::Metrics).starts_with(TopStart(std::format("fastcached {}", Absent))));
    CHECK(titleOf(BuildInfoFields("0.4.1"), StatsOrigin::Info).starts_with(TopStart(std::format("fastcached {}", Absent))));
}

TEST_CASE("a cache panel's source line names the source, what was asked and where, and counts at the edge",
          "[cli][dashboard][panel][chrome]")
{
    // §3: `source  metrics (/metrics at 127.0.0.1:9464)     148 samples, 1 gap`. WHAT DISTINGUISHES: the
    // route and the address are the ANSWERING attempt's, and the counts end one blank column before the
    // right edge rather than trailing the source.
    auto const frames = CacheChromeFrames({ CacheChromeSample(), Tick }, 80);
    REQUIRE(frames.size() == 1);
    auto const row = SourceRow(frames.front());
    REQUIRE(row.has_value());
    CHECK(Columns(Unwrap(row), 1, 80).starts_with(std::format("  source  metrics (/metrics at {})   ", ChromeAdmin)));
    CHECK(Unwrap(row).ends_with(std::format("1 samples, 0 gaps {}", GlyphsFor(RenderRung::Unicode).vertical)));
}

TEST_CASE("before its first reading a title bar and source line name each absent fact by the marker",
          "[cli][dashboard][panel][chrome]")
{
    // A fact with no reading keeps its place and reads as the marker, never a missing word: `up n/a`, not a
    // title bar that silently lost its uptime.
    auto const frames = CacheChromeFrames({ Tick }, 80);
    REQUIRE(frames.size() == 1);
    auto const top = TopEdge(frames.front());
    CHECK(top.starts_with(TopStart(std::format("fastcached {}", Absent))));
    CHECK(top.ends_with(TopEnd(std::format("127.0.0.1:6379  up {}  every 2s  q quit", Absent))));
    auto const row = SourceRow(frames.front());
    REQUIRE(row.has_value());
    CHECK(Trimmed(Columns(Unwrap(row), 1, 40)) == std::format("source  {}", Absent));
}

TEST_CASE("a narrow title bar drops its facts by priority, and never the subject", "[cli][dashboard][panel][chrome]")
{
    // WHAT DISTINGUISHES: the version is drawn LEFT of the endpoint and still goes first, because the
    // endpoint's row outranks it -- a title cut from the right would keep the version and lose the address.
    auto const at = [](int columns) {
        auto const frames = CacheChromeFrames({ CacheChromeSample(), Tick }, columns);
        REQUIRE(frames.size() == 1);
        return TopEdge(frames.front());
    };
    auto const wide = at(56);
    CHECK(wide.ends_with(TopEnd("127.0.0.1:6379  up 6d04:12")));
    CHECK_FALSE(wide.contains("every"));
    CHECK_FALSE(wide.contains(" q quit"));

    auto const narrow = at(40);
    CHECK(FakeCellWidth(narrow) == 40);
    CHECK(narrow.starts_with(TopStart("fastcached")));
    CHECK(narrow.ends_with(TopEnd("127.0.0.1:6379")));
    CHECK_FALSE(narrow.contains("0.4.1"));
}

TEST_CASE("a node panel's title bar and source line read as section 4 draws them", "[cli][dashboard][panel][chrome]")
{
    // §4: `fastcache-compile-node 0.4.1 ───── build-07:7070  up 2d11:48  every 2s  q ─┐` and
    // `source  metrics (/metrics at build-07:9464)`. The version and the uptime come from the node's own
    // status, which its stats record does not carry.
    auto sample = SampleOf({ { "fastcache_node_logical_cores", 32 } }, 1);
    sample.attempts.front().where = "build-07:9464";
    auto status = CompileCacheWire::NodeStatusFields {};
    status.version = "0.4.1";
    status.uptimeSeconds = (2 * 24 * 60 * 60) + (11 * 60 * 60) + (48 * 60);
    sample.nodeStatus = std::move(status);

    auto view = PanelView { NodePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "build-07:7070",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive(
        { DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 40 }, std::move(sample), Tick },
        DashboardLimits {},
        view,
        sink);
    REQUIRE(sink.frames.size() == 1);
    auto const top = TopEdge(sink.frames.front());
    CHECK(top.starts_with(TopStart("fastcache-compile-node 0.4.1")));
    CHECK(top.ends_with(TopEnd("build-07:7070  up 2d11:48  every 2s  q")));
    auto const row = SourceRow(sink.frames.front());
    REQUIRE(row.has_value());
    CHECK(Columns(Unwrap(row), 1, 80).starts_with("  source  metrics (/metrics at build-07:9464)   "));
}

TEST_CASE("a fleet panel's title bar names the leader once it answered as one, and its source line says so",
          "[cli][dashboard][panel][chrome][fleet]")
{
    // §5: `fleet ───── leader build-01:7071  12 machines  every 5s  q ─┐` and
    // `source  /fleet.txt at build-01:9464 (leader)`. WHAT DISTINGUISHES: before a reading the address is
    // not called the leader's -- nothing has shown it to be one -- and the machine count is the marker.
    auto sample = FleetSampleOf(1, FleetText(FleetMachines));
    sample.documentWhere = "10.0.0.4:9464";
    auto view = PanelView { FleetPanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "10.0.0.4:6674",
                                           .interval = 5s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive(
        { DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 40 }, Tick, std::move(sample), Tick },
        DashboardLimits {},
        view,
        sink,
        &ReadFleetSample);
    REQUIRE(sink.frames.size() == 2);

    auto const before = TopEdge(sink.frames[0]);
    CHECK(before.ends_with(TopEnd(std::format("10.0.0.4:6674  {} machines  every 5s  q", Absent))));
    CHECK_FALSE(before.contains("leader"));

    auto const after = TopEdge(sink.frames[1]);
    CHECK(after.starts_with(TopStart("fleet")));
    CHECK(after.ends_with(TopEnd(std::format("leader 10.0.0.4:6674  {} machines  every 5s  q", FleetMachines))));
    auto const row = SourceRow(sink.frames[1]);
    REQUIRE(row.has_value());
    CHECK(Columns(Unwrap(row), 1, 80).starts_with("  source  /fleet.txt at 10.0.0.4:9464 (leader)   "));
}

namespace
{

using NodeCounter = IMetricsSink::Counter;

/// A catalogued counter's exported name.
/// @param counter The counter.
/// @return Its name.
[[nodiscard]] std::string CounterName(NodeCounter counter)
{
    return std::string { DescriptorOf(counter)->prometheusName };
}

/// A node reading @p step intervals in: every counter the node panel draws, moved by @p step.
/// @param step How far the counters have moved.
/// @param seconds When it was taken.
/// @param status What the node said about itself, or nothing.
/// @return The event.
[[nodiscard]] DashboardEvent NodeSampleOf(std::uint64_t step,
                                          int seconds,
                                          std::optional<CompileCacheWire::NodeStatusFields> status)
{
    auto sample = SampleOf({ { CounterName(NodeCounter::WorkerJobsCompleted), 41 * step },
                             { CounterName(NodeCounter::WorkerCompileMillisTotal), 75440 * step },
                             { CounterName(NodeCounter::WorkerJobsRefusedNoSlot), 2 * step },
                             { CounterName(NodeCounter::WorkerJobsRefusedLeaseExpired), step },
                             { CounterName(NodeCounter::WorkerJobsRefusedUnknownFingerprint), 0 },
                             { CounterName(NodeCounter::NodeCacheHits), 881 * step },
                             { CounterName(NodeCounter::NodeCacheMisses), 119 * step },
                             { "fastcache_node_disk_free_bytes", std::uint64_t { 41 } << 30U } },
                           seconds);
    sample.attempts.front().where = "build-07:9464";
    sample.nodeStatus = std::move(status);
    return sample;
}

/// §4's node: a follower running a cache tier, a worker and consensus, serving three toolchains.
/// @return The status.
[[nodiscard]] CompileCacheWire::NodeStatusFields MockupNodeStatus()
{
    namespace Bits = CompileCacheWire::NodeComponentBit;
    auto status = CompileCacheWire::NodeStatusFields {};
    status.version = "0.4.1";
    status.nodeId = "n-7f3c9a21";
    status.uptimeSeconds = 215280;
    status.components = Bits::CacheTier | Bits::Worker | Bits::Consensus;
    status.runtime.toolchains = CompileCacheWire::ToolchainState::Serving;
    status.runtime.toolchainsServed = 3;
    status.runtime.toolchainsDiscovered = 3;
    status.runtime.compileSlots = 16;
    status.runtime.compilesInFlight = 6;
    status.runtime.schedulerRole = CompileCacheWire::WireSchedulerRole::Follower;
    status.runtime.leaderEndpoint = "build-01:7071";
    status.runtime.registrarsRegistered = 1;
    status.runtime.registrarsTotal = 1;
    status.runtime.lastRegistrationSecondsAgo = 4;
    return status;
}

/// The frames a node panel asking `build-07:7070` every two seconds draws for @p script.
/// @param script The events after the resize.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frames.
[[nodiscard]] std::vector<std::string> NodeFramesAt(std::vector<DashboardEvent> script, int columns, int rows)
{
    script.insert(script.begin(), DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = rows });
    auto view = PanelView { NodePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "build-07:7070",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Unicode } };
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    return sink.frames;
}

/// The one frame §4's node draws at @p columns by @p rows, two readings in.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @param status What the node says about itself.
/// @return The frame.
[[nodiscard]] std::string NodeFrameAt(int columns, int rows, CompileCacheWire::NodeStatusFields const& status)
{
    auto const frames = NodeFramesAt({ NodeSampleOf(1, 1, status), NodeSampleOf(2, 3, status), Tick }, columns, rows);
    REQUIRE(frames.size() == 1);
    return frames.front();
}

} // namespace

namespace
{

/// The line under the one of @p frame starting with @p prefix.
/// @param frame The frame.
/// @param prefix The start of the line above.
/// @return The line, or nullopt without one.
[[nodiscard]] std::optional<std::string> LineUnder(std::string_view frame, std::string_view prefix)
{
    auto const lines = Lines(frame);
    for (auto const index: std::views::iota(std::size_t { 0 }, lines.size()))
        if (Columns(lines[index], LabelFrom, FakeCellWidth(prefix)) == prefix && index + 1 < lines.size())
            return lines[index + 1];
    return std::nullopt;
}

/// What the line of an 80-column @p frame starting with @p prefix says, between its edges and without its padding.
/// @param frame The frame.
/// @param prefix The start.
/// @return The text; empty when no line starts so, which no expected text is.
[[nodiscard]] std::string ContentStarting(std::string_view frame, std::string_view prefix)
{
    auto const line = LineStarting(frame, prefix);
    return line.has_value() ? Trimmed(Columns(Unwrap(line), 1, 78)) : std::string {};
}

/// The cell column @p text starts at on @p line, or nullopt.
/// @param line The line.
/// @param text The text.
/// @return Its column.
[[nodiscard]] std::optional<std::size_t> ColumnOf(std::string_view line, std::string_view text)
{
    auto const at = line.find(text);
    return at == std::string_view::npos ? std::nullopt : std::optional { FakeCellWidth(line.substr(0, at)) };
}

} // namespace

TEST_CASE("a node panel says who the node is and whether it is working, as section 4 draws it",
          "[cli][dashboard][panel][node]")
{
    // §4's upper half. WHAT DISTINGUISHES: each fact is the status's own -- identity, the survey's state
    // and counts, the registrations and when one was last accepted, the role and its leader -- and the
    // second column is ONE column: `registrars` and `leader` start in the same cell.
    auto const frame = NodeFrameAt(80, 24, MockupNodeStatus());

    auto const identity = LineStarting(frame, "node-id");
    REQUIRE(identity.has_value());
    CHECK(Unwrap(identity).contains("n-7f3c9a21"));
    CHECK(Unwrap(identity).contains("components  cache-tier, worker, consensus"));

    auto const working = LineStarting(frame, "toolchains");
    auto const consensus = LineStarting(frame, "consensus");
    REQUIRE(working.has_value());
    REQUIRE(consensus.has_value());
    CHECK(Trimmed(Columns(Unwrap(working), 1, 78)).starts_with("toolchains  serving 3 of 3"));
    CHECK(Unwrap(working).contains("registrars  1 of 1, last 4s ago"));
    CHECK(Trimmed(Columns(Unwrap(consensus), 1, 78)).starts_with("consensus   follower"));
    CHECK(Unwrap(consensus).contains("leader      build-01:7071"));
    CHECK(ColumnOf(Unwrap(working), "registrars") == ColumnOf(Unwrap(consensus), "leader"));
    // And every first label is ONE column, so `slots` and `host` start where `toolchains` does.
    auto const slots = LineStarting(frame, "slots");
    REQUIRE(slots.has_value());
    CHECK(ColumnOf(Unwrap(working), "serving") == ColumnOf(Unwrap(slots), "6 in flight"));
}

TEST_CASE("a node panel draws its slots as three numbers and what limits them, marking what no status carries",
          "[cli][dashboard][panel][node]")
{
    // N4. The three numbers are in flight, available and registered; `available` and `limited-by` are
    // `SlotCeilingsFor` over the machine's live load, which no status carries yet -- so both are the marker BY
    // NAME, under the value column, never a missing line.
    auto const frame = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const slots = LineStarting(frame, "slots");
    REQUIRE(slots.has_value());
    CHECK(Trimmed(Columns(Unwrap(slots), 1, 78))
          == std::format("slots       6 in flight / {} available / 16 registered", Absent));
    auto const under = LineUnder(frame, "slots");
    REQUIRE(under.has_value());
    CHECK(ColumnOf(Unwrap(under), "limited-by") == ColumnOf(Unwrap(slots), "6 in flight"));
    CHECK(Trimmed(Columns(Unwrap(under), 1, 78)) == std::format("limited-by  {}", Absent));
}

TEST_CASE("a node panel draws ONE refusal total with its trend, and the split under it", "[cli][dashboard][panel][node]")
{
    // N6. WHAT DISTINGUISHES: the total is the three counters' rates ADDED (2 + 1 + 0 per two seconds is
    // 90 a minute, and no one counter reads 90), the split is on the line under it, and no refusal is a
    // row of its own any more.
    auto const frame = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const total = RowLine(frame, "refused/min");
    REQUIRE(total.has_value());
    CHECK(FigureOf(Unwrap(total)) == "90");
    auto const split = LineUnder(frame, "refused/min");
    REQUIRE(split.has_value());
    CHECK(Trimmed(Columns(Unwrap(split), 1, 78)) == "no-slot 60/min   lease-expired 30/min   unknown-fingerprint 0.0/min");
    for (auto const* label: { "no-slot/min", "lease-exp/min", "unknown-fp/min" })
        CHECK_FALSE(RowLine(frame, label).has_value());
}

TEST_CASE("a mean compile note too long for its line wraps under where it began", "[cli][dashboard][panel][node]")
{
    // N7. At 80 the note is two lines, the second hanging at the column the first began at, and the words
    // are the note's in order; at 120 it is one line. A note dropped for width would pass neither.
    auto const note = std::string { "sum/count over this interval; no histogram exists, so no p50/p95 can be shown" };
    auto const narrow = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const row = RowLine(narrow, "mean compile");
    auto const under = LineUnder(narrow, "mean compile");
    REQUIRE(row.has_value());
    REQUIRE(under.has_value());
    CHECK_FALSE(Unwrap(row).contains(note));
    REQUIRE(ColumnOf(Unwrap(row), "sum/count").has_value());
    CHECK(Trimmed(Columns(Unwrap(under), 1, Unwrap(ColumnOf(Unwrap(row), "sum/count")) - 1)).empty());
    auto const column = Unwrap(ColumnOf(Unwrap(row), "sum/count"));
    auto const first = Trimmed(Columns(Unwrap(row), column, 79 - column));
    auto const second = Trimmed(Columns(Unwrap(under), 1, 78));
    CHECK(std::format("{} {}", first, second) == note);

    auto const wide = NodeFrameAt(120, 40, MockupNodeStatus());
    auto const one = RowLine(wide, "mean compile");
    REQUIRE(one.has_value());
    CHECK(Unwrap(one).contains(note));
}

TEST_CASE("a node panel draws its cache tier and host below the rates, and the tier only on a node running one",
          "[cli][dashboard][panel][node]")
{
    // N8 and N9. The tier line appears exactly when the status names the component -- not a line of markers
    // for a tier that does not exist -- and the host line is one line of the machine's figures, the two no
    // status carries yet marked by name.
    auto const frame = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const tier = LineStarting(frame, "cache tier");
    REQUIRE(tier.has_value());
    CHECK(Trimmed(Columns(Unwrap(tier), 1, 78)).starts_with("cache tier  hits 88.1 %"));
    auto const host = LineStarting(frame, "host");
    REQUIRE(host.has_value());
    CHECK(Trimmed(Columns(Unwrap(host), 1, 78))
          == std::format("host        cpu-busy {}   mem free {}   scratch free 41.00 GiB", Absent, Absent));
    for (auto const* gone: { "cores", "memory", "slots busy", "scratch free" })
        CHECK_FALSE(LineStarting(frame, gone).has_value());

    auto plain = MockupNodeStatus();
    plain.components = CompileCacheWire::NodeComponentBit::Worker;
    plain.runtime.schedulerRole.reset();
    plain.runtime.leaderEndpoint.clear();
    plain.nodeId.clear();
    auto const worker = NodeFrameAt(80, 24, plain);
    CHECK_FALSE(LineStarting(worker, "cache tier").has_value());
    CHECK_FALSE(LineStarting(worker, "consensus").has_value());
    CHECK(LineStarting(worker, "host").has_value());
    // No consensus, no minted identity: the marker, never an empty name somebody could paste.
    CHECK(ContentStarting(worker, "node-id").starts_with(std::format("node-id     {}", Absent)));
}

TEST_CASE("before a node says anything about itself its facts read the marker, and what may not apply draws nothing",
          "[cli][dashboard][panel][node]")
{
    // A reading with no status: identity and work are the marker by name, while consensus and a cache tier --
    // which may not exist on this node at all -- are not claimed.
    auto const frames = NodeFramesAt({ NodeSampleOf(1, 1, std::nullopt), Tick }, 80, 24);
    REQUIRE(frames.size() == 1);
    auto const& frame = frames.front();
    CHECK(ContentStarting(frame, "node-id").starts_with(std::format("node-id     {}", Absent)));
    CHECK(ContentStarting(frame, "toolchains").starts_with(std::format("toolchains  {}", Absent)));
    CHECK(ContentStarting(frame, "slots") == std::format("slots       {}", Absent));
    CHECK_FALSE(LineStarting(frame, "consensus").has_value());
    CHECK_FALSE(LineStarting(frame, "cache tier").has_value());

    // And a node no scheduler has ever accepted says NEVER, not an age.
    auto unaccepted = MockupNodeStatus();
    unaccepted.runtime.registrarsRegistered = 0;
    unaccepted.runtime.lastRegistrationSecondsAgo.reset();
    CHECK(ContentStarting(NodeFrameAt(80, 24, unaccepted), "toolchains").contains("registrars  0 of 1, never accepted"));
}

TEST_CASE("the active fleet section's tab is one Selected run over exactly its bracketed key",
          "[cli][dashboard][panel][fleet][tone]")
{
    // #134 F6, and the mechanism's first consumer. WHAT DISTINGUISHES: exactly one run, covering `[workers]`
    // byte for byte in the row it sits in -- not the gap before it, which inverse video would draw as a
    // block, and not `[machines]` -- after the Tab that switched to it; the text is unchanged, brackets
    // included, so a terminal with no colour reads the same grid.
    auto view = PanelView {
        FleetPanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 132, .rows = 40 },
                   FleetSampleOf(1, FleetText(FleetMachines)),
                   Tick,
                   DashboardEvent { .kind = DashboardEventKind::Key, .keys = "\t" } },
                 DashboardLimits {},
                 view,
                 sink,
                 &ReadFleetSample);
    REQUIRE(sink.frames.size() == 2);
    for (auto const& [index, tab]: { std::pair { std::size_t { 0 }, std::string_view { "[machines]" } },
                                     std::pair { std::size_t { 1 }, std::string_view { "[workers]" } } })
    {
        INFO("frame " << index);
        REQUIRE(sink.spans.at(index).size() == 1);
        auto const& span = sink.spans[index].front();
        CHECK(span.tone == FrameTone::Selected);
        CHECK(Lines(sink.frames[index]).at(span.row - 1).substr(span.byte, span.length) == tab);
        CHECK(sink.frames[index].contains(tab));
    }
}

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

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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

/// A `Sample` carrying @p reading, taken @p seconds in, from a stream dialled at @p where.
///
/// **The reading is the model a subscription carries**, built whole: a panel reads every figure from this struct, and
/// a block is present or absent in it exactly as a capture states it.
/// @param reading The reading.
/// @param seconds When it was taken.
/// @param where The endpoint the stream was read from; empty when the case does not say.
/// @return The event.
[[nodiscard]] DashboardEvent SampleOf(StatsReading reading, int seconds, std::string where = {})
{
    return DashboardEvent { .kind = DashboardEventKind::Sample,
                            .at = TimePoint { std::chrono::seconds { seconds } },
                            .reading = std::move(reading),
                            .where = std::move(where) };
}

/// A reading stating one catalogue counter and nothing else.
/// @param counter The counter.
/// @param value What it reads.
/// @return The reading.
[[nodiscard]] StatsReading CounterReading(IMetricsSink::Counter counter, std::uint64_t value)
{
    auto reading = StatsReading {};
    reading.counters[static_cast<std::size_t>(counter)] = value;
    return reading;
}

/// A full reading for the cache panel, every counter scaled by @p step.
///
/// A cache block is whole or absent, because the live model states nothing else: a reading with half a block is
/// one no capture produces.
/// @param step How far every counter has moved.
/// @param tiers Which tiers the reading carries.
/// @return The reading.
[[nodiscard]] StatsReading CacheReading(std::uint64_t step, std::vector<std::string_view> const& tiers)
{
    auto reading = StatsReading {};
    reading.counters[static_cast<std::size_t>(IMetricsSink::Counter::ConnectionsTotal)] = 3 * step;
    // The cycle's share of expiry moves slower than expiry itself, so a panel reading the one for the other differs.
    reading.counters[static_cast<std::size_t>(IMetricsSink::Counter::ExpiryKeysReclaimed)] = step;
    reading.snapshot.storage = StorageStats { .itemCount = 1284991,
                                              .bytesUsed = std::size_t { 3 } << 30U,
                                              .bytesLimit = std::size_t { 4 } << 30U,
                                              .evictions = 2 * step,
                                              .cmdGet = 100 * step,
                                              .cmdSet = 5 * step,
                                              .getHits = 90 * step,
                                              .getMisses = 10 * step,
                                              .evictedUnfetched = step,
                                              .expiredUnfetched = step,
                                              .expirations = 4 * step };
    for (auto const tier: tiers)
    {
        auto const* row = FindIfOrNull(StorageTierTable, [tier](auto const& one) { return one.name == tier; });
        REQUIRE(row != nullptr);
        reading.snapshot.storageTiers[static_cast<std::size_t>(row->tier)] =
            StorageStats { .itemCount = 412003,
                           .bytesUsed = std::size_t { 800 } << 20U,
                           .bytesLimit = std::size_t { 1 } << 30U,
                           .indexBytes = std::size_t { 248 } << 20U,
                           .evictions = step };
    }
    return reading;
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

// Where a rate row's columns are, in the frame, for a panel whose labels and figures fit the default columns: §3
// ends a figure at column 23 and starts its trend at 26. Stated once here and read by every case. A wrong constant
// makes `RowLine` find no row, and every case REQUIREs the rows it reads.
constexpr auto LabelFrom = std::size_t { 3 }; // the edge and the indent
constexpr auto LabelWidth = std::size_t { 12 };
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
        CacheFrames({ SampleOf(CacheReading(1, { "memory" }), 1), Tick, SampleOf(CacheReading(2, { "memory" }), 2), Tick },
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
        return CounterReading(IMetricsSink::Counter::ConnectionsTotal, value);
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
        { SampleOf(CacheReading(1, {}), 1), SampleOf(CacheReading(2, {}), 2), SampleOf(CacheReading(3, {}), 3), Tick },
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
        return CounterReading(IMetricsSink::Counter::ConnectionsTotal, value);
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

    auto const frames = CacheFrames({ SampleOf(CacheReading(1, { "memory", "disk" }), 1), Tick }, RenderRung::Unicode);
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
        return SampleOf(CacheReading(step, { "memory", "disk" }), seconds, "127.0.0.1:6379");
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

    // `expired/s` is every expiry (4 a step over 2 s), not the cycle's reclaims alone (1 a step).
    auto const expired = row("expired/s");
    REQUIRE(expired.has_value());
    CHECK(FigureOf(Unwrap(expired)) == "2.0");
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
            lines, [label](std::string const& one) { return Trimmed(Columns(one, LabelFrom, LabelWidth)) == label; });
        REQUIRE(line != lines.end());
        CHECK(line->contains(qualifier));
        // §3's columns: the figure's last cell is column 23 and the two before the trend are blank. A label column
        // of sixteen, which this block had, ends it at 27.
        CHECK(Columns(*line, 23, 1) != " ");
        CHECK(Columns(*line, 24, 2) == "  ");
    }
    CHECK(frame.contains("index (RAM)"));
    CHECK(frame.contains("tier bytes carry per-tier denominations and do not sum; no per-tier"));
    CHECK(frame.contains("hit rate is published, deliberately."));
    CHECK(frame.contains("source  subscription (SUBSCRIBE at 127.0.0.1:6379)"));
}

TEST_CASE("a cache frame dresses its level and tier readings as figures, their labels and notes as labels, and an "
          "absent reading not at all",
          "[cli][dashboard][panel][tone]")
{
    // G1. WHAT DISTINGUISHES: each run covers exactly the words it names -- `items`, not `items       ` -- with the
    // tone of what they are, so a figure and its label are two runs rather than one run over both; and a reading
    // the source did not carry gets no run, where dressing the marker as a figure would give weight to a number
    // that is not there. The absent readings here are the model's own: `connected` names no field, and one sample
    // is no interval, so the tier's `evict/s` is absent.
    auto const series = CacheReading(1, { "memory", "disk" });
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
             // G1 dims a unit: the number is the figure run and its unit a label run, and the `/` between a
             // reading and its limit is neither.
             { FrameTone::Figure, "3.00" },
             { FrameTone::Label, "GiB" },
             { FrameTone::Figure, "4.00" },
             { FrameTone::Figure, "75.0" },
             { FrameTone::Label, "%" },
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
    // The absent readings: the connected row has no field and one sample has no rate, so no marker is a run.
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
        LevelRow { .label = "scratch free", .key = "long", .value = { .field = StorageField<&StorageStats::itemCount>() } },
        LevelRow { .label = "items", .key = "items", .value = { .field = StorageField<&StorageStats::itemCount>() } },
    };
    static constexpr auto spec =
        PanelSpec { .title = "levels", .rates = {}, .levels = levels, .tierColumns = {}, .tierNote = {} };
    auto view = PanelView {
        spec, PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 24 },
                   SampleOf(CacheReading(1, {}), 1),
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

TEST_CASE("a rate label wider than the label column and a figure as wide as the figure column move the whole block",
          "[cli][dashboard][panel]")
{
    // WHAT DISTINGUISHES: a fifteen-cell label takes fifteen columns rather than being cut to twelve, and a
    // nine-cell figure in a nine-cell column still keeps a blank before it -- `rate label wide1 284 991` otherwise.
    // Both widen the block, so the short row's figure ends in the same column as the long one's.
    static constexpr auto rates = std::array {
        RateRow { .label = "rate label wide",
                  .key = "wide",
                  .figure = { .field = StorageField<&StorageStats::itemCount>(), .source = FigureSource::Level },
                  .trend = Trend::None },
        RateRow { .label = "ops",
                  .key = "ops",
                  .figure = { .field = StorageField<&StorageStats::itemCount>(), .source = FigureSource::Level },
                  .trend = Trend::None },
    };
    static constexpr auto spec =
        PanelSpec { .title = "rates", .rates = rates, .levels = {}, .tierColumns = {}, .tierNote = {} };
    auto view = PanelView {
        spec, PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 24 },
                   SampleOf(CacheReading(1, {}), 1),
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
    CHECK(starting("rate label wide 1 284 991") == 1);
    CHECK(starting("ops             1 284 991") == 1);
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

    auto const memoryOnly = CacheFrames({ SampleOf(CacheReading(1, { "memory" }), 1), Tick }, RenderRung::Unicode);
    auto const both = CacheFrames({ SampleOf(CacheReading(1, { "memory", "disk" }), 1), Tick }, RenderRung::Unicode);
    REQUIRE(memoryOnly.size() == 1);
    REQUIRE(both.size() == 1);

    CHECK(tierRows(memoryOnly[0]) == std::set<std::string> { "memory" });
    CHECK(tierRows(both[0]) == std::set<std::string> { "disk", "memory" });
}

TEST_CASE("an absent figure reads the same bytes on the Unicode and ASCII rungs", "[cli][dashboard][panel]")
{
    // §9.6. One reading with no cache block -- so `items` is absent -- and no active cycle's counter, drawn on
    // both rungs. WHAT DISTINGUISHES: every row's label-and-figure columns are byte-identical across the two
    // rungs, and the rows compared DO carry the marker -- or identical lines of numbers pass.
    auto series = CacheReading(1, { "memory" });
    series.snapshot.storage.reset();
    series.counters[static_cast<std::size_t>(IMetricsSink::Counter::ExpiryKeysReclaimed)].reset();
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
    // The second reading carries no cache block, so `items` and every figure over the block read absent.
    auto series = CacheReading(2, { "memory", "disk" });
    series.snapshot.storage.reset();
    auto const script =
        std::vector<DashboardEvent> { SampleOf(CacheReading(1, { "memory", "disk" }), 1), SampleOf(series, 2), Tick };
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
                             SampleOf(CacheReading(1, { "memory" }), 1),
                             SampleOf(CacheReading(2, { "memory" }), 2),
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

TEST_CASE("every figure a panel names reads the same number off the reading a subscription carries",
          "[cli][dashboard][panel]")
{
    // A panel names the MODEL, and a live session reads it off the wire in the binary form (#1399). WHAT
    // DISTINGUISHES: a reading whose every field is a different number, encoded by the daemon's encoder and
    // decoded by the client's decoder, reads back the SAME number for every figure a panel draws -- tier columns
    // once per tier -- so a figure the codec does not carry, or carries into its neighbour's field, reads absent
    // or the wrong number here. The control is a figure the original carries and a reading with no cache does not.
    auto sink = AtomicMetricsSink {};
    auto value = std::uint64_t { 1 };
    for (auto const& row: CounterTable)
        sink.Increment(row.counter, (value++ * 1'000'003) + 17);
    auto const distinct = [&value] {
        auto stats = StorageStats {};
        for (auto const member: StorageStatsSizeFields)
            stats.*member = static_cast<std::size_t>(value++ * 7919);
        for (auto const member: StorageStatsCounterFields)
            stats.*member = value++ * 7919;
        return stats;
    };
    auto tiers = TieredStorageStats {};
    for (auto& tier: tiers)
        tier = distinct();
    auto const original = CaptureStatsReading(
        sink,
        MetricsSnapshot { .storage = distinct(),
                          .storageTiers = tiers,
                          .host = HostCapacity { .logicalCores = 32,
                                                 .configuredSlots = 30,
                                                 .totalMemoryBytes = 68'719'476'736,
                                                 .diskCapacityBytes = 2'000'398'934'016,
                                                 .diskFreeBytes = 442'381'631'488,
                                                 .busySlots = 7 },
                          .hostLoad = HostLoadReading { .cpu = CpuTicks { .busy = 7'700'001, .total = 9'100'003 },
                                                        .availableMemoryBytes = 21'474'836'480 },
                          .uptime = Uptime { 864'017s } });
    auto const decoded = DecodeStatsReading(EncodeStatsReading(original));
    REQUIRE(decoded.has_value());
    auto const& adapted = decoded.value();
    auto const noCache = StatsReading {};

    auto checked = std::size_t { 0 };
    auto const check = [&](ReadingField field, std::string_view tierName) {
        if (!field.Names())
            return;
        auto const* row = FindIfOrNull(StorageTierTable, [tierName](auto const& tier) { return tier.name == tierName; });
        auto const tier = row == nullptr ? std::optional<StorageTier> {} : std::optional { row->tier };
        auto const expected = field.read(original, tier);
        INFO("figure " << checked << " tier " << tierName);
        REQUIRE(expected.has_value());
        CHECK(field.read(adapted, tier) == expected);
        ++checked;
    };
    auto const checkFigure = [&](FigureSpec const& figure, std::string_view tier) {
        check(figure.field, tier);
        check(figure.other, tier);
        for (auto const addend: figure.addends)
            check(addend, tier);
    };
    CHECK_FALSE(CachePanel().levels[1].value.field.read(noCache, std::nullopt).has_value());

    for (auto const* panel: { &CachePanel(), &NodePanel() })
    {
        for (auto const& row: panel->rates)
        {
            checkFigure(row.figure, {});
            for (auto const& beside: row.beside)
                checkFigure(beside.figure, {});
            for (auto const& part: row.split)
                checkFigure(part.figure, {});
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
        for (auto const& block: panel->facts)
            for (auto const& line: block.lines)
                for (auto const& cell: line.cells)
                    for (auto const& figure: cell.figures)
                        checkFigure(figure.figure, {});
    }
    CHECK(checked >= 30);
}

TEST_CASE("a cache that served no reads has no hit rate rather than zero percent", "[cli][dashboard][panel]")
{
    // A ratio over nothing is not a ratio. WHAT DISTINGUISHES: two readings with no new hit and no
    // new miss show the marker, and the control -- the same counters moving -- shows a percentage.
    auto const reads = [](std::uint64_t hits, std::uint64_t misses) {
        auto reading = StatsReading {};
        reading.snapshot.storage = StorageStats { .getHits = hits, .getMisses = misses };
        return reading;
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

    auto const failed = CacheFrames({ SampleOf(CacheReading(1, { "memory" }), 1),
                                      DashboardEvent { .kind = DashboardEventKind::SampleFailed, .at = TimePoint { 2s } },
                                      Tick },
                                    RenderRung::Unicode);
    auto const answered = CacheFrames({ SampleOf(CacheReading(1, { "memory" }), 1), Tick }, RenderRung::Unicode);
    REQUIRE(failed.size() == 1);
    REQUIRE(answered.size() == 1);

    CHECK(levelOf(failed[0], "items") == Absent);
    CHECK(levelOf(answered[0], "items") == "1 284 991");
}

TEST_CASE("the node panel's per-minute rate and mean compile come from the catalogue counters", "[cli][dashboard][panel]")
{
    // A figure's SCALE and its quotient are data in the node table, and nothing else draws them.
    // Ten jobs over two seconds is 300 per minute, not 5; twenty thousand milliseconds over those
    // ten jobs is a 2 s mean, not 2000. Read from a reading stating those two counters alone.
    auto const node = [](std::uint64_t jobs, std::uint64_t millis) {
        auto reading = CounterReading(IMetricsSink::Counter::WorkerJobsCompleted, jobs);
        reading.counters[static_cast<std::size_t>(IMetricsSink::Counter::WorkerCompileMillisTotal)] = millis;
        return reading;
    };
    auto view = PanelView {
        NodePanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ SampleOf(node(10, 1000), 1), SampleOf(node(20, 21000), 3), Tick }, DashboardLimits {}, view, sink);
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
    return { SampleOf(CacheReading(1, { "memory", "disk" }), 1), SampleOf(CacheReading(2, { "memory", "disk" }), 2), Tick };
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

/// Where a fleet frame's history chart is, from its title to its legend.
///
/// A band's first line names its machine exactly as a table row does, so a case that finds or counts the table's
/// rows by machine name reads around these lines -- or it counts a chart band as a machine row.
/// @param lines The frame's lines.
/// @return The chart's first line and the line after its legend; both the line count for a frame with no chart.
[[nodiscard]] std::pair<std::size_t, std::size_t> ChartLineRange(std::vector<std::string> const& lines)
{
    auto const title = static_cast<std::size_t>(
        std::ranges::find_if(lines, [](std::string const& line) { return line.contains(" per machine, last "); })
        - lines.begin());
    auto axis = title;
    while (axis < lines.size())
    {
        auto const cells = CodePoints(lines[axis]).size();
        if (cells >= 2 && Trimmed(Columns(lines[axis], 1, cells - 2)).ends_with("now"))
            break;
        ++axis;
    }
    if (axis == lines.size())
        return { lines.size(), lines.size() };
    return { title, std::min(lines.size(), axis + 2) };
}

/// The indices of @p frame's lines outside its history chart, top to bottom.
/// @param lines The frame's lines.
/// @return The indices.
[[nodiscard]] std::vector<std::size_t> LinesOutsideChart(std::vector<std::string> const& lines)
{
    auto const [first, last] = ChartLineRange(lines);
    auto indices = std::vector<std::size_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, lines.size()))
        if (index < first || index >= last)
            indices.push_back(index);
    return indices;
}

/// The frame line outside its chart whose content starts with @p prefix after the edge and indent, or nullopt.
/// @param frame The frame.
/// @param prefix The start.
/// @return The line.
[[nodiscard]] std::optional<std::string> LineStarting(std::string_view frame, std::string_view prefix)
{
    auto const lines = Lines(frame);
    for (auto const index: LinesOutsideChart(lines))
        if (Columns(lines[index], LabelFrom, FakeCellWidth(prefix)) == prefix)
            return lines[index];
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
/// @param document The leader's document, as its stream carried it.
/// @param where The endpoint the stream was read from; empty when the case does not say.
/// @return The event.
[[nodiscard]] DashboardEvent FleetSampleOf(int seconds, std::string document, std::string where = {})
{
    return DashboardEvent { .kind = DashboardEventKind::Sample,
                            .at = TimePoint { std::chrono::seconds { seconds } },
                            .document = std::move(document),
                            .where = std::move(where) };
}

/// A fleet frame's source text when its sample did not say where: what was asked, from the leader.
/// @return `source  subscription (SUBSCRIBE, leader)`.
[[nodiscard]] std::string FleetSourceText()
{
    return std::format("source  {} ({}, {})", SubscriptionSource, SubscriptionRoute, LeaderRole);
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

/// A leader's `/fleet.txt` for @p machines machines, rendered by the leader's own renderer.
///
/// Rendered rather than written, so every column a real leader sends is there under its real name, with
/// widths a real fleet has: a build string, a heartbeat past its threshold on the second machine.
/// @param machines How many machines.
/// @return The document.
[[nodiscard]] std::string LeaderFleetText(std::size_t machines)
{
    auto snapshot = Distributed::FleetSnapshot {};
    snapshot.role = Distributed::SchedulerRole::Leader;
    for (auto const index: std::views::iota(std::size_t { 1 }, machines + 1))
    {
        auto machine = Distributed::NodeReport {};
        machine.endpoint = std::format("build-{:02}:7070", index);
        machine.displayName = std::format("ci-{:02}", index);
        machine.version = "0.4.1-12-g0123abc";
        machine.fingerprints = { "gcc-13-abcdef", "clang-18-fedcba" };
        machine.capacity.logicalCores = 64;
        machine.capacity.totalMemoryBytes = 100552671232ULL;
        machine.load.cpuBusyPermille = 182;
        machine.load.freeScratchBytes = 442381631488ULL;
        machine.registeredSlots = 16;
        machine.heartbeatAge = std::chrono::milliseconds { index == 2 ? 71'300 : 900 };
        snapshot.nodes.push_back(std::move(machine));
    }
    snapshot.liveLeases = 47;
    return RenderFleetText(snapshot, Distributed::FleetHistoryView {}, std::nullopt);
}

/// Whether @p heading names the column @p name as a whole word.
/// @param heading A table's heading line.
/// @param name A column name.
/// @return True when the heading carries it, bounded by spaces or the line's ends.
[[nodiscard]] bool HeadingNames(std::string_view heading, std::string_view name)
{
    for (auto at = heading.find(name); at != std::string_view::npos; at = heading.find(name, at + 1))
    {
        auto const end = at + name.size();
        if ((at == 0 || heading[at - 1] == ' ') && (end == heading.size() || heading[end] == ' '))
            return true;
    }
    return false;
}

/// @p names joined by spaces, for a message.
/// @param names The names.
/// @return The text.
[[nodiscard]] std::string JoinedNames(std::vector<std::string> const& names)
{
    auto text = std::string {};
    for (auto const& name: names)
        text += (text.empty() ? "" : " ") + name;
    return text;
}

/// The index of the first line of @p frame holding @p text.
/// @param frame The frame.
/// @param text What to find.
/// @return The index; the line count when none holds it.
[[nodiscard]] std::size_t LineIndexHolding(std::string_view frame, std::string_view text)
{
    auto const lines = Lines(frame);
    auto const found = std::ranges::find_if(lines, [text](std::string const& line) { return line.contains(text); });
    return static_cast<std::size_t>(found - lines.begin());
}

/// How many lines of @p frame outside its chart start, after the edge and indent, with @p prefix.
/// @param frame The frame.
/// @param prefix The start.
/// @return The count.
[[nodiscard]] std::size_t LinesStarting(std::string_view frame, std::string_view prefix)
{
    auto const lines = Lines(frame);
    return static_cast<std::size_t>(std::ranges::count_if(LinesOutsideChart(lines), [&](std::size_t index) {
        return Columns(lines[index], LabelFrom, FakeCellWidth(prefix)) == prefix;
    }));
}

/// The index of the first line of @p frame outside its chart holding @p text.
/// @param frame The frame.
/// @param text What to find.
/// @return The index; the line count when none holds it.
[[nodiscard]] std::size_t TableLineIndexHolding(std::string_view frame, std::string_view text)
{
    auto const lines = Lines(frame);
    for (auto const index: LinesOutsideChart(lines))
        if (lines[index].contains(text))
            return index;
    return lines.size();
}

} // namespace

TEST_CASE("a fleet panel before its first reading draws every tile absent, the strip, and no table",
          "[cli][dashboard][panel][fleet]")
{
    // WHAT DISTINGUISHES: every headline figure has its tile under the page's label and each reads the
    // absent marker -- never a missing tile or a zero -- and where the table goes there is the marker
    // alone, never an empty table, which would say the fleet has no machines.
    auto const frames = FleetFramesAt({ Tick }, 132, 40);
    REQUIRE(frames.size() == 1);
    auto const& frame = frames.front();

    auto tiles = std::size_t { 0 };
    for (auto const& kpi: Distributed::FleetKpis())
    {
        INFO("tile " << kpi.label);
        auto const after = AfterWord(frame, kpi.label);
        REQUIRE(after.has_value());
        CHECK(Unwrap(after).starts_with(Absent));
        ++tiles;
    }
    CHECK(tiles == Distributed::FleetKpis().size());

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

    auto const kpis = Distributed::FleetKpis();
    REQUIRE(kpis.size() >= 3);
    CHECK(AfterWord(frame, kpis[0].label).value_or("").starts_with("12 884"));
    CHECK(AfterWord(frame, kpis[1].label).value_or("").starts_with("47"));
    CHECK(AfterWord(frame, kpis[1].label).value_or("").contains(std::format("of 192 {}", kpis[1].ofNoun)));
    CHECK(AfterWord(frame, kpis[2].label).value_or("").starts_with("88.1 %"));
    CHECK(frame.contains(FleetSourceText()));
}

TEST_CASE("a fleet table fills its width by the leader's rank, a narrow column in room a wide one cannot use",
          "[cli][dashboard][panel][fleet][keep]")
{
    // #134 F7: the positional rule is withdrawn, and dropping by rank alone left room empty. Over a machines
    // section whose every cell is the absent marker -- so a column is exactly as wide as its name -- at every
    // width from 240 cells down to the narrowest frame, with the ranks read back through `FleetColumnKeep`
    // and never restated here. WHAT DISTINGUISHES:
    //   - the identity column is always drawn;
    //   - no column left out would fit in the room the drawn ones leave: a fit that stops at the first
    //     column too wide fails this;
    //   - no column left out outranks a drawn one it could have replaced: a fit in drawing order fails this;
    //   - at some width a column of a lower rank IS drawn while one of a higher rank is not, so the filling
    //     is observed rather than vacuous.
    auto const names = Distributed::FleetColumnNames(FleetSection::Machines, Distributed::FleetSnapshot {});
    auto document =
        std::format("# {}\n", Distributed::FleetSectionTable[static_cast<std::size_t>(FleetSection::Machines)].key);
    for (auto const index: std::views::iota(std::size_t { 0 }, names.size()))
        document += (index == 0 ? "" : "\t") + names[index];
    document += "\n";
    for (auto const machine: std::views::iota(1, 4))
    {
        document += std::format("m{}", machine);
        for (auto const index: std::views::iota(std::size_t { 1 }, names.size()))
        {
            (void) index;
            document += "\t-";
        }
        document += "\n";
    }
    auto const rank = [](std::string const& name) {
        return std::to_underlying(Unwrap(Distributed::FleetColumnKeep(FleetSection::Machines, name)));
    };
    auto const gap = std::size_t { 2 };

    auto filled = false;
    auto narrowestFrame = std::optional<int> {};
    for (auto const columns: std::views::iota(12, 241) | std::views::reverse)
    {
        auto const frames = FleetFramesAt({ FleetSampleOf(1, document), Tick }, columns, 60);
        REQUIRE(frames.size() == 1);
        if (!frames.front().starts_with("\xe2\x94\x8c"))
            break;
        narrowestFrame = columns;
        auto const heading = LineStarting(frames.front(), "endpoint");
        REQUIRE(heading.has_value());
        auto shown = std::vector<std::string> {};
        auto hidden = std::vector<std::string> {};
        auto used = std::size_t { 2 }; // the indent
        for (auto const& name: names)
        {
            if (HeadingNames(Unwrap(heading), name))
            {
                shown.push_back(name);
                used += FakeCellWidth(name) + (name == names.front() ? 0 : gap);
            }
            else
                hidden.push_back(name);
        }
        auto const budget = ContentColumns(static_cast<std::size_t>(columns));
        REQUIRE(used <= budget);
        auto const room = budget - used;
        INFO(columns << " columns: drawn " << JoinedNames(shown) << "; left out " << JoinedNames(hidden) << "; room "
                     << room);
        CHECK(std::ranges::find(shown, names.front()) != shown.end());
        for (auto const& out: hidden)
        {
            CHECK(FakeCellWidth(out) + gap > room);
            for (auto const& in: shown)
                if (in != names.front() && rank(in) > rank(out))
                    CHECK(FakeCellWidth(out) + gap > room + FakeCellWidth(in) + gap);
            filled = filled || std::ranges::any_of(shown, [&](std::string const& in) { return rank(in) > rank(out); });
        }
    }
    REQUIRE(narrowestFrame.has_value());
    CHECK(filled);

    auto const below = FleetFrameAt(FleetMachines, Unwrap(narrowestFrame) - 1, 60);
    CHECK(Lines(below).size() == 1);
    CHECK(below.contains("needs "));
}

TEST_CASE("a fleet table taller than its room gives up rows before the tiles, and says how to reach them",
          "[cli][dashboard][panel][fleet]")
{
    // §5 at 80x24: the tiles, the strip, some machines and `... 8 more machines; PgDn scrolls, / filters`.
    // WHAT DISTINGUISHES: at the tallest height that cannot show every machine, the first tile, the strip
    // and the source line are all STILL drawn, and the overflow line counts exactly the machines not shown
    // -- the rule this replaced dropped every one of them before hiding a row. One line taller shows every
    // machine. Much shorter, the tiles go while the table still keeps the rows it keeps.
    auto const& spec = Unwrap(FleetPanel().document);
    auto shrunkAt = std::optional<int> {};
    for (auto const rows: std::views::iota(4, 41) | std::views::reverse)
        if (FleetFrameAt(FleetMachines, 132, rows).contains(" more "))
        {
            shrunkAt = rows;
            break;
        }
    REQUIRE(shrunkAt.has_value());

    auto const frame = FleetFrameAt(FleetMachines, 132, Unwrap(shrunkAt));
    CHECK(Lines(frame).size() <= static_cast<std::size_t>(Unwrap(shrunkAt)));
    CHECK(LineStarting(frame, "endpoint").has_value());
    CHECK(frame.contains(Distributed::FleetKpis().front().label));
    CHECK(frame.contains("[machines]"));
    CHECK(frame.contains(FleetSourceText()));
    auto const shown = LinesStarting(frame, "build-");
    CHECK(frame.contains(std::format("... {} more machines; PgDn scrolls, / filters", FleetMachines - shown)));

    auto const taller = FleetFrameAt(FleetMachines, 132, Unwrap(shrunkAt) + 1);
    CHECK(!taller.contains(" more "));
    CHECK(LinesStarting(taller, "build-") == FleetMachines);

    auto keptWithoutTiles = std::optional<int> {};
    for (auto const rows: std::views::iota(4, Unwrap(shrunkAt)) | std::views::reverse)
    {
        auto const shorter = FleetFrameAt(FleetMachines, 132, rows);
        if (!shorter.contains(Distributed::FleetKpis().back().label))
        {
            keptWithoutTiles = rows;
            CHECK(LinesStarting(shorter, "build-") == spec.tableRowsKept);
            break;
        }
    }
    CHECK(keptWithoutTiles.has_value());
}

TEST_CASE("a fleet panel drawn after a failed sample shows no table from before it", "[cli][dashboard][panel][fleet]")
{
    // A leader that stopped answering is a gap, not its last fleet. WHAT DISTINGUISHES: the frame after
    // the refusal has no machine rows and the marker where the table goes, while the frame before it has
    // every row -- a panel keeping the last document passes the first half and fails the second.
    auto const frames = FleetFramesAt({ FleetSampleOf(1, FleetText(FleetMachines)),
                                        Tick,
                                        DashboardEvent { .kind = DashboardEventKind::SampleFailed,
                                                         .at = TimePoint { 2s },
                                                         .outcome = Outcome::Refused,
                                                         .note = "not the leader" },
                                        Tick },
                                      132,
                                      40);
    REQUIRE(frames.size() == 2);
    CHECK(LinesStarting(frames[0], "build-") == FleetMachines);
    CHECK(LinesStarting(frames[1], "build-") == 0);
    CHECK(AfterWord(frames[1], Distributed::FleetKpis().front().label).value_or("").starts_with(Absent));
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

/// The cell under @p heading in the row of @p frame, outside its chart, whose first cell starts with @p rowStart, trimmed.
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
    auto const outside = LinesOutsideChart(lines);
    auto const headingPoints = CodePoints(heading).size();
    // The chart's title names its figure as a heading does, so the heading is looked for outside it too.
    for (auto const headerAt: outside)
    {
        auto const& header = lines[headerAt];
        auto const points = CodePoints(header);
        for (auto const start: std::views::iota(std::size_t { 1 }, points.size()))
        {
            auto const end = start + headingPoints;
            if (Columns(header, start, headingPoints) != heading || points[start - 1] != " "
                || (end < points.size() && points[end] != " "))
                continue;
            auto const row = std::ranges::find_if(outside, [&](std::size_t index) {
                return Trimmed(Columns(lines[index], LabelFrom, FakeCellWidth(lines[index]))).starts_with(rowStart);
            });
            if (row == outside.end())
                return std::nullopt;
            // Back from the heading's last column to the gap before the cell.
            auto const cell = Columns(lines[*row], 0, end);
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
    REQUIRE(drawing.placements[0].size() == 2);
    REQUIRE(drawing.requests.size() == 2);
    auto const lines = Lines(drawing.frames[0]);
    for (auto const image: { std::size_t { 0 }, std::size_t { 1 } })
    {
        INFO("image " << image);
        auto const& placement = drawing.placements[0][image];
        auto const& request = drawing.requests[image];
        CHECK(request.width == placement.cellsWide * ChartCell.width);
        CHECK(request.height == placement.cellsHigh * ChartCell.height);
        CHECK(placement.sixel == std::format("sixel:{}x{}", request.width, request.height));
        REQUIRE(placement.row + placement.cellsHigh - 1 <= lines.size());
        for (auto const row: std::views::iota(placement.row, placement.row + placement.cellsHigh))
        {
            INFO("frame row " << row);
            CHECK(Trimmed(Columns(lines[row - 1], placement.column - 1, placement.cellsWide)).empty());
        }
    }
    auto const& placement = drawing.placements[0][0];
    auto const& request = drawing.requests[0];
    CHECK(placement.cellsHigh <= Unwrap(FleetPanel().document).chartGrowth.cellsHighMost);
    auto drawn = std::size_t { 0 };
    for (auto const pixel: std::views::iota(std::size_t { 0 }, request.pixels.size() / 4))
        drawn += request.pixels[(pixel * 4) + 3] != 0 ? 1 : 0;
    CHECK(drawn > 0);
}

TEST_CASE("below the Sixel rung, or without a cell size, the fleet chart is text in the rows the image had",
          "[cli][dashboard][panel][fleet][chart]")
{
    // One layout, two ways to draw its bands. WHAT DISTINGUISHES: the Unicode frame asks the encoder for nothing and
    // places nothing, yet its chart title is there, on the Sixel frame's row, and its strip is lower only by its
    // spacer rows -- and a Sixel rung with no cell size draws exactly the Unicode frame, since no size is ever
    // guessed.
    auto const sixel = DrawChart(RenderRung::Sixel, ChartCell, 100, 60);
    auto const unicode = DrawChart(RenderRung::Unicode, ChartCell, 100, 60);
    auto const unmeasured = DrawChart(RenderRung::Sixel, std::nullopt, 100, 60);
    REQUIRE(unicode.frames.size() == 1);
    REQUIRE(unmeasured.frames.size() == 1);
    REQUIRE(sixel.placements.at(0).size() == 2);
    CHECK(unicode.requests.empty());
    CHECK(unicode.placements.at(0).empty());
    CHECK(unicode.frames[0].contains(std::format("{} per machine, last ", FleetChartMetrics.front().key)));
    // The same layout: the title on the same row, and the strip lower by exactly the text chart's two spacer rows,
    // one between each two of its three bands (#134 F-b) -- here the bands are at their tallest on both rungs, so the
    // spacers are rows the image left over. A text chart laid out apart from the image one would move either.
    CHECK(LineIndexHolding(unicode.frames[0], " per machine, last ")
          == LineIndexHolding(sixel.frames.at(0), " per machine, last "));
    CHECK(LineIndexHolding(unicode.frames[0], "[machines]") == LineIndexHolding(sixel.frames.at(0), "[machines]") + 2);
    CHECK(unmeasured.requests.empty());
    CHECK(unmeasured.placements.at(0).empty());
    CHECK(unmeasured.frames[0] == unicode.frames[0]);
}

TEST_CASE("the fleet chart goes for height before the tiles, and below its width it is not drawn",
          "[cli][dashboard][panel][fleet][chart]")
{
    // The chart takes only rows nothing else wants. WHAT DISTINGUISHES: at the tallest height without the
    // chart, the headline tiles are still there -- the chart went first -- one row taller it is back with one
    // band, and its title says which of the three machines that is; at a width under the chart's minimum there
    // is no image while the table still draws.
    auto shortest = std::optional<int> {};
    for (auto const rows: std::views::iota(8, 61) | std::views::reverse)
        if (DrawChart(RenderRung::Sixel, ChartCell, 100, rows).placements.at(0).empty())
        {
            shortest = rows;
            break;
        }
    REQUIRE(shortest.has_value());
    auto const without = DrawChart(RenderRung::Sixel, ChartCell, 100, Unwrap(shortest));
    CHECK(without.frames.at(0).contains(Distributed::FleetKpis().front().label));
    auto const oneBand = DrawChart(RenderRung::Sixel, ChartCell, 100, Unwrap(shortest) + 1);
    CHECK(oneBand.placements.at(0).size() == 2);
    CHECK(oneBand.frames.at(0).contains("; the first 1 of 3 by name"));

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

/// What one fleet session drew, frame by frame, and how it ended.
struct FleetRun
{
    std::vector<std::string> frames {};                     ///< Each frame's text.
    std::vector<std::vector<FramePlacement>> placements {}; ///< Each frame's images.
    std::vector<std::vector<FrameSpan>> spans {};           ///< Each frame's dressed runs.
    DashboardStop stop { DashboardStop::SourceDetached };   ///< How the run ended.
};

/// Draw @p script through the fleet panel after a `Resize` of @p columns by @p rows.
/// @param script The events after the resize.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @param context The panel's context; its width function is the fake one.
/// @param cellPixels The cell size the resize reports, or nullopt.
/// @return What was drawn.
[[nodiscard]] FleetRun RunFleet(std::vector<DashboardEvent> script,
                                int columns,
                                int rows,
                                PanelContext context,
                                std::optional<CellPixelSize> cellPixels = std::nullopt)
{
    script.insert(
        script.begin(),
        DashboardEvent { .kind = DashboardEventKind::Resize, .columns = columns, .rows = rows, .cellPixels = cellPixels });
    context.cellWidth = &FakeCellWidth;
    auto view = PanelView { FleetPanel(), std::move(context) };
    auto sink = CollectingSink {};
    auto const exit = Drive(std::move(script), DashboardLimits {}, view, sink, &ReadFleetSample);
    return FleetRun { .frames = std::move(sink.frames),
                      .placements = std::move(sink.placements),
                      .spans = std::move(sink.spans),
                      .stop = exit.stop };
}

/// A key event.
/// @param keys The bytes.
/// @return The event.
[[nodiscard]] DashboardEvent KeyOf(std::string_view keys)
{
    return DashboardEvent { .kind = DashboardEventKind::Key, .keys = std::string { keys } };
}

/// The text a span dresses.
/// @param frame The frame's text.
/// @param span The span.
/// @return The bytes it covers.
[[nodiscard]] std::string SpanText(std::string_view frame, FrameSpan const& span)
{
    return Lines(frame).at(span.row - 1).substr(span.byte, span.length);
}

/// The number in the first machine row's endpoint, `build-NN`.
/// @param frame The frame.
/// @return NN, or zero with no machine row.
[[nodiscard]] int FirstMachine(std::string_view frame)
{
    auto const row = LineStarting(frame, "build-");
    if (!row.has_value())
        return 0;
    auto const& line = Unwrap(row);
    auto const at = line.find("build-");
    return std::stoi(line.substr(at + 6, 2));
}

/// The plain Unicode context the parity cases draw with.
/// @return The context.
[[nodiscard]] PanelContext UnicodeContext()
{
    return PanelContext { .absent = std::string { Absent }, .rung = RenderRung::Unicode };
}

} // namespace

TEST_CASE("at 80x24 the fleet panel reads as the mockup draws it", "[cli][dashboard][panel][fleet][parity]")
{
    // #134 §5, the owner's report. WHAT DISTINGUISHES, each against the frame the owner saw:
    //   - every tile under the page's LABEL -- the frame that was sent back said `compiling-now`;
    //   - two columns of tiles, `Dispatched` beside `Cache hit rate`, where it stacked seven;
    //   - the qualifiers `of 192 slots` and `not yet resolved`, where it wrote a bare `of 30` and nothing;
    //   - `heartbeat-age` and `cpu-busy` in the heading at 80, `memory`, `class` and `version` gone, where
    //     it dropped from the right and kept `version`;
    //   - the strip's `keys  m w l c t`, right-aligned against the frame's blank column.
    auto const run = RunFleet({ FleetSampleOf(1, LeaderFleetText(FleetMachines)), Tick }, 80, 24, UnicodeContext());
    REQUIRE(run.frames.size() == 1);
    auto const& frame = run.frames.front();
    INFO(frame);

    for (auto const& kpi: Distributed::FleetKpis())
    {
        INFO("tile " << kpi.label);
        CHECK(frame.contains(kpi.label));
    }
    auto const kpis = Distributed::FleetKpis();
    auto const first = LineStarting(frame, kpis[0].label);
    REQUIRE(first.has_value());
    auto const* const beside = FindIfOrNull(
        kpis, [](Distributed::FleetKpiText const& kpi) { return kpi.ofNoun.empty() && kpi.note.empty() && !kpi.sparkline; });
    REQUIRE(beside != nullptr);
    CHECK(Unwrap(first).contains(beside->label));
    CHECK(AfterWord(frame, "Compiling now").value_or("").contains("of 192 slots"));
    CHECK(AfterWord(frame, "Leases outstanding").value_or("").contains("not yet resolved"));

    auto const heading = LineStarting(frame, "endpoint");
    REQUIRE(heading.has_value());
    CHECK(HeadingNames(Unwrap(heading), "heartbeat-age"));
    CHECK(HeadingNames(Unwrap(heading), "cpu-busy"));
    CHECK_FALSE(HeadingNames(Unwrap(heading), "memory"));
    CHECK_FALSE(HeadingNames(Unwrap(heading), "class"));
    CHECK_FALSE(HeadingNames(Unwrap(heading), "version"));

    auto const strip = LineIndexHolding(frame, "[machines]");
    REQUIRE(strip < Lines(frame).size());
    CHECK(Lines(frame)[strip].ends_with("keys  m w l c t \xe2\x94\x82"));

    CHECK(Lines(frame).size() == 24);
    CHECK(WidestLine(frame) <= 80);
}

TEST_CASE("the active section's tab is dressed as selected, and a letter from the strip's keys switches it",
          "[cli][dashboard][panel][fleet][parity]")
{
    // #134 F6. WHAT DISTINGUISHES: the one Selected span covers exactly the bracketed tab -- not the strip,
    // not the key alone -- and after `w` it covers `[workers]`, so the dress follows the section rather
    // than a position.
    auto const run = RunFleet({ FleetSampleOf(1, LeaderFleetText(3)), Tick, KeyOf("w") }, 132, 40, UnicodeContext());
    REQUIRE(run.frames.size() == 2);
    for (auto const& [index, tab]: { std::pair { std::size_t { 0 }, std::string_view { "[machines]" } },
                                     std::pair { std::size_t { 1 }, std::string_view { "[workers]" } } })
    {
        INFO("frame " << index);
        auto selected = std::vector<std::string> {};
        for (auto const& span: run.spans.at(index))
            if (span.tone == FrameTone::Selected)
                selected.push_back(SpanText(run.frames[index], span));
        CHECK(selected == std::vector<std::string> { std::string { tab } });
    }
}

namespace
{

/// The endpoint the chrome fixtures' cache sessions subscribe at, and their streams answered from.
constexpr std::string_view ChromeEndpoint = "127.0.0.1:6379";

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

/// A cache reading carrying what §3's title bar states: a version, and an uptime of `6d04:12`.
/// @return The event.
[[nodiscard]] DashboardEvent CacheChromeSample()
{
    constexpr auto SixDaysAndABit = std::chrono::seconds { (6 * 24 * 60 * 60) + (4 * 60 * 60) + (12 * 60) };
    auto reading = CacheReading(1, { "memory" });
    reading.snapshot.uptime = Uptime { SixDaysAndABit };
    reading.version = "0.4.1";
    return SampleOf(std::move(reading), 1, std::string { ChromeEndpoint });
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

TEST_CASE("a cache panel titles itself with the version its reading carries, and with the marker for none",
          "[cli][dashboard][panel][chrome]")
{
    // WHAT DISTINGUISHES: both directions. The version is the reading's own (`StatsReading::version`, the build that
    // captured it), so a reading stating one titles the panel with it, and a reading stating none titles it with the
    // marker -- never an empty word, and never a version from anywhere but the reading (#134 C1).
    auto const titleOf = [](std::string version) {
        auto reading = CacheReading(1, { "memory" });
        reading.version = std::move(version);
        auto const frames = CacheChromeFrames({ SampleOf(std::move(reading), 1), Tick }, 80);
        REQUIRE(frames.size() == 1);
        return TopEdge(frames.front());
    };

    CHECK(titleOf("0.4.1").starts_with(TopStart("fastcached 0.4.1")));
    CHECK(titleOf({}).starts_with(TopStart(std::format("fastcached {}", Absent))));
}

TEST_CASE("a cache panel is titled by the server that answered, and a panel whose subject is not a process keeps its own",
          "[cli][dashboard][panel][chrome]")
{
    // #1399 D6: a compile node serves the cache subject, and titled `fastcached` the panel named a process that was not
    // at the address. WHAT DISTINGUISHES: the same cache reading under a node's name titles itself with that name; with
    // none stated it is `fastcached`; and the fleet panel handed a server name still says `fleet`, since its subject
    // is the fleet rather than the process that sent it.
    auto const titled = [](PanelSpec const& panel, std::string server, DashboardEvent sample) {
        auto view = PanelView { panel,
                                PanelContext { .absent = std::string { Absent },
                                               .endpoint = "127.0.0.1:6379",
                                               .server = std::move(server),
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
        return TopEdge(sink.frames.front());
    };

    CHECK(titled(CachePanel(), "fastcache-compile-node", CacheChromeSample())
              .starts_with(TopStart("fastcache-compile-node 0.4.1")));
    CHECK(titled(CachePanel(), {}, CacheChromeSample()).starts_with(TopStart("fastcached 0.4.1")));
    CHECK(titled(FleetPanel(), "fastcache-compile-node", CacheChromeSample()).starts_with(TopStart("fleet")));
}

TEST_CASE("a panel's title states the cadence the server granted and the asked interval while none is known",
          "[cli][dashboard][panel][chrome]")
{
    // #1399 D10. The server keeps the cadence: a node whose floor is not this build's paces the stream at its own.
    // WHAT DISTINGUISHES: asked every 2 s and granted 5 s, the title says `every 5s` -- a title reading the asked
    // interval says `every 2s` over a stream arriving every five. And a failed sample carries no grant, so the title
    // after it says `every 2s` again -- a title keeping the last grant passes the first half and fails the second.
    auto granted = CacheChromeSample();
    granted.cadence = std::chrono::milliseconds { 5000 };
    auto const frames = CacheChromeFrames(
        { granted, Tick, DashboardEvent { .kind = DashboardEventKind::SampleFailed, .at = TimePoint { 3s } }, Tick }, 80);
    REQUIRE(frames.size() == 2);
    auto const whileGranted = TopEdge(frames[0]);
    CHECK(whileGranted.ends_with(TopEnd("127.0.0.1:6379  up 6d04:12  every 5s  q quit")));
    CHECK_FALSE(whileGranted.contains("every 2s"));
    auto const afterFailure = TopEdge(frames[1]);
    CHECK(afterFailure.contains("every 2s"));
    CHECK_FALSE(afterFailure.contains("every 5s"));

    // The control: a reading that carried no grant states the asked interval from the first frame.
    auto const ungranted = CacheChromeFrames({ CacheChromeSample(), Tick }, 80);
    REQUIRE(ungranted.size() == 1);
    CHECK(TopEdge(ungranted.front()).ends_with(TopEnd("127.0.0.1:6379  up 6d04:12  every 2s  q quit")));
}

TEST_CASE("a cache panel's source line names the source, what was asked and where, and counts at the edge",
          "[cli][dashboard][panel][chrome]")
{
    // §3: `source  subscription (SUBSCRIBE at 127.0.0.1:6379)     148 samples, 1 gap`. WHAT DISTINGUISHES: the
    // address is the one the stream was READ from, and the counts end one blank column before the right edge
    // rather than trailing the source.
    auto const frames = CacheChromeFrames({ CacheChromeSample(), Tick }, 80);
    REQUIRE(frames.size() == 1);
    auto const row = SourceRow(frames.front());
    REQUIRE(row.has_value());
    CHECK(
        Columns(Unwrap(row), 1, 80)
            .starts_with(std::format("  source  {} ({} at {})   ", SubscriptionSource, SubscriptionRoute, ChromeEndpoint)));
    CHECK(Unwrap(row).ends_with(std::format("1 sample, 0 gaps {}", GlyphsFor(RenderRung::Unicode).vertical)));
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
    // `source  subscription (SUBSCRIBE at build-07:7070)`. The version and the uptime come from the node's own
    // status, which its reading here does not carry.
    auto sample = SampleOf(StatsReading {}, 1, "build-07:7070");
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
    CHECK(Columns(Unwrap(row), 1, 80).starts_with("  source  subscription (SUBSCRIBE at build-07:7070)   "));
}

TEST_CASE("a fleet panel's title bar names the leader once it answered as one, and its source line says so",
          "[cli][dashboard][panel][chrome][fleet]")
{
    // §5: `fleet ───── leader build-01:7071  12 machines  every 5s  q ─┐` and
    // `source  subscription (SUBSCRIBE at build-01:7071, leader)` -- the cache and node panels' shape, with the
    // role inside the brackets, sparing the address for the counts where both do not fit. WHAT DISTINGUISHES: before a
    // reading the address is not called the leader's -- nothing has shown it to be one -- and the machine count is the
    // marker.
    auto sample = FleetSampleOf(1, FleetText(FleetMachines), "10.0.0.4:6674");
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
    // At 80 columns the address -- in the title bar already -- goes so the counts can stay (lane-livestats).
    CHECK(Columns(Unwrap(row), 1, 80).starts_with("  source  subscription (SUBSCRIBE, leader)   "));
    CHECK(Unwrap(row).ends_with(std::format("1 sample, 0 gaps {}", GlyphsFor(RenderRung::Unicode).vertical)));
}

TEST_CASE("at 80 columns a fleet source line spares the address before its counts, and a wider one keeps both",
          "[cli][dashboard][panel][chrome][fleet]")
{
    // lane-livestats, on the owner's demo: `subscription (SUBSCRIBE at 127.0.0.1:26674, leader)` and `14 samples, 0 gaps`
    // are 82 cells at 80 columns, and the counts went. WHAT DISTINGUISHES: at 80 the row ENDS with the counts and says
    // `SUBSCRIBE, leader` (the title bar names the address); at 120 the same run says the address AND the counts --
    // a line that dropped the address always would pass the first half and fail the second.
    auto const lastSourceRow = [](int columns) {
        auto script = std::vector<DashboardEvent> { DashboardEvent {
            .kind = DashboardEventKind::Resize, .columns = columns, .rows = 40 } };
        for (auto const second: std::views::iota(1, 15))
        {
            script.push_back(FleetSampleOf(second, FleetText(FleetMachines), "127.0.0.1:26674"));
            script.push_back(Tick);
        }
        auto view = PanelView { FleetPanel(),
                                PanelContext { .absent = std::string { Absent },
                                               .endpoint = "127.0.0.1:26674",
                                               .interval = 5s,
                                               .cellWidth = &FakeCellWidth,
                                               .rung = RenderRung::Unicode } };
        auto sink = CollectingSink {};
        (void) Drive(std::move(script), DashboardLimits {}, view, sink, &ReadFleetSample);
        REQUIRE(sink.frames.size() == 14);
        auto row = SourceRow(sink.frames.back());
        REQUIRE(row.has_value());
        return Unwrap(row);
    };
    auto const counts = std::format("14 samples, 0 gaps {}", GlyphsFor(RenderRung::Unicode).vertical);

    auto const narrow = lastSourceRow(80);
    CHECK(narrow.ends_with(counts));
    CHECK(narrow.contains(std::format("  source  {} ({}, {})   ", SubscriptionSource, SubscriptionRoute, LeaderRole)));

    auto const wide = lastSourceRow(120);
    CHECK(wide.ends_with(counts));
    CHECK(wide.contains(
        std::format("  source  {} ({} at 127.0.0.1:26674, {})   ", SubscriptionSource, SubscriptionRoute, LeaderRole)));
}

namespace
{

using NodeCounter = IMetricsSink::Counter;

/// What the machine under a node reading is doing.
struct NodeMachine
{
    /// Host-wide CPU busy over every interval, in permille; §4's 625 is 10 of 16 cores, 4 of them somebody else's.
    std::uint64_t cpuPermille { 625 };
    /// Memory a new process could obtain.
    std::uint64_t availableMemoryBytes { std::uint64_t { 32 } << 30U };
};

/// A node reading @p step intervals in: every counter the node panel draws, moved by @p step.
/// @param step How far the counters have moved.
/// @param seconds When it was taken.
/// @param status What the node said about itself, or nothing.
/// @param machine What its machine is doing.
/// @return The event.
[[nodiscard]] DashboardEvent NodeSampleOf(std::uint64_t step,
                                          int seconds,
                                          std::optional<CompileCacheWire::NodeStatusFields> status,
                                          NodeMachine machine = {})
{
    auto reading = StatsReading {};
    auto const count = [&reading](NodeCounter counter, std::uint64_t value) {
        reading.counters[static_cast<std::size_t>(counter)] = value;
    };
    count(NodeCounter::WorkerJobsCompleted, 41 * step);
    count(NodeCounter::WorkerCompileMillisTotal, 75440 * step);
    count(NodeCounter::WorkerJobsRefusedNoSlot, 2 * step);
    count(NodeCounter::WorkerJobsRefusedLeaseExpired, step);
    count(NodeCounter::WorkerJobsRefusedUnknownFingerprint, 0);
    count(NodeCounter::NodeCacheHits, 881 * step);
    count(NodeCounter::NodeCacheMisses, 119 * step);
    // §4's machine: 16 cores registered with 16 slots, 6 running, and by default somebody else using 4 cores' worth
    // with room in memory and on scratch, so the CPU is what binds: 12 available.
    reading.snapshot.host = HostCapacity { .logicalCores = 16,
                                           .configuredSlots = 16,
                                           .totalMemoryBytes = std::uint64_t { 64 } << 30U,
                                           .diskCapacityBytes = std::uint64_t { 512 } << 30U,
                                           .diskFreeBytes = std::uint64_t { 41 } << 30U,
                                           .busySlots = 6 };
    reading.snapshot.hostLoad =
        HostLoadReading { .cpu = CpuTicks { .busy = machine.cpuPermille * step, .total = 1000 * step },
                          .availableMemoryBytes = machine.availableMemoryBytes };
    // Its cache tier holds 2 of its 8 GiB.
    reading.snapshot.storage =
        StorageStats { .bytesUsed = std::size_t { 2 } << 30U, .bytesLimit = std::size_t { 8 } << 30U };
    auto sample = SampleOf(std::move(reading), seconds, "build-07:7070");
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

/// What a panel presented: its frames, and the runs each dresses.
struct Presented
{
    std::vector<std::string> frames {};           ///< Each frame's text.
    std::vector<std::vector<FrameSpan>> spans {}; ///< Each frame's runs, index for index.
};

/// What a node panel asking `build-07:7070` every two seconds presents for @p script: its frames and their runs.
/// @param script The events after the resize.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return What it presented.
[[nodiscard]] Presented NodeSinkAt(std::vector<DashboardEvent> script, int columns, int rows)
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
    return Presented { .frames = std::move(sink.frames), .spans = std::move(sink.spans) };
}

/// The frames a node panel asking `build-07:7070` every two seconds draws for @p script.
/// @param script The events after the resize.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frames.
[[nodiscard]] std::vector<std::string> NodeFramesAt(std::vector<DashboardEvent> script, int columns, int rows)
{
    return NodeSinkAt(std::move(script), columns, rows).frames;
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

TEST_CASE("a node panel draws its slots as section 4 does: three numbers, the gauge, the limit and its remedy",
          "[cli][dashboard][panel][node]")
{
    // N4. WHAT DISTINGUISHES: `available` and `limited-by` are the scheduler's own ceilings over the machine's load
    // (12 of 16, bound by the CPU somebody else is using); the gauge's three parts are 6 running, 6 more available and
    // 4 withdrawn; and the remedy is `SlotLimitTable`'s sentence, wrapped under the gauge at the value column.
    auto const frame = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const slots = LineStarting(frame, "slots");
    REQUIRE(slots.has_value());
    CHECK(Trimmed(Columns(Unwrap(slots), 1, 78)) == "slots       6 in flight / 12 available / 16 registered");
    auto const gauge = LineUnder(frame, "slots");
    REQUIRE(gauge.has_value());
    CHECK(ColumnOf(Unwrap(gauge), "\u2588") == ColumnOf(Unwrap(slots), "6 in flight"));
    CHECK(Trimmed(Columns(Unwrap(gauge), 1, 78))
          == "\u2588\u2588\u2588\u2588\u2588\u2588\u2592\u2592\u2592\u2592\u2592\u2592\u2591\u2591\u2591\u2591   limited-by "
             " external-cpu");

    auto const lines = Lines(frame);
    auto const at = std::ranges::find_if(lines, [](std::string const& line) { return line.contains("limited-by"); });
    REQUIRE(std::distance(at, lines.end()) > 2);
    CHECK(ColumnOf(*(at + 1), "somebody") == ColumnOf(Unwrap(slots), "6 in flight"));
    CHECK(std::format("{} {}", Trimmed(Columns(*(at + 1), 1, 78)), Trimmed(Columns(*(at + 2), 1, 78)))
          == Distributed::TraitsFor(Distributed::SlotLimit::ExternalCpu).remedy);
}

TEST_CASE("a node's slots name no limit before two readings, and nothing to remedy when none binds",
          "[cli][dashboard][panel][node]")
{
    // The ceilings take a CPU share, which one reading cannot give: `available` and `limited-by` are the marker by
    // name, with no gauge and no remedy. And a machine nothing holds back names `registered` with no sentence under
    // it, since there is nothing to do.
    auto const first = NodeFramesAt({ NodeSampleOf(1, 1, MockupNodeStatus()), Tick }, 80, 24);
    REQUIRE(first.size() == 1);
    CHECK(ContentStarting(first.front(), "slots")
          == std::format("slots       6 in flight / {} available / 16 registered", Absent));
    auto const unknown = LineUnder(first.front(), "slots");
    REQUIRE(unknown.has_value());
    CHECK(Trimmed(Columns(Unwrap(unknown), 1, 78)) == std::format("limited-by  {}", Absent));
    // No remedy under it: the line after `limited-by` is the blank that ends the block.
    auto const lines = Lines(first.front());
    auto const at = std::ranges::find_if(lines, [](std::string const& line) { return line.contains("limited-by"); });
    REQUIRE(std::distance(at, lines.end()) > 1);
    CHECK(Trimmed(Columns(*(at + 1), 1, 78)).empty());

    // Two readings that are not ADJACENT name no limit either: the second stamped no later than the first continues no
    // run, so the fold measured no interval and no CPU share can be taken across the pair.
    auto const unordered =
        NodeFramesAt({ NodeSampleOf(1, 3, MockupNodeStatus()), NodeSampleOf(2, 1, MockupNodeStatus()), Tick }, 80, 24);
    REQUIRE(unordered.size() == 1);
    CHECK(ContentStarting(unordered.front(), "slots")
          == std::format("slots       6 in flight / {} available / 16 registered", Absent));

    auto const idle = NodeMachine { .cpuPermille = 0 };
    auto const frames = NodeFramesAt(
        { NodeSampleOf(1, 1, MockupNodeStatus(), idle), NodeSampleOf(2, 3, MockupNodeStatus(), idle), Tick }, 80, 24);
    REQUIRE(frames.size() == 1);
    CHECK(ContentStarting(frames.front(), "slots") == "slots       6 in flight / 16 available / 16 registered");
    auto const free = LineUnder(frames.front(), "slots");
    REQUIRE(free.has_value());
    CHECK(Trimmed(Columns(Unwrap(free), 1, 78)).ends_with("   limited-by  registered"));
    // No sentence under it. Asked of the LINE after the gauge rather than of the whole remedy's text, which a wrap
    // splits across two lines and so would be absent from the frame either way.
    auto const freeLines = Lines(frames.front());
    auto const gaugeLine =
        std::ranges::find_if(freeLines, [](std::string const& line) { return line.contains("limited-by"); });
    REQUIRE(std::distance(gaugeLine, freeLines.end()) > 1);
    CHECK(Trimmed(Columns(*(gaugeLine + 1), 1, 78)).empty());
}

TEST_CASE("a node short of memory is limited by memory, with memory's remedy", "[cli][dashboard][panel][node]")
{
    // The other limit, so a panel that named the CPU whatever bound is caught: 2 GiB left supports two jobs on top of
    // the six running, so 8 are available -- fewer than the CPU leaves -- and the gauge withdraws 8.
    auto const tight = NodeMachine { .cpuPermille = 625, .availableMemoryBytes = std::uint64_t { 2 } << 30U };
    auto const frames = NodeFramesAt(
        { NodeSampleOf(1, 1, MockupNodeStatus(), tight), NodeSampleOf(2, 3, MockupNodeStatus(), tight), Tick }, 80, 24);
    REQUIRE(frames.size() == 1);
    auto const& frame = frames.front();
    CHECK(ContentStarting(frame, "slots") == "slots       6 in flight / 8 available / 16 registered");
    auto const gauge = LineUnder(frame, "slots");
    REQUIRE(gauge.has_value());
    CHECK(Trimmed(Columns(Unwrap(gauge), 1, 78))
          == "\u2588\u2588\u2588\u2588\u2588\u2588\u2592\u2592\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591   limited-by "
             " memory");
    CHECK(frame.contains(Distributed::TraitsFor(Distributed::SlotLimit::Memory).remedy));
    CHECK(ContentStarting(frame, "host") == "host        cpu-busy 62.5 %   mem free 2.00 GiB   scratch free 41.00 GiB");
}

TEST_CASE("a node's slot gauge goes whole when the width cannot hold it, and the limit stays",
          "[cli][dashboard][panel][node]")
{
    // A gauge cut short would be a different share, so it is all or nothing: at a width with no room for it the line
    // under the slots names the limit alone.
    auto const frames =
        NodeFramesAt({ NodeSampleOf(1, 1, MockupNodeStatus()), NodeSampleOf(2, 3, MockupNodeStatus()), Tick }, 59, 40);
    REQUIRE(frames.size() == 1);
    // 59 columns leave the slots line its 42 cells and the gauge line one short of its 43; 40 rows, so no line goes
    // for height.
    auto const gauge = LineUnder(frames.front(), "slots");
    REQUIRE(gauge.has_value());
    CHECK(Trimmed(Columns(Unwrap(gauge), 1, 57)) == "limited-by  external-cpu");
    // The control: at 80 the same readings draw the gauge.
    CHECK(NodeFrameAt(80, 24, MockupNodeStatus()).contains("\u2592\u2592\u2592\u2592\u2592\u2592\u2591"));
}

namespace
{

/// A node's history as the chart tests read it: @p count samples two seconds apart, the work rising and falling, one
/// sample that read nothing at @p failAt and one interval in which no counter moved, ending at @p stillAt.
/// @param count How many entries the history holds, the failure included.
/// @param failAt The entry that read nothing.
/// @param stillAt The entry whose interval moved nothing.
/// @return The events, a tick last.
[[nodiscard]] std::vector<DashboardEvent> NodeHistory(int count, int failAt, int stillAt)
{
    auto script = std::vector<DashboardEvent> {};
    auto step = std::uint64_t { 0 };
    for (auto const index: std::views::iota(0, count))
    {
        auto const seconds = 2 * (index + 1);
        if (index == failAt)
        {
            script.push_back(DashboardEvent { .kind = DashboardEventKind::SampleFailed,
                                              .at = TimePoint { std::chrono::seconds { seconds } } });
            continue;
        }
        step += index == stillAt ? 0U : static_cast<std::uint64_t>(1 + (index % 4));
        script.push_back(NodeSampleOf(step, seconds, MockupNodeStatus()));
    }
    script.push_back(Tick);
    return script;
}

/// The one frame a node panel draws for @p script at @p columns by @p rows, on @p rung.
/// @param script The events.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frame.
[[nodiscard]] std::string NodeHistoryFrame(std::vector<DashboardEvent> script, int columns, int rows)
{
    auto const frames = NodeFramesAt(std::move(script), columns, rows);
    REQUIRE(!frames.empty());
    return frames.back();
}

/// What a frame line says between its two edges, trimmed.
/// @param line A frame line.
/// @return Its content.
[[nodiscard]] std::string Inside(std::string_view line)
{
    auto const cells = CodePoints(line).size();
    return cells < 2 ? std::string {} : Trimmed(Columns(line, 1, cells - 2));
}

/// Where a frame's history chart is: its title line, and every band's first line in order.
struct ChartLines
{
    std::size_t title { 0 };           ///< The title's line.
    std::vector<std::size_t> bands {}; ///< Each band's first line.
    std::size_t axis { 0 };            ///< The axis's line.

    /// Rows of marks per band, read off the last band, which runs to the axis with no spacer under it.
    /// @return The rows.
    [[nodiscard]] std::size_t BandRows() const noexcept
    {
        return axis - bands.back();
    }

    /// Blank rows between two bands: how far apart the first two start, less a band's rows.
    /// @return The rows; zero for a chart of one band.
    [[nodiscard]] std::size_t Spacer() const noexcept
    {
        return bands.size() < 2 ? 0 : (bands[1] - bands[0]) - std::min(bands[1] - bands[0], BandRows());
    }
};

/// The history chart of @p frame, found by its title and its band labels in @p spec's `charts` order.
/// @param frame The frame.
/// @param spec The panel that drew it.
/// @return The lines, or nullopt for a frame with no chart.
[[nodiscard]] std::optional<ChartLines> ChartIn(std::string_view frame, PanelSpec const& spec)
{
    auto const lines = Lines(frame);
    auto const title =
        std::ranges::find_if(lines, [](std::string const& line) { return Inside(line).starts_with("history, last "); });
    if (title == lines.end())
        return std::nullopt;
    auto chart = ChartLines { .title = static_cast<std::size_t>(title - lines.begin()), .bands = {}, .axis = 0 };
    auto at = chart.title + 1;
    for (auto const& row: spec.charts)
    {
        while (at < lines.size() && !Inside(lines[at]).starts_with(row.label))
        {
            if (Inside(lines[at]).ends_with("now"))
                return chart;
            ++at;
        }
        if (at == lines.size())
            return chart;
        chart.bands.push_back(at++);
    }
    while (at < lines.size() && !Inside(lines[at]).ends_with("now"))
        ++at;
    chart.axis = at;
    return chart;
}

/// The history chart of a node @p frame.
/// @param frame The frame.
/// @return The lines, or nullopt for a frame with no chart.
[[nodiscard]] std::optional<ChartLines> NodeChartIn(std::string_view frame)
{
    return ChartIn(frame, NodePanel());
}

/// Requires @p chart to be a whole chart -- at least two bands and an axis under them -- before a case reads its
/// lines, so a chart missing a band fails on this assertion rather than reading past the frame.
/// @param chart The chart found.
/// @return The chart.
[[nodiscard]] ChartLines WholeChart(std::optional<ChartLines> const& chart)
{
    REQUIRE(chart.has_value());
    REQUIRE(Unwrap(chart).bands.size() >= 2);
    REQUIRE(Unwrap(chart).axis > Unwrap(chart).bands.back());
    return Unwrap(chart);
}

/// The chart cells at the right end of @p line, newest last: the @p count cells before the frame's edge and its blank.
/// @param line A frame line.
/// @param count How many.
/// @return The cells, one glyph each.
[[nodiscard]] std::vector<std::string> NewestCells(std::string_view line, std::size_t count)
{
    auto const points = CodePoints(line);
    REQUIRE(points.size() >= count + 2);
    return { points.end() - static_cast<std::ptrdiff_t>(count + 2), points.end() - 2 };
}

} // namespace

TEST_CASE("a node panel at 80x24 draws no history chart, and one with rows to spare draws it under the rates",
          "[cli][dashboard][panel][node][chart]")
{
    // #134: the lower half of a tall terminal was empty. WHAT DISTINGUISHES: §4's 80x24 frame is unchanged -- no chart
    // takes a row the mockup gives to something else -- while at 120x40 the chart is there, AFTER the rates, with a
    // band per `NodePanel().charts` row in the table's order, each the same height, an axis ending at `now` and a
    // legend saying a blank is a sample not read.
    auto const history = NodeHistory(24, 18, 20);
    CHECK_FALSE(NodeChartIn(NodeHistoryFrame(history, 80, 24)).has_value());

    auto const frame = NodeHistoryFrame(history, 120, 40);
    INFO(frame);
    auto const chart = WholeChart(NodeChartIn(frame));
    auto const lines = Lines(frame);
    REQUIRE(chart.bands.size() == NodePanel().charts.size());
    auto const refused =
        std::ranges::find_if(lines, [](std::string const& line) { return Inside(line).starts_with("refused/min"); });
    CHECK(std::cmp_less(refused - lines.begin(), chart.title));
    // Every band the same height, and one blank row apart (#134 F-b): stacked marks from two bands would run together.
    auto const height = chart.BandRows();
    CHECK(height > 1);
    CHECK(chart.Spacer() == 1);
    for (auto const index: std::views::iota(std::size_t { 1 }, chart.bands.size()))
    {
        CHECK(chart.bands[index] - chart.bands[index - 1] == height + 1);
        CHECK(Inside(lines.at(chart.bands[index] - 1)).empty());
    }
    CHECK(Inside(lines.at(chart.axis + 1)).ends_with("blank: no reading"));
    CHECK(lines.size() <= 40);
    // Every band's first row says what its top stands for, since a bar's height means nothing without it.
    for (auto const band: chart.bands)
        CHECK(lines[band].contains(" to "));
}

TEST_CASE("a node's history band draws zero as the floor mark and a sample not read as blanks",
          "[cli][dashboard][panel][node][chart]")
{
    // The chart's promise, on the node. WHAT DISTINGUISHES, in the compiles/min band's floor row: the interval where
    // no counter moved is the zero mark `▁`, the failed sample and the reading after it (no interval to take a rate
    // over) are blanks in EVERY row of the band, and the newest cell is a bar.
    constexpr auto Count = 24;
    constexpr auto FailAt = 18;
    constexpr auto StillAt = 21;
    auto const frame = NodeHistoryFrame(NodeHistory(Count, FailAt, StillAt), 120, 40);
    INFO(frame);
    auto const chart = WholeChart(NodeChartIn(frame));
    auto const lines = Lines(frame);
    auto const first = chart.bands[0];
    auto const height = chart.BandRows();
    auto const fromRight = [](int index) {
        return static_cast<std::size_t>((Count - 1) - index);
    };
    auto const floor = NewestCells(lines.at(first + height - 1), Count);
    CHECK(floor.at(floor.size() - 1 - fromRight(StillAt)) == "\u2581");
    CHECK(floor.back() != " ");
    for (auto const row: std::views::iota(first, first + height))
    {
        auto const cells = NewestCells(lines.at(row), Count);
        INFO("row " << row);
        CHECK(cells.at(cells.size() - 1 - fromRight(FailAt)) == " ");
        CHECK(cells.at(cells.size() - 1 - fromRight(FailAt + 1)) == " ");
    }
}

TEST_CASE("a taller node terminal grows taller bands, and a wider one a longer span", "[cli][dashboard][panel][node][chart]")
{
    // "Widths grow history the same way": a sample is a cell, so the span the title names is the band's cells, and a
    // wider terminal names a longer one. A taller one gives each band more rows, up to the growth's most.
    auto const history = NodeHistory(24, 18, 20);
    auto const small = NodeHistoryFrame(history, 120, 40);
    auto const large = NodeHistoryFrame(history, 200, 60);
    auto const smallChart = WholeChart(NodeChartIn(small));
    auto const largeChart = WholeChart(NodeChartIn(large));
    auto const bandHeight = [](ChartLines const& chart) {
        return chart.BandRows();
    };
    CHECK(bandHeight(largeChart) > bandHeight(smallChart));
    CHECK(bandHeight(largeChart) <= NodePanel().chartGrowth.bandCellsMost);
    auto const titleOf = [](std::string const& frame, ChartLines const& chart) {
        // The lines are held while the title is read: a reference into a temporary `Lines` dangles.
        auto const lines = Lines(frame);
        return Inside(lines.at(chart.title));
    };
    auto const smallTitle = titleOf(small, smallChart);
    auto const largeTitle = titleOf(large, largeChart);
    CHECK(smallTitle.starts_with("history, last "));
    CHECK(largeTitle.starts_with("history, last "));
    CHECK(smallTitle != largeTitle);
}

TEST_CASE("a node frame is as tall as the terminal, its source line on the last row", "[cli][dashboard][panel][node][chart]")
{
    // #134: a box that ends above the bottom of the screen reads as one that stopped drawing, and a chart's bands are
    // whole rows each, so the rows they leave over are padded above the source line rather than left under the box.
    // WHAT DISTINGUISHES: at every height the frame has exactly as many lines as the terminal, with the bottom edge
    // last and the source line right above it -- a panel that did not fill its height draws fewer.
    auto const history = NodeHistory(24, 18, 20);
    for (auto const& [columns, rows]:
         { std::pair { 80, 24 }, std::pair { 120, 40 }, std::pair { 200, 60 }, std::pair { 120, 43 } })
    {
        INFO("size " << columns << "x" << rows);
        auto lines = Lines(NodeHistoryFrame(history, columns, rows));
        if (!lines.empty() && lines.back().empty())
            lines.pop_back();
        REQUIRE(lines.size() == static_cast<std::size_t>(rows));
        CHECK(lines.back().starts_with("\xe2\x94\x94")); // the bottom-left corner
        CHECK(Inside(lines[lines.size() - 2]).starts_with("source"));
    }
}

TEST_CASE("a node panel draws no history chart until a band has a reading, however many rows it has",
          "[cli][dashboard][panel][node][chart]")
{
    // Captured at 80x24: the first frames, drawn before any reading while the status facts are still short, laid a
    // chart into the rows those facts claimed a moment later -- a chart of nothing that flashed up and vanished.
    // WHAT DISTINGUISHES: at 120x40, a frame before any sample and a frame after ONE sample (a rate needs two) draw no
    // chart; the frame after the second sample does.
    auto const frames = NodeFramesAt(
        { Tick, NodeSampleOf(1, 2, MockupNodeStatus()), Tick, NodeSampleOf(3, 4, MockupNodeStatus()), Tick }, 120, 40);
    REQUIRE(frames.size() >= 3);
    for (auto const index: std::views::iota(std::size_t { 0 }, frames.size() - 1))
    {
        INFO("frame " << index << "\n" << frames[index]);
        CHECK_FALSE(NodeChartIn(frames[index]).has_value());
    }
    INFO(frames.back());
    CHECK(NodeChartIn(frames.back()).has_value());
}

TEST_CASE("a node's history chart grows band by band in the table's order", "[cli][dashboard][panel][node][chart]")
{
    // Which figure gets rows first is the `charts` table's order. WHAT DISTINGUISHES: at the first height that draws
    // a chart it has the first band only, one row high; one row taller it has the first two.
    auto const history = NodeHistory(24, 18, 20);
    auto first = std::optional<int> {};
    for (auto const rows: std::views::iota(24, 60))
        if (NodeChartIn(NodeHistoryFrame(history, 120, rows)).has_value())
        {
            first = rows;
            break;
        }
    REQUIRE(first.has_value());
    auto const one = NodeChartIn(NodeHistoryFrame(history, 120, Unwrap(first)));
    REQUIRE(one.has_value());
    CHECK(Unwrap(one).bands.size() == 1);
    auto const two = NodeChartIn(NodeHistoryFrame(history, 120, Unwrap(first) + 1));
    REQUIRE(two.has_value());
    CHECK(Unwrap(two).bands.size() == 2);
    // Two one-row bands are packed: a spacer would cost a band the row it is drawn in.
    CHECK(Unwrap(two).bands[1] - Unwrap(two).bands[0] == 1);
}

TEST_CASE("on the Sixel rung a node's history is one image over its bands' rows, with its colour scale",
          "[cli][dashboard][panel][node][chart]")
{
    // The same layout drawn as pixels. WHAT DISTINGUISHES: two images -- the bands and the scale -- the bands' image
    // exactly as many cells high as the bands' rows and starting at the first band's row, and those rows carry no text
    // marks where the image goes.
    auto encoder = ScriptedSixelEncoder {};
    auto view = PanelView { NodePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "build-07:7070",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .sixel = &encoder,
                                           .rung = RenderRung::Sixel } };
    auto script = NodeHistory(24, 18, 20);
    script.insert(
        script.begin(),
        DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40, .cellPixels = ChartCell });
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    REQUIRE(!sink.frames.empty());
    auto const& frame = sink.frames.back();
    INFO(frame);
    auto const chart = WholeChart(NodeChartIn(frame));
    auto const& placements = sink.placements.back();
    REQUIRE(placements.size() == 2);
    auto const& image = placements.front();
    auto const rows = chart.axis - chart.bands.front();
    CHECK(image.cellsHigh == rows);
    // An image is not spaced: its bands draw their own gap, so the rows it covers are the bands' rows alone.
    CHECK(rows == chart.bands.size() * chart.BandRows());
    // A frame row is its content line plus one: the top edge is line 0 of the frame text.
    CHECK(image.row == chart.bands.front() + 1);
    auto const lines = Lines(frame);
    for (auto const row: std::views::iota(chart.bands.front(), chart.axis))
        CHECK(Trimmed(Columns(lines[row], image.column - 1, image.cellsWide)).empty());
    CHECK(placements.back().cellsHigh == 1);
}

TEST_CASE("a pixel chart with no band in the ramp places no colour scale and states none",
          "[cli][dashboard][panel][node][chart]")
{
    // A scale says which colour stands for which share, so it belongs beside a band coloured that way and nowhere
    // else. WHAT DISTINGUISHES: the node's own table, whose cpu-busy band is in the ramp, places the bands and the
    // scale (the case above); the same panel with every band plain places the bands alone, and its legend has no
    // `0 ... top` around a scale.
    static auto const plainCharts = [] {
        auto charts = std::vector<ChartRow>(NodePanel().charts.begin(), NodePanel().charts.end());
        for (auto& row: charts)
            row.paint = ChartPaint::Plain;
        return charts;
    }();
    static auto const plainPanel = [] {
        auto spec = NodePanel();
        spec.charts = plainCharts;
        return spec;
    }();
    REQUIRE(std::ranges::any_of(NodePanel().charts, [](ChartRow const& row) { return row.paint == ChartPaint::Ramp; }));

    auto encoder = ScriptedSixelEncoder {};
    auto view = PanelView { plainPanel,
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "build-07:7070",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .sixel = &encoder,
                                           .rung = RenderRung::Sixel } };
    auto script = NodeHistory(24, 18, 20);
    script.insert(
        script.begin(),
        DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40, .cellPixels = ChartCell });
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    REQUIRE(!sink.frames.empty());
    auto const& frame = sink.frames.back();
    INFO(frame);
    auto const chart = WholeChart(ChartIn(frame, plainPanel));
    auto const& placements = sink.placements.back();
    REQUIRE(placements.size() == 1);
    CHECK(placements.front().cellsHigh == chart.axis - chart.bands.front());
    auto const legend = Inside(Lines(frame).at(chart.axis + 1));
    CHECK(legend.starts_with("bar: share of its band's top"));
    CHECK_FALSE(legend.contains(" top  bar"));
}

TEST_CASE("on the ASCII rung a node's history bands are ASCII marks", "[cli][dashboard][panel][node][chart]")
{
    // A multi-row chart carries its heights on a rung with no sparkline, so the ASCII rung draws one, byte for byte
    // in ASCII -- a stray UTF-8 mark is mojibake on exactly the terminal the rung serves.
    auto view = PanelView { NodePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "build-07:7070",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Ascii } };
    auto script = NodeHistory(24, 18, 20);
    script.insert(script.begin(), DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40 });
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    REQUIRE(!sink.frames.empty());
    auto const& frame = sink.frames.back();
    auto const chart = WholeChart(NodeChartIn(frame));
    auto const lines = Lines(frame);
    auto marks = std::size_t { 0 };
    for (auto const row: std::views::iota(chart.bands.front(), chart.axis))
    {
        CHECK(std::ranges::all_of(lines[row], [](char byte) { return static_cast<unsigned char>(byte) < 0x80U; }));
        marks += static_cast<std::size_t>(std::ranges::count(lines[row], '#'));
    }
    CHECK(marks > 0);
}

namespace
{

/// A cache's history as the chart tests read it, both tiers as §3 draws them: @p count samples a second apart, one that read
/// nothing at @p failAt, and one interval in which no operation happened, ending at @p stillAt.
/// @param count How many entries the history holds, the failure included.
/// @param failAt The entry that read nothing.
/// @param stillAt The entry whose interval moved nothing.
/// @return The events, a tick last.
[[nodiscard]] std::vector<DashboardEvent> CacheHistory(int count, int failAt, int stillAt)
{
    auto script = std::vector<DashboardEvent> {};
    auto step = std::uint64_t { 0 };
    for (auto const index: std::views::iota(0, count))
    {
        auto const seconds = index + 1;
        if (index == failAt)
        {
            script.push_back(DashboardEvent { .kind = DashboardEventKind::SampleFailed,
                                              .at = TimePoint { std::chrono::seconds { seconds } } });
            continue;
        }
        step += index == stillAt ? 0U : static_cast<std::uint64_t>(1 + (index % 3));
        script.push_back(SampleOf(CacheReading(step, { "memory", "disk" }), seconds));
    }
    script.push_back(Tick);
    return script;
}

/// The last frame a cache panel draws for @p script at @p columns by @p rows on @p rung.
/// @param script The events.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @param rung The rung.
/// @return The frame.
[[nodiscard]] std::string CacheHistoryFrame(std::vector<DashboardEvent> script,
                                            int columns,
                                            int rows,
                                            RenderRung rung = RenderRung::Unicode)
{
    auto const frames = FramesAt(CachePanel(), std::move(script), rung, columns, rows);
    REQUIRE(!frames.empty());
    return frames.back();
}

/// The line index in @p lines whose content starts with @p prefix, or `lines.size()`.
/// @param lines A frame's lines.
/// @param prefix The start.
/// @return The index.
[[nodiscard]] std::size_t IndexStarting(std::vector<std::string> const& lines, std::string_view prefix)
{
    auto const found =
        std::ranges::find_if(lines, [prefix](std::string const& line) { return Inside(line).starts_with(prefix); });
    return static_cast<std::size_t>(std::ranges::distance(lines.begin(), found));
}

} // namespace

TEST_CASE("a cache panel at 80x24 draws no history chart, and one with rows to spare draws it under the rates",
          "[cli][dashboard][panel][cache][chart]")
{
    // #134 C9: the cache panel's lower half was empty at 120x40. WHAT DISTINGUISHES: §3's 80x24 frame has no chart,
    // while at 120x40 the chart is there between the last rate row and the levels, with a band per `CachePanel().charts`
    // row in the table's order, each the same height, and the frame as tall as the terminal.
    auto const history = CacheHistory(24, 18, 20);
    CHECK_FALSE(ChartIn(CacheHistoryFrame(history, 80, 24), CachePanel()).has_value());

    auto const frame = CacheHistoryFrame(history, 120, 40);
    INFO(frame);
    auto const chart = WholeChart(ChartIn(frame, CachePanel()));
    REQUIRE(chart.bands.size() == CachePanel().charts.size());
    auto lines = Lines(frame);
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    CHECK(lines.size() == 40);
    CHECK(IndexStarting(lines, "expired/s") < chart.title);
    CHECK(chart.axis < IndexStarting(lines, "items"));
    auto const height = chart.BandRows();
    for (auto const index: std::views::iota(std::size_t { 1 }, chart.bands.size()))
        CHECK(chart.bands[index] - chart.bands[index - 1] == height + chart.Spacer());

    auto const tall = WholeChart(ChartIn(CacheHistoryFrame(history, 200, 60), CachePanel()));
    CHECK(tall.BandRows() > height);
}

TEST_CASE("a cache's history tells an idle interval from one with no reading: zero operations, and no hit rate",
          "[cli][dashboard][panel][cache][chart]")
{
    // The cache's own case of absent-is-not-zero. WHAT DISTINGUISHES, in the interval where nothing was asked: the
    // `ops/sec` band draws the zero mark on its floor, while the `hit rate` band -- hits over gets, with no gets --
    // is blank in EVERY row. A failed sample is blank in both.
    constexpr auto Count = 24;
    constexpr auto FailAt = 17;
    constexpr auto StillAt = 21;
    auto const frame = CacheHistoryFrame(CacheHistory(Count, FailAt, StillAt), 120, 40);
    INFO(frame);
    auto const chart = WholeChart(ChartIn(frame, CachePanel()));
    auto const lines = Lines(frame);
    auto const height = chart.BandRows();
    auto const cellAt = [&lines](std::size_t row, int index) {
        auto const cells = NewestCells(lines.at(row), Count);
        return cells.at(static_cast<std::size_t>(index));
    };
    auto const hitRate = chart.bands[0];
    auto const ops = chart.bands[1];
    CHECK(cellAt(ops + height - 1, StillAt) == "\u2581");
    CHECK(cellAt(ops + height - 1, Count - 1) != " ");
    for (auto const row: std::views::iota(std::size_t { 0 }, height))
    {
        INFO("row " << row);
        CHECK(cellAt(hitRate + row, StillAt) == " ");
        CHECK(cellAt(hitRate + row, FailAt) == " ");
        CHECK(cellAt(ops + row, FailAt) == " ");
    }
    // A reading beside it is drawn: the hit rate before the idle interval is a bar.
    CHECK(cellAt(hitRate + height - 1, StillAt - 1) != " ");
}

TEST_CASE("a cache's fill band is drawn against the limit, not against its own peak",
          "[cli][dashboard][panel][cache][chart]")
{
    // A band whose figure has a whole of its own says so: three GiB of a four GiB limit is 75 % of a band whose top
    // is 100 %. Scaled to its own peak the band would draw a full bar and say `to 75.0 %`, a cache at its limit.
    auto const frame = CacheHistoryFrame(CacheHistory(24, 18, 20), 120, 40);
    INFO(frame);
    auto const chart = WholeChart(ChartIn(frame, CachePanel()));
    auto const lines = Lines(frame);
    auto const bytes = Inside(lines.at(chart.bands[2]));
    CHECK(bytes.starts_with("fill"));
    CHECK(bytes.contains("75.0 %"));
    CHECK(bytes.contains("to 100.0 %"));
}

TEST_CASE("a cache's hit rate band is drawn against the whole, not against its own peak",
          "[cli][dashboard][panel][cache][chart]")
{
    // The hit rate is a share, so its band's top is 100 % whatever the readings were. WHAT DISTINGUISHES: every reading
    // here is 90 %, so a band scaled to its own peak would say `to 90.0 %` and draw every cell a full bar -- which is
    // what a cache serving every read looks like -- while the ops/sec band, which has no whole, states its own peak.
    auto const frame = CacheHistoryFrame(CacheHistory(24, 18, 20), 120, 40);
    INFO(frame);
    auto const chart = WholeChart(ChartIn(frame, CachePanel()));
    auto const lines = Lines(frame);
    auto const hitRate = Inside(lines.at(chart.bands[0]));
    CHECK(hitRate.starts_with("hit rate"));
    CHECK(hitRate.contains("90.0 %"));
    CHECK(hitRate.contains("to 100.0 %"));
    auto const ops = Inside(lines.at(chart.bands[1]));
    CHECK(ops.starts_with("ops/sec"));
    CHECK_FALSE(ops.contains("to 100.0 %"));
    CHECK(ops.contains(" to "));
}

TEST_CASE("a cache's history chart grows band by band in the table's order", "[cli][dashboard][panel][cache][chart]")
{
    // WHAT DISTINGUISHES: at the first height that draws a chart it has the hit rate alone; one row taller it adds
    // the operations. The table, not the rate rows' order or priority, says which band comes next.
    auto const history = CacheHistory(24, 18, 20);
    auto first = std::optional<int> {};
    for (auto const rows: std::views::iota(24, 60))
        if (ChartIn(CacheHistoryFrame(history, 120, rows), CachePanel()).has_value())
        {
            first = rows;
            break;
        }
    REQUIRE(first.has_value());
    auto const one = ChartIn(CacheHistoryFrame(history, 120, Unwrap(first)), CachePanel());
    REQUIRE(one.has_value());
    CHECK(Unwrap(one).bands.size() == 1);
    auto const two = ChartIn(CacheHistoryFrame(history, 120, Unwrap(first) + 1), CachePanel());
    REQUIRE(two.has_value());
    CHECK(Unwrap(two).bands.size() == 2);
}

TEST_CASE("on the Sixel rung a cache's history is one image over its bands' rows, with its colour scale",
          "[cli][dashboard][panel][cache][chart]")
{
    // WHAT DISTINGUISHES: two images, the bands' exactly as many cells high as the bands' rows and starting at the
    // first band's row, over rows the text left blank where the image goes.
    auto encoder = ScriptedSixelEncoder {};
    auto view = PanelView { CachePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .cellWidth = &FakeCellWidth,
                                           .sixel = &encoder,
                                           .rung = RenderRung::Sixel } };
    auto script = CacheHistory(24, 18, 20);
    script.insert(
        script.begin(),
        DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40, .cellPixels = ChartCell });
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink);
    REQUIRE(!sink.frames.empty());
    auto const& frame = sink.frames.back();
    INFO(frame);
    auto const chart = WholeChart(ChartIn(frame, CachePanel()));
    auto const& placements = sink.placements.back();
    REQUIRE(placements.size() == 2);
    auto const& image = placements.front();
    CHECK(image.cellsHigh == chart.axis - chart.bands.front());
    CHECK(image.cellsHigh == chart.bands.size() * chart.BandRows());
    CHECK(image.row == chart.bands.front() + 1);
    auto const lines = Lines(frame);
    for (auto const row: std::views::iota(chart.bands.front(), chart.axis))
        CHECK(Trimmed(Columns(lines[row], image.column - 1, image.cellsWide)).empty());
    CHECK(placements.back().cellsHigh == 1);
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

TEST_CASE("a mean compile note says it is a change over the interval, in ASCII on the ASCII rung",
          "[cli][dashboard][panel][node]")
{
    // §4 writes `Δsum/Δcount`: the figure is a delta over the interval, so the delta is content. WHAT DISTINGUISHES:
    // the Unicode rung draws the delta mark, and the ASCII rung draws the same note with `d` in its place -- one cell
    // either way -- and not one byte of that line is outside ASCII.
    auto const unicode = NodeFrameAt(120, 40, MockupNodeStatus());
    auto const unicodeRow = RowLine(unicode, "mean compile");
    REQUIRE(unicodeRow.has_value());
    CHECK(Unwrap(unicodeRow).contains("Δsum/Δcount over this interval"));

    auto view = PanelView { NodePanel(),
                            PanelContext { .absent = std::string { Absent },
                                           .endpoint = "build-07:7070",
                                           .interval = 2s,
                                           .cellWidth = &FakeCellWidth,
                                           .rung = RenderRung::Ascii } };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 120, .rows = 40 },
                   NodeSampleOf(1, 1, MockupNodeStatus()),
                   NodeSampleOf(2, 3, MockupNodeStatus()),
                   Tick },
                 DashboardLimits {},
                 view,
                 sink);
    REQUIRE(!sink.frames.empty());
    auto const asciiRow = RowLine(sink.frames.back(), "mean compile");
    REQUIRE(asciiRow.has_value());
    CHECK(Unwrap(asciiRow).contains("dsum/dcount over this interval"));
    CHECK(std::ranges::all_of(Unwrap(asciiRow), [](char byte) { return static_cast<unsigned char>(byte) < 0x80U; }));
    CHECK(ColumnOf(Unwrap(asciiRow), "dsum") == ColumnOf(Unwrap(unicodeRow), "Δsum"));
}

TEST_CASE("a mean compile note too long for its line wraps under where it began", "[cli][dashboard][panel][node]")
{
    // N7. At 80 the note is two lines, the second hanging at the column the first began at, and the words
    // are the note's in order; at 120 it is one line. A note dropped for width would pass neither.
    auto const note = std::string { "Δsum/Δcount over this interval; no histogram exists, so no p50/p95 can be shown" };
    auto const narrow = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const row = RowLine(narrow, "mean compile");
    auto const under = LineUnder(narrow, "mean compile");
    REQUIRE(row.has_value());
    REQUIRE(under.has_value());
    CHECK_FALSE(Unwrap(row).contains(note));
    REQUIRE(ColumnOf(Unwrap(row), "Δsum/Δcount").has_value());
    CHECK(Trimmed(Columns(Unwrap(under), 1, Unwrap(ColumnOf(Unwrap(row), "Δsum/Δcount")) - 1)).empty());
    auto const column = Unwrap(ColumnOf(Unwrap(row), "Δsum/Δcount"));
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
    // for a tier that does not exist -- with its fill off the reading's cache (2 of 8 GiB, a quarter of the gauge);
    // and the host line is one line of the machine's figures: the CPU share between the two readings, and what
    // memory and scratch have left.
    auto const frame = NodeFrameAt(80, 24, MockupNodeStatus());
    auto const tier = LineStarting(frame, "cache tier");
    REQUIRE(tier.has_value());
    CHECK(Trimmed(Columns(Unwrap(tier), 1, 78))
          == "cache tier  hits 88.1 %   2.00 GiB / 8.00 GiB  "
             "\u2588\u2588\u2588\u2588\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591  25.0 %");
    auto const host = LineStarting(frame, "host");
    REQUIRE(host.has_value());
    CHECK(Trimmed(Columns(Unwrap(host), 1, 78))
          == "host        cpu-busy 62.5 %   mem free 32.00 GiB   scratch free 41.00 GiB");
    for (auto const* gone: { "cores", "memory", "slots busy", "scratch free" })
        CHECK_FALSE(LineStarting(frame, gone).has_value());

    auto plain = MockupNodeStatus();
    plain.components = CompileCacheWire::NodeComponentBit::Worker;
    plain.runtime.schedulerRole.reset();
    plain.runtime.leaderEndpoint.clear();
    plain.nodeId.clear();
    auto const worker = NodeFrameAt(80, 24, plain);

    // A leader names no leader: it is the one, and says so where a follower names the address.
    auto leading = MockupNodeStatus();
    leading.runtime.schedulerRole = CompileCacheWire::WireSchedulerRole::Leader;
    leading.runtime.leaderEndpoint.clear();
    CHECK(ContentStarting(NodeFrameAt(80, 24, leading), "consensus").ends_with("leader      this node"));
    auto electing = MockupNodeStatus();
    electing.runtime.schedulerRole = CompileCacheWire::WireSchedulerRole::Undecided;
    electing.runtime.leaderEndpoint.clear();
    CHECK(ContentStarting(NodeFrameAt(80, 24, electing), "consensus").ends_with(std::format("leader      {}", Absent)));
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
    // The slots are the reading's own, since it carries the machine; one reading names no limit yet.
    CHECK(ContentStarting(frame, "slots") == std::format("slots       6 in flight / {} available / 16 registered", Absent));
    CHECK_FALSE(LineStarting(frame, "consensus").has_value());
    CHECK_FALSE(LineStarting(frame, "cache tier").has_value());

    // And a node no scheduler has ever accepted says NEVER, not an age.
    auto unaccepted = MockupNodeStatus();
    unaccepted.runtime.registrarsRegistered = 0;
    unaccepted.runtime.lastRegistrationSecondsAgo.reset();
    CHECK(ContentStarting(NodeFrameAt(80, 24, unaccepted), "toolchains").contains("registrars  0 of 1, never accepted"));
}

namespace
{

/// The tone of the one run of the newest frame in @p sink whose text is exactly @p text.
/// @param sink What was presented.
/// @param text The run's text.
/// @return Its tone; nullopt when no run covers exactly that text.
[[nodiscard]] std::optional<FrameTone> ToneOver(Presented const& sink, std::string_view text)
{
    REQUIRE(!sink.frames.empty());
    auto const lines = Lines(sink.frames.back());
    for (auto const& span: sink.spans.back())
        if (span.row >= 1 && span.row <= lines.size() && lines[span.row - 1].substr(span.byte, span.length) == text)
            return span.tone;
    return std::nullopt;
}

/// A figure as a frame shows it, split where its number ends: `90.0 %` is `90.0` and `%`, `0.5/s` is `0.5` and `/s`,
/// and `1 311` is all number. Read off the frame's text rather than asked of the writer, so a panel that wrote the
/// parts into one run is caught.
/// @param figure The figure's text.
/// @return The number, and the unit without the blank before it.
[[nodiscard]] std::pair<std::string, std::string> NumberAndUnit(std::string_view figure)
{
    auto const last = figure.find_last_of("0123456789");
    REQUIRE(last != std::string_view::npos);
    return { std::string { figure.substr(0, last + 1) }, Trimmed(figure.substr(last + 1)) };
}

/// §4's node presented at 80 by 24, two readings in, saying @p status about itself.
/// @param status What the node says.
/// @return What it presented.
[[nodiscard]] Presented NodeSinkOf(CompileCacheWire::NodeStatusFields const& status)
{
    auto sink = NodeSinkAt({ NodeSampleOf(1, 1, status), NodeSampleOf(2, 3, status), Tick }, 80, 24);
    REQUIRE(sink.frames.size() == 1);
    return sink;
}

} // namespace

TEST_CASE("a node panel dresses labels as labels, figures as figures, and a refusal above zero as an alert",
          "[cli][dashboard][panel][node][tone]")
{
    // #134 G1 on §4's panel. WHAT DISTINGUISHES: the label and the figure of one row are two runs of two tones,
    // not one run over both; the refusal total and each refusal that moved are ALERTS while the one that did
    // not move is an ordinary figure; each refusal's name is a label run apart from its figure, inside a piece
    // that still goes for width whole; and a wrapped caveat is a Label run on each of its lines.
    auto const sink = NodeSinkOf(MockupNodeStatus());
    CHECK(ToneOver(sink, "compiles/min") == FrameTone::Label);
    CHECK(ToneOver(sink, "1 230") == FrameTone::Figure);
    CHECK(ToneOver(sink, "refused/min") == FrameTone::Label);
    CHECK(ToneOver(sink, "90") == FrameTone::Alert);
    CHECK(ToneOver(sink, "no-slot") == FrameTone::Label);
    CHECK(ToneOver(sink, "60") == FrameTone::Alert);
    CHECK(ToneOver(sink, "/min") == FrameTone::Label);
    CHECK(ToneOver(sink, "lease-expired") == FrameTone::Label);
    CHECK(ToneOver(sink, "30") == FrameTone::Alert);
    CHECK(ToneOver(sink, "unknown-fingerprint") == FrameTone::Label);
    CHECK(ToneOver(sink, "0.0") == FrameTone::Figure);
    CHECK(ToneOver(sink, "completed") == FrameTone::Label);
    // Where the caveat wraps is the layout's business (`a mean compile note too long...`), so each line's words are
    // read off the frame: from where the note begins to the edge, and the whole hanging line under it.
    auto const& frame = sink.frames.front();
    auto const row = RowLine(frame, "mean compile");
    auto const under = LineUnder(frame, "mean compile");
    REQUIRE(row.has_value());
    REQUIRE(under.has_value());
    auto const noteFrom = ColumnOf(Unwrap(row), "Δsum/Δcount");
    REQUIRE(noteFrom.has_value());
    CHECK(ToneOver(sink, Trimmed(Columns(Unwrap(row), Unwrap(noteFrom), 79 - Unwrap(noteFrom)))) == FrameTone::Label);
    CHECK(ToneOver(sink, Trimmed(Columns(Unwrap(under), 1, 78))) == FrameTone::Label);
    for (auto const* label: { "node-id", "components", "toolchains", "registrars", "consensus", "leader", "slots", "host" })
    {
        INFO("label " << label);
        CHECK(ToneOver(sink, label) == FrameTone::Label);
    }
    // The slot gauge's running part is dressed by the band of what is available that runs (6 of 12, the middle);
    // the part available and the part withdrawn are not; somebody else's CPU is worth a look, and its remedy is a label.
    CHECK(ToneOver(sink, "\u2588\u2588\u2588\u2588\u2588\u2588") == FrameTone::LevelMid);
    CHECK_FALSE(ToneOver(sink, "\u2592\u2592\u2592\u2592\u2592\u2592").has_value());
    CHECK(ToneOver(sink, "limited-by") == FrameTone::Label);
    CHECK(ToneOver(sink, "external-cpu") == FrameTone::Stale);
    CHECK(ToneOver(sink, "somebody else is using this machine. Its own work is not the") == FrameTone::Label);
    // A fact's figures are figures like a rate's: the words around each a label, its number a figure, its unit a
    // label -- read off the frame's host and cache tier lines, each cell of which is words, a number and a unit.
    // The slot line's separators are written as the words after each figure (`SlotFigures`), so they recede with them.
    for (auto const* word: { "in flight /", "available /", "registered" })
    {
        INFO("slot word " << word);
        CHECK(ToneOver(sink, word) == FrameTone::Label);
    }
    auto cells = std::vector<std::string> {};
    for (auto const* line: { "host", "cache tier" })
    {
        auto content = ContentStarting(frame, line);
        REQUIRE_FALSE(content.empty());
        content.erase(0, std::string_view { line }.size());
        for (auto const cell: std::views::split(std::string_view { content }, std::string_view { "   " }))
            if (auto const text = Trimmed(std::string_view { cell.begin(), cell.end() }); !text.empty())
                cells.push_back(text);
    }
    auto checked = 0;
    for (auto const& cell: cells)
    {
        auto const first = cell.find_first_of("0123456789");
        auto const last = cell.find_last_of("0123456789");
        auto const unit = last == std::string::npos ? std::string {} : Trimmed(std::string_view { cell }.substr(last + 1));
        // Only a cell that is words, one number and a unit: the tier's `used / limit` and its gauge are other shapes.
        if (first == 0 || first == std::string::npos || unit.empty() || unit.contains(' ') || unit.contains('/'))
            continue;
        INFO("fact cell " << cell);
        CHECK(ToneOver(sink, Trimmed(std::string_view { cell }.substr(0, first))) == FrameTone::Label);
        CHECK(ToneOver(sink, cell.substr(first, last + 1 - first)) == FrameTone::Figure);
        CHECK(ToneOver(sink, unit) == FrameTone::Label);
        ++checked;
    }
    // `cpu-busy 62.5 %`, `mem free .. GiB`, `scratch free .. GiB` and `hits 88.1 %`: a split that found none checks nothing.
    CHECK(checked == 4);
    // The ordinary states are not dressed: a run over them would be a claim that something needs looking at.
    CHECK_FALSE(ToneOver(sink, "serving 3 of 3").has_value());
    CHECK_FALSE(ToneOver(sink, "follower").has_value());
    CHECK_FALSE(ToneOver(sink, "1 of 1, last 4s ago").has_value());
}

TEST_CASE("a node's states worth a look are dressed: a survey not done, an election, registrations not held",
          "[cli][dashboard][panel][node][tone]")
{
    // The other direction of the case above, one state at a time, so a tone that dressed EVERY state as one colour
    // is caught by the states that must differ.
    auto surveying = MockupNodeStatus();
    surveying.runtime.toolchains = CompileCacheWire::ToolchainState::Surveying;
    surveying.runtime.toolchainsServed = 1;
    CHECK(ToneOver(NodeSinkOf(surveying), "surveying 1 of 3") == FrameTone::Stale);

    auto idle = MockupNodeStatus();
    idle.runtime.toolchains = CompileCacheWire::ToolchainState::NothingToServe;
    idle.runtime.toolchainsServed = 0;
    idle.runtime.toolchainsDiscovered = 0;
    CHECK(ToneOver(NodeSinkOf(idle), "nothing-to-serve 0 of 0") == FrameTone::Alert);

    auto electing = MockupNodeStatus();
    electing.runtime.schedulerRole = CompileCacheWire::WireSchedulerRole::Undecided;
    CHECK(ToneOver(NodeSinkOf(electing), "undecided") == FrameTone::Stale);

    auto partial = MockupNodeStatus();
    partial.runtime.registrarsTotal = 2;
    CHECK(ToneOver(NodeSinkOf(partial), "1 of 2, last 4s ago") == FrameTone::Stale);

    auto never = MockupNodeStatus();
    never.runtime.registrarsRegistered = 0;
    never.runtime.lastRegistrationSecondsAgo.reset();
    CHECK(ToneOver(NodeSinkOf(never), "0 of 1, never accepted") == FrameTone::Alert);

    // A limit is dressed by the leader's one judgement (`SlotLimitTone`), the one the fleet's workers cell and the
    // page's chip take: memory is limited -- worth a look, as somebody else's CPU is -- where a full scratch disk is
    // an alert and nothing withdrawn is fresh. WHAT DISTINGUISHES: the node's `memory` is exactly the fleet's tone for
    // it, and a node that has its own table dresses it however that table says.
    auto const tight = NodeMachine { .cpuPermille = 625, .availableMemoryBytes = std::uint64_t { 1 } << 30U };
    auto const memory = NodeSinkAt(
        { NodeSampleOf(1, 1, MockupNodeStatus(), tight), NodeSampleOf(2, 3, MockupNodeStatus(), tight), Tick }, 80, 24);
    REQUIRE(memory.frames.size() == 1);
    CHECK(Distributed::SlotLimitTone(Distributed::SlotLimit::Memory) == Distributed::CellTone::Limited);
    CHECK(ToneOver(memory, "memory") == FrameTone::Stale);
    auto const unloaded = NodeMachine { .cpuPermille = 0 };
    auto const free = NodeSinkAt(
        { NodeSampleOf(1, 1, MockupNodeStatus(), unloaded), NodeSampleOf(2, 3, MockupNodeStatus(), unloaded), Tick },
        80,
        24);
    REQUIRE(free.frames.size() == 1);
    // `registered` is also the slot line's last word, a label: the limit is the run on the gauge's line.
    auto const freeLines = Lines(free.frames.back());
    auto const gaugeRow =
        std::ranges::find_if(freeLines, [](std::string const& line) { return line.contains("limited-by"); });
    REQUIRE(gaugeRow != freeLines.end());
    auto const row = static_cast<std::size_t>(std::ranges::distance(freeLines.begin(), gaugeRow)) + 1;
    auto const* const limit = FindIfOrNull(free.spans.back(), [&](FrameSpan const& span) {
        return span.row == row && freeLines[row - 1].substr(span.byte, span.length) == "registered";
    });
    REQUIRE(limit != nullptr);
    CHECK(limit->tone == FrameTone::Fresh);
    // 6 running of the 7 memory leaves is the top band.
    CHECK(ToneOver(memory, "\u2588\u2588\u2588\u2588\u2588\u2588") == FrameTone::LevelHigh);
}

TEST_CASE("a cache rate block dresses its labels and beside words as labels and its figures as figures",
          "[cli][dashboard][panel][tone]")
{
    // G1 over §3's rates, two readings in so every rate is a figure. WHAT DISTINGUISHES: a rate's label and figure
    // are two runs; `since start` AFTER a figure is a label run as `evicted unfetched` BEFORE one is; the figure
    // between them is a figure run of its own, where one tone over the whole beside piece dresses words and value
    // alike; and a figure's unit (`%`, `/s`) is a label run apart from its number.
    auto sink = CollectingSink {};
    auto view = PanelView {
        CachePanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 80, .rows = 24 },
                   SampleOf(CacheReading(1, { "memory", "disk" }), 1),
                   SampleOf(CacheReading(2, { "memory", "disk" }), 3),
                   Tick },
                 DashboardLimits {},
                 view,
                 sink);
    auto const presented = Presented { .frames = sink.frames, .spans = sink.spans };
    REQUIRE(presented.frames.size() == 1);
    auto const& frame = presented.frames.front();
    INFO(frame);
    for (auto const& rate: CachePanel().rates)
    {
        INFO("rate " << rate.label);
        auto const line = RowLine(frame, rate.label);
        REQUIRE(line.has_value());
        CHECK(ToneOver(presented, rate.label) == FrameTone::Label);
        auto const [number, unit] = NumberAndUnit(FigureOf(Unwrap(line)));
        CHECK(ToneOver(presented, number) == FrameTone::Figure);
        CHECK((unit.empty() || ToneOver(presented, unit) == FrameTone::Label));
    }
    for (auto const* word: { "since start", "get", "set", "accepted", "evicted unfetched", "expired unfetched" })
    {
        INFO("word " << word);
        CHECK(ToneOver(presented, word) == FrameTone::Label);
    }
    // The beside figure is the line's last text, so it runs from after its words to the padding before the edge.
    auto const expired = RowLine(frame, "expired/s");
    REQUIRE(expired.has_value());
    auto const words = ColumnOf(Unwrap(expired), "expired unfetched ");
    REQUIRE(words.has_value());
    auto const figureFrom = Unwrap(words) + FakeCellWidth("expired unfetched ");
    auto const [number, unit] = NumberAndUnit(Trimmed(Columns(Unwrap(expired), figureFrom, 79 - figureFrom)));
    CHECK(ToneOver(presented, number) == FrameTone::Figure);
    CHECK(unit == "/s");
    CHECK(ToneOver(presented, unit) == FrameTone::Label);
}

TEST_CASE("a heartbeat past its threshold is dressed stale and one inside it fresh, on exactly its cell",
          "[cli][dashboard][panel][fleet][parity]")
{
    // #134 F9, through the leader's one threshold. WHAT DISTINGUISHES: the stale tone lies on exactly the
    // second machine's heartbeat cell and the fresh one on the first's -- a span one cell over dresses a
    // figure the threshold never judged -- and the three machines' ages are the only toned cells. Whether a
    // tone shows as colour is the presenter's decision from the capability record, not the panel's.
    auto const lit = RunFleet({ FleetSampleOf(1, LeaderFleetText(3)), Tick }, 240, 40, UnicodeContext());
    REQUIRE(lit.frames.size() == 1);
    auto const& frame = lit.frames.front();

    auto const toneOn = [&](std::string_view rowStart) -> std::optional<FrameTone> {
        auto const row = TableLineIndexHolding(frame, rowStart) + 1;
        auto const cell = CellUnder(frame, "heartbeat-age", rowStart);
        for (auto const& span: lit.spans.front())
            if (span.row == row && cell.has_value() && SpanText(frame, span) == Unwrap(cell))
                return span.tone;
        return std::nullopt;
    };
    CHECK(toneOn("build-01") == std::optional<FrameTone> { FrameTone::Fresh });
    CHECK(toneOn("build-02") == std::optional<FrameTone> { FrameTone::Stale });
    CHECK(std::ranges::count_if(
              lit.spans.front(),
              [](FrameSpan const& span) { return span.tone == FrameTone::Fresh || span.tone == FrameTone::Stale; })
          == 3);
}

TEST_CASE("a count of one is singular: one machine in the title, one sample, one of one machine in the filter",
          "[cli][dashboard][panel][fleet][parity]")
{
    // The owner's demo fleet is one machine, so `1 machines` was the first title he read (gallery D1). WHAT
    // DISTINGUISHES, each spelling asked for in both numbers so neither passes by never being drawn: the title says
    // `1 machine` over one machine and `12 machines` over twelve; the source line says `1 sample` after one sample
    // and `2 samples` after two; a kept filter says `1 of 1 machine` and `12 of 12 machines`.
    auto const filtered = [](std::size_t machines) {
        auto const run = RunFleet({ FleetSampleOf(1, LeaderFleetText(machines)),
                                    Tick,
                                    FleetSampleOf(2, LeaderFleetText(machines)),
                                    Tick,
                                    KeyOf("/"),
                                    KeyOf("b"),
                                    KeyOf(std::string { '\r' }) },
                                  120,
                                  40,
                                  UnicodeContext());
        REQUIRE(run.frames.size() == 5);
        return run.frames;
    };
    auto const one = filtered(1);
    INFO(one.front());
    CHECK(Lines(one.front()).front().contains("  1 machine  "));
    CHECK(one.front().contains("1 sample, 0 gaps"));
    CHECK(one[1].contains("2 samples, 0 gaps"));
    CHECK(one.back().contains("filter  /b  1 of 1 machine; / edits"));
    for (auto const& frame: one)
        CHECK_FALSE(frame.contains("1 machines"));

    auto const twelve = filtered(FleetMachines);
    CHECK(Lines(twelve.front()).front().contains(std::format("  {} machines  ", FleetMachines)));
    CHECK(twelve.back().contains(std::format("filter  /b  {0} of {0} machines; / edits", FleetMachines)));
}

TEST_CASE("PgDn and PgUp scroll the fleet table by the rows it shows, and the overflow line says so",
          "[cli][dashboard][panel][fleet][parity]")
{
    // #134 F10: `... N more machines; PgDn scrolls, / filters` is a promise, so it is kept. WHAT
    // DISTINGUISHES: after PgDn the first row drawn is the one after the last row shown before, and the
    // overflow line counts what is above as well as what is below; PgUp comes back to the first machine.
    constexpr auto Machines = std::size_t { 40 };
    auto const run = RunFleet(
        { FleetSampleOf(1, LeaderFleetText(Machines)), Tick, KeyOf("\x1b[6~"), KeyOf("\x1b[5~") }, 80, 24, UnicodeContext());
    REQUIRE(run.frames.size() == 3);
    auto const shown = LinesStarting(run.frames[0], "build-");
    REQUIRE(shown > 0);
    CHECK(FirstMachine(run.frames[0]) == 1);
    CHECK(run.frames[0].contains(std::format("... {} more machines; PgDn scrolls, / filters", Machines - shown)));

    CHECK(FirstMachine(run.frames[1]) == static_cast<int>(shown) + 1);
    CHECK(run.frames[1].contains(std::format(", {} above; PgUp/PgDn scroll, / filters", shown)));

    CHECK(FirstMachine(run.frames[2]) == 1);
    CHECK(run.frames[2] == run.frames[0]);
}

TEST_CASE("a typed filter keeps the fleet rows that contain it, holds a q, and Esc clears it",
          "[cli][dashboard][panel][fleet][parity]")
{
    // #134 F10's `/ filters`. WHAT DISTINGUISHES: while the filter is typed a `q` is a character of it --
    // the session does not end, and the filter reads `build-2q` and matches nothing -- Backspace takes it
    // back to the ten machines `build-2` names, case ignored, Enter keeps it with a count, and `/` then
    // Esc brings every machine back.
    constexpr auto Machines = std::size_t { 40 };
    auto script = std::vector<DashboardEvent> { FleetSampleOf(1, LeaderFleetText(Machines)), Tick, KeyOf("/") };
    for (auto const* const typed: { "B", "u", "i", "l", "d", "-", "2" })
        script.push_back(KeyOf(typed));
    for (auto const* const key: { "q", "\x7f", "\r", "/", "\x1b" })
        script.push_back(KeyOf(key));
    auto const run = RunFleet(std::move(script), 132, 60, UnicodeContext());

    CHECK(run.stop == DashboardStop::SourceDetached);
    REQUIRE(run.frames.size() == 14);
    CHECK(run.frames[1].contains("filter  /_  Enter keeps, Esc clears"));
    auto const& typed = run.frames[8];
    CHECK(typed.contains("filter  /Build-2_"));
    CHECK(LinesStarting(typed, "build-") == 10);
    CHECK(LinesStarting(typed, "build-2") == 10);
    CHECK(run.frames[9].contains("filter  /Build-2q_"));
    CHECK(LinesStarting(run.frames[9], "build-") == 0);
    CHECK(LinesStarting(run.frames[10], "build-") == 10);
    CHECK(run.frames[11].contains(std::format("filter  /Build-2  10 of {} machines; / edits", Machines)));
    CHECK(LinesStarting(run.frames[13], "build-") == Machines);
    CHECK_FALSE(run.frames[13].contains("filter  /"));
}

TEST_CASE("the fleet chart yields rows to the table, and grows into rows nothing else wants",
          "[cli][dashboard][panel][fleet][chart][parity]")
{
    // #134 F12 and F13. WHAT DISTINGUISHES: at 80x24 with twelve machines the Sixel rung draws NO chart and
    // every machine row, where a chart kept at the table's cost would hide rows; with one machine at
    // 120x40 its band grows to the tallest a band gets, the frame is the terminal's height, and the
    // source line sits on its last content row.
    auto encoder = ScriptedSixelEncoder {};
    auto context = UnicodeContext();
    context.rung = RenderRung::Sixel;
    context.sixel = &encoder;

    auto const crowded = RunFleet({ FleetSampleOf(1, LeaderFleetText(FleetMachines)), Tick }, 80, 24, context, ChartCell);
    REQUIRE(crowded.frames.size() == 1);
    CHECK(crowded.placements.front().empty());
    CHECK(LinesStarting(crowded.frames.front(), "build-") == FleetMachines);

    auto const roomy = RunFleet({ FleetSampleOf(1, LeaderFleetText(1)), Tick }, 120, 40, context, ChartCell);
    REQUIRE(roomy.frames.size() == 1);
    REQUIRE(roomy.placements.front().size() == 2);
    // The one growth policy every history chart has, not a bound of the fleet's own.
    CHECK(Unwrap(FleetPanel().document).chartGrowth.bandCellsMost == ChartGrowth {}.bandCellsMost);
    CHECK(roomy.placements.front().front().cellsHigh == ChartGrowth {}.bandCellsMost);
    auto const lines = Lines(roomy.frames.front());
    CHECK(lines.size() == 40);
    CHECK(lines.at(38).contains(FleetSourceText()));
}

TEST_CASE("the Dispatched tile draws its trend across the samples, and no trend on a rung without one",
          "[cli][dashboard][panel][fleet][parity]")
{
    // #134 F5: the page marks `dispatched` with a sparkline, so the tile carries one. WHAT DISTINGUISHES: after
    // three samples the text after `Dispatched`'s figure holds block glyphs -- a tile drawing its words column
    // blank passes every label check -- and on the ASCII rung, which draws no sparkline anywhere (§10), there
    // are none.
    auto const kpis = Distributed::FleetKpis();
    auto const* const trended = FindIfOrNull(kpis, [](Distributed::FleetKpiText const& kpi) { return kpi.sparkline; });
    REQUIRE(trended != nullptr);
    auto script = std::vector<DashboardEvent> {
        FleetSampleOf(1, FleetText(3)), FleetSampleOf(2, FleetText(3)), FleetSampleOf(3, FleetText(3)), Tick
    };
    auto const drawn = RunFleet(script, 132, 40, UnicodeContext());
    auto ascii = UnicodeContext();
    ascii.rung = RenderRung::Ascii;
    auto const plain = RunFleet(script, 132, 40, ascii);
    REQUIRE(drawn.frames.size() == 1);
    REQUIRE(plain.frames.size() == 1);

    auto const blocks = [](std::string_view text) {
        return std::ranges::count_if(BlockLevels, [text](std::string_view glyph) { return text.contains(glyph); });
    };
    auto const after = AfterWord(drawn.frames.front(), trended->label);
    REQUIRE(after.has_value());
    CHECK(Unwrap(after).starts_with("12 884"));
    CHECK(blocks(Unwrap(after)) > 0);
    CHECK(blocks(AfterWord(plain.frames.front(), trended->label).value_or("")) == 0);
}

TEST_CASE("on a text rung the fleet frame is still the terminal's height, its source line at the bottom",
          "[cli][dashboard][panel][fleet][parity]")
{
    // #134 F13 on the rungs with no image. WHAT DISTINGUISHES: one machine at 120x40 fills forty rows with
    // the source on the last content row -- the frame the owner saw ended halfway down the screen -- and
    // the rows the chart did not grow into are blank above the source line, not below it.
    auto const run = RunFleet({ FleetSampleOf(1, LeaderFleetText(1)), Tick }, 120, 40, UnicodeContext());
    REQUIRE(run.frames.size() == 1);
    auto const lines = Lines(run.frames.front());
    CHECK(lines.size() == 40);
    CHECK(lines.at(38).contains(FleetSourceText()));
    CHECK(Trimmed(Columns(lines.at(37), 1, FakeCellWidth(lines.at(37)) - 2)).empty());
}

namespace
{

/// The tones of the runs in @p run's frame @p index covering exactly @p text on the row holding @p rowText.
/// @param run What was drawn.
/// @param index Which frame.
/// @param rowText Text the row holds.
/// @param text The run's bytes.
/// @return The tones, in span order; empty when no run covers exactly that text there.
[[nodiscard]] std::vector<FrameTone> TonesOver(FleetRun const& run,
                                               std::size_t index,
                                               std::string_view rowText,
                                               std::string_view text)
{
    auto const& frame = run.frames.at(index);
    auto const row = LineIndexHolding(frame, rowText) + 1;
    auto tones = std::vector<FrameTone> {};
    for (auto const& span: run.spans.at(index))
        if (span.row == row && SpanText(frame, span) == text)
            tones.push_back(span.tone);
    return tones;
}

} // namespace

TEST_CASE("a fleet tile's label and words recede, its figure is weighted, and a refusal above zero alerts",
          "[cli][dashboard][panel][fleet][tone]")
{
    // #134 G1. WHAT DISTINGUISHES: each label is one Label run over exactly its word -- not its padding --
    // `Dispatched`'s figure is a Figure run, `of 192 slots` a Label run, and `Refused` is an Alert run at 1
    // and a Figure run at 0 over the same document, so the alert is the value and not the tile.
    auto const refusing = FleetText(3);
    auto quiet = refusing;
    auto const refusedRow = std::string { "\nrefused\t1\tcount\t-\n" };
    REQUIRE(quiet.contains(refusedRow));
    quiet.replace(quiet.find(refusedRow), refusedRow.size(), "\nrefused\t0\tcount\t-\n");

    auto const run =
        RunFleet({ FleetSampleOf(1, refusing), Tick, FleetSampleOf(2, quiet), Tick }, 132, 40, UnicodeContext());
    REQUIRE(run.frames.size() == 2);
    for (auto const& kpi: Distributed::FleetKpis())
    {
        INFO("tile " << kpi.label);
        CHECK(TonesOver(run, 0, kpi.label, kpi.label) == std::vector<FrameTone> { FrameTone::Label });
    }
    CHECK(TonesOver(run, 0, "Dispatched", "12 884") == std::vector<FrameTone> { FrameTone::Figure });
    CHECK(TonesOver(run, 0, "Compiling now", "of 192 slots") == std::vector<FrameTone> { FrameTone::Label });
    CHECK(TonesOver(run, 0, "Refused", "1") == std::vector<FrameTone> { FrameTone::Alert });
    CHECK(TonesOver(run, 1, "Refused", "0") == std::vector<FrameTone> { FrameTone::Figure });
}

TEST_CASE("a worker's limit is toned by the leader, and a table's heading, key hint and overflow are labels",
          "[cli][dashboard][panel][fleet][tone]")
{
    // #134 G1. WHAT DISTINGUISHES: `registered` is Fresh and `scratch` an Alert, by the leader's one table --
    // a panel tinting every limit alike fails the pair -- the heading's `limited-by` is a Label run over the
    // word alone, and so are `keys  m w l c t` and the overflow line, while no machine name is toned.
    auto document = FleetText(3);
    document += "\n# workers\nendpoint\tlimited-by\n";
    for (auto const index: std::views::iota(1, 31))
        document += std::format("build-{:02}:7070\t{}\n", index, index == 2 ? "scratch" : "registered");

    auto const run = RunFleet({ FleetSampleOf(1, document), Tick, KeyOf("w") }, 132, 24, UnicodeContext());
    REQUIRE(run.frames.size() == 2);
    auto const& frame = run.frames[1];
    CHECK(TonesOver(run, 1, "build-01:7070", "registered") == std::vector<FrameTone> { FrameTone::Fresh });
    CHECK(TonesOver(run, 1, "build-02:7070", "scratch") == std::vector<FrameTone> { FrameTone::Alert });
    CHECK(TonesOver(run, 1, "build-02:7070", "build-02:7070").empty());
    CHECK(TonesOver(run, 1, "limited-by", "limited-by") == std::vector<FrameTone> { FrameTone::Label });
    CHECK(TonesOver(run, 1, "keys  m w l c t", "keys  m w l c t") == std::vector<FrameTone> { FrameTone::Label });

    auto const overflow = LineStarting(frame, "... ");
    REQUIRE(overflow.has_value());
    auto const& line = Unwrap(overflow);
    constexpr auto Last = std::string_view { "/ filters" };
    auto const at = line.find("... ");
    auto const end = line.find(Last);
    REQUIRE(at != std::string::npos);
    REQUIRE(end != std::string::npos);
    auto const words = line.substr(at, end + Last.size() - at);
    CHECK(TonesOver(run, 1, words, words) == std::vector<FrameTone> { FrameTone::Label });
}

TEST_CASE("the fleet chart explains itself: its figure and span, each machine's newest figure, a time axis and a scale",
          "[cli][dashboard][panel][fleet][chart][parity]")
{
    // #134 F14: the owner saw bands of colour and could not tell what they meant. WHAT DISTINGUISHES, each
    // against that frame:
    //   - the row above the image names the figure and the span its width covers;
    //   - each band's row names its machine and its newest figure left of the image, `-` for the machine the
    //     newest sample did not read, so no figure is a reading nobody gave;
    //   - the row under the image runs from how long ago its left edge is to `now`, under the image exactly;
    //   - the legend's scale is an image of its own between the two values it runs between, over blank cells.
    auto const drawing = DrawChart(RenderRung::Sixel, ChartCell, 100, 60);
    REQUIRE(drawing.placements.at(0).size() == 2);
    auto const& chart = drawing.placements[0][0];
    auto const& scale = drawing.placements[0][1];
    auto const lines = Lines(drawing.frames.at(0));
    auto const& metric = FleetChartMetrics.front();
    auto const window = ChartWindows.front();

    auto const title = Trimmed(Columns(lines.at(chart.row - 2), 1, FakeCellWidth(lines.at(chart.row - 2)) - 2));
    CHECK(title == std::format("{} per machine, last {} samples", metric.key, window));

    auto const figure = [](std::uint64_t permille) {
        return Distributed::HumanFleetFigure(permille, Distributed::CellFormat::Permille);
    };
    auto const bandCells = chart.cellsHigh / 3;
    REQUIRE(bandCells * 3 == chart.cellsHigh);
    auto const expected = std::to_array<std::pair<std::string_view, std::string>>(
        { { "build-01:7070", figure(300) }, { "build-02:7070", figure(800) }, { "build-03:7070", figure(700) } });
    for (auto const index: std::views::iota(std::size_t { 0 }, expected.size()))
    {
        auto const& row = lines.at(chart.row - 1 + (index * bandCells));
        auto const label = Trimmed(Columns(row, 1, chart.column - 2));
        INFO("band " << index << ": " << label);
        CHECK(label.starts_with(expected[index].first));
        CHECK(label.ends_with(expected[index].second));
    }

    auto const& axis = lines.at(chart.row - 1 + chart.cellsHigh);
    auto const under = Columns(axis, chart.column - 1, chart.cellsWide);
    CHECK(under.starts_with(std::format("-{} samples", window)));
    CHECK(under.ends_with("now"));

    auto const& legend = lines.at(scale.row - 1);
    CHECK(scale.row == chart.row + chart.cellsHigh + 1);
    CHECK(scale.cellsHigh == 1);
    CHECK(Trimmed(Columns(legend, scale.column - 1, scale.cellsWide)).empty());
    CHECK(Trimmed(Columns(legend, 1, scale.column - 2)).ends_with(figure(0)));
    CHECK(
        Trimmed(
            Columns(legend, scale.column - 1 + scale.cellsWide, FakeCellWidth(legend) - 1 - scale.cellsWide - scale.column))
            .starts_with(figure(1000)));
    CHECK(legend.contains(std::format("bar: {}, grey: to {}, blank: no reading", metric.key, figure(1000))));
}

TEST_CASE("before any machine reports the chart's figure, the chart says so on every rung and draws no image",
          "[cli][dashboard][panel][fleet][chart][parity]")
{
    // WHAT DISTINGUISHES: an empty chart would read as a fleet with nothing on it; the line names the figure
    // and says nobody reported it, on the Sixel rung and on the Unicode one alike, and nothing is placed or encoded.
    auto encoder = ScriptedSixelEncoder {};
    auto context = UnicodeContext();
    context.rung = RenderRung::Sixel;
    context.sixel = &encoder;
    auto const said = std::format("{} per machine: no machine has reported it yet", FleetChartMetrics.front().key);
    auto const run = RunFleet({ FleetSampleOf(1, FleetText(3)), Tick }, 100, 60, context, ChartCell);
    REQUIRE(run.frames.size() == 1);
    CHECK(run.placements.front().empty());
    CHECK(encoder.Requests().empty());
    CHECK(run.frames.front().contains(said));
    auto const text = RunFleet({ FleetSampleOf(1, FleetText(3)), Tick }, 100, 60, UnicodeContext());
    REQUIRE(text.frames.size() == 1);
    CHECK(text.frames.front().contains(said));
}

namespace
{

/// The text chart in @p frame over @p machines, found by its title and then each machine's first band line.
/// @param frame The frame.
/// @param machines The bands' labels, top to bottom.
/// @return The lines; the case fails where a line is missing.
[[nodiscard]] ChartLines FleetTextChartIn(std::string_view frame, std::span<std::string_view const> machines)
{
    auto const lines = Lines(frame);
    auto chart = ChartLines { .title = LineIndexHolding(frame, " per machine, last "), .bands = {}, .axis = 0 };
    REQUIRE(chart.title < lines.size());
    for (auto const machine: machines)
    {
        // The chart is above the table, so a machine's first line after the title is its band.
        auto at = chart.title + 1;
        while (at < lines.size() && !Inside(lines[at]).starts_with(machine))
            ++at;
        REQUIRE(at < lines.size());
        chart.bands.push_back(at);
    }
    chart.axis = chart.title + 1;
    while (chart.axis < lines.size() && !Inside(lines[chart.axis]).ends_with("now"))
        ++chart.axis;
    REQUIRE(chart.axis < lines.size());
    return chart;
}

} // namespace

TEST_CASE("on a text rung the fleet chart is the rung's marks: a sample a cell, a blank for no reading, and a legend",
          "[cli][dashboard][panel][fleet][chart][parity]")
{
    // #134 F14 below the Sixel rung, where the chart used to be missing. WHAT DISTINGUISHES, over three samples, the
    // second of which has no reading for build-02:
    //   - under the tiles and over the strip, three bands of one height, each naming its machine and its newest
    //     figure, and the axis right under them;
    //   - the span the title names is exactly the cells across the axis, so a cell is a sample;
    //   - build-02's second sample is blank in every row of its band while its neighbours are marks -- a zero drawn
    //     there would be a reading nobody gave;
    //   - the legend names the zero mark and the full cell, and that every band's top is the metric's whole;
    //   - on the ASCII rung the same rows are ASCII marks, byte for byte.
    auto const figure = [](std::uint64_t permille) {
        return Distributed::HumanFleetFigure(permille, Distributed::CellFormat::Permille);
    };
    auto const& metric = FleetChartMetrics.front();
    constexpr auto Machines = std::to_array<std::string_view>({ "build-01:7070", "build-02:7070", "build-03:7070" });
    auto const newest = std::to_array({ figure(300), figure(800), figure(700) });

    auto const drawing = DrawChart(RenderRung::Unicode, std::nullopt, 100, 60);
    REQUIRE(drawing.frames.size() == 1);
    auto const& frame = drawing.frames.front();
    INFO(frame);
    auto const lines = Lines(frame);
    auto const chart = FleetTextChartIn(frame, Machines);
    // Between the tiles and the strip: under every tile, over the section tabs.
    for (auto const& kpi: Distributed::FleetKpis())
    {
        INFO("tile " << kpi.label);
        CHECK(LineIndexHolding(frame, kpi.label) < chart.title);
    }
    CHECK(chart.axis + 1 < LineIndexHolding(frame, "[machines]"));
    auto const bandCells = chart.BandRows();
    CHECK(bandCells >= 2);
    for (auto const index: { std::size_t { 1 }, std::size_t { 2 } })
    {
        CHECK(chart.bands[index] - chart.bands[index - 1] == bandCells + 1);
        CHECK(Inside(lines.at(chart.bands[index] - 1)).empty());
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, Machines.size()))
    {
        INFO("band " << Machines[index]);
        CHECK(Inside(lines.at(chart.bands[index])).contains(newest[index]));
    }

    auto const title = Inside(lines.at(chart.title));
    auto const lead = std::format("{} per machine, last ", metric.key);
    REQUIRE(title.starts_with(lead));
    REQUIRE(title.ends_with(" samples"));
    auto const span = title.substr(lead.size(), title.size() - lead.size() - std::string_view { " samples" }.size());
    auto const axis = Inside(lines.at(chart.axis));
    CHECK(axis.starts_with(std::format("-{} samples", span)));
    CHECK(std::to_string(CodePoints(axis).size()) == span);

    for (auto const row: std::views::iota(chart.bands[1], chart.bands[1] + bandCells))
    {
        INFO("build-02 row " << row);
        auto const cells = NewestCells(lines.at(row), 3);
        CHECK(cells[1] == " ");
    }
    auto const floorOf = [&](std::size_t band) {
        return NewestCells(lines.at(chart.bands[band] + bandCells - 1), 3);
    };
    CHECK(floorOf(1)[0] != " ");
    CHECK(floorOf(1)[2] != " ");
    CHECK(std::ranges::none_of(floorOf(0), [](std::string const& cell) { return cell == " "; }));

    auto const legend = Inside(lines.at(chart.axis + 1));
    CHECK(legend.contains(std::format("{} zero", BlockLevels.front())));
    CHECK(legend.contains(std::format("{} the top", BlockLevels.back())));
    CHECK(legend.contains(std::format("bar: {} of {}, blank: no reading", metric.key, figure(1000))));

    auto const ascii = DrawChart(RenderRung::Ascii, std::nullopt, 100, 60);
    REQUIRE(ascii.frames.size() == 1);
    auto const asciiLines = Lines(ascii.frames.front());
    auto const asciiChart = FleetTextChartIn(ascii.frames.front(), Machines);
    auto marks = std::size_t { 0 };
    for (auto const row: std::views::iota(asciiChart.bands.front(), asciiChart.axis))
    {
        CHECK(std::ranges::all_of(asciiLines.at(row), [](char byte) { return static_cast<unsigned char>(byte) < 0x80U; }));
        marks += static_cast<std::size_t>(std::ranges::count(asciiLines.at(row), '#'));
    }
    CHECK(marks > 0);
}

namespace
{

/// A Unicode fleet frame over 100 samples of build-01 and build-02, with build-03 in the first two when @p stale.
/// @param stale Whether build-03 reported, long ago.
/// @param columns The terminal's width.
/// @param rows The terminal's height.
/// @return The frame.
[[nodiscard]] std::string StaleMachineFrame(bool stale, int columns, int rows)
{
    auto view = PanelView {
        FleetPanel(),
        PanelContext { .absent = std::string { Absent }, .cellWidth = &FakeCellWidth, .rung = RenderRung::Unicode }
    };
    auto script = std::vector<DashboardEvent> { DashboardEvent {
        .kind = DashboardEventKind::Resize, .columns = columns, .rows = rows } };
    for (auto const second: std::views::iota(1, 101))
        script.push_back(FleetSampleOf(second,
                                       ChartText(stale && second <= 2 ? std::vector<std::string> { "100", "900", "500" }
                                                                      : std::vector<std::string> { "100", "900" })));
    script.push_back(Tick);
    auto sink = CollectingSink {};
    (void) Drive(std::move(script), DashboardLimits {}, view, sink, &ReadFleetSample);
    REQUIRE(sink.frames.size() == 1);
    return sink.frames.front();
}

} // namespace

TEST_CASE("a text fleet chart bands the machines its cells read, and counts them over those cells",
          "[cli][dashboard][panel][fleet][chart]")
{
    // A text chart's span is its cells, a sample each, so a machine that reported only before its oldest cell has
    // no band in it. WHAT DISTINGUISHES, over 100 samples in which build-03 reported only the first two, at a width
    // whose cells hold fewer than 98 of them: two bands, no build-03 in the chart, and no "the first 2 of 3", which
    // counts a machine the chart cannot draw. The title has room for that qualifier -- it is Low and goes first for
    // width, so without the room its absence would say nothing.
    auto const frame = StaleMachineFrame(true, 120, 60);
    INFO(frame);
    auto const lines = Lines(frame);

    constexpr auto Read = std::to_array<std::string_view>({ "build-01:7070", "build-02:7070" });
    auto const chart = FleetTextChartIn(frame, Read);
    auto const title = Inside(lines.at(chart.title));
    auto const lead = std::format("{} per machine, last ", FleetChartMetrics.front().key);
    REQUIRE(title.starts_with(lead));
    CHECK(std::stoul(title.substr(lead.size())) < 98);
    CHECK_FALSE(title.contains("by name"));
    constexpr auto Qualifier = std::string_view { "; the first 2 of 3 by name" };
    CHECK(FakeCellWidth(lines.at(chart.title)) - 6 >= FakeCellWidth(title) + FakeCellWidth(Qualifier));
    CHECK(chart.bands[1] - chart.bands[0] == chart.BandRows() + chart.Spacer());
    CHECK(chart.Spacer() <= 1);
    for (auto const row: std::views::iota(chart.title, chart.axis + 2))
        CHECK_FALSE(lines.at(row).contains("build-03"));
}

TEST_CASE("a machine outside a text chart's span changes nothing about its rows", "[cli][dashboard][panel][fleet][chart]")
{
    // The grow step offers the chart a band per machine the history holds, and a text span holds fewer. WHAT
    // DISTINGUISHES: with build-03 long gone the chart is line for line the one drawn where it never reported -- rows
    // laid out for three bands and drawn for two leave both bands shorter -- at a height where the bands have not
    // reached the growth's most, so taller bands are there to be had.
    constexpr auto Read = std::to_array<std::string_view>({ "build-01:7070", "build-02:7070" });
    auto const stale = StaleMachineFrame(true, 120, 40);
    auto const never = StaleMachineFrame(false, 120, 40);
    INFO(stale);
    INFO(never);
    auto const staleChart = FleetTextChartIn(stale, Read);
    auto const neverChart = FleetTextChartIn(never, Read);
    REQUIRE(neverChart.BandRows() < ChartGrowth {}.bandCellsMost);
    CHECK(staleChart.BandRows() == neverChart.BandRows());
    auto const staleLines = Lines(stale);
    auto const neverLines = Lines(never);
    REQUIRE(staleChart.title == neverChart.title);
    REQUIRE(staleChart.axis == neverChart.axis);
    for (auto const row: std::views::iota(staleChart.title, staleChart.axis + 2))
    {
        INFO("line " << row);
        CHECK(staleLines.at(row) == neverLines.at(row));
    }
}

TEST_CASE("at every height the fleet chart takes only rows nothing else wanted", "[cli][dashboard][panel][fleet][chart]")
{
    // WHAT DISTINGUISHES: the ASCII rung draws the chart in marks and the piped glyphs, alike in every other glyph,
    // draw none. At every height from 12 to 60 rows, the ASCII frame's lines outside its chart -- blank rows
    // aside -- are the chartless frame's, so a chart that took a row more than the first fit left over pushes a table
    // row, a tile or the strip out at some height. At least one height draws a chart, or nothing was compared.
    auto const kept = [](std::string const& frame) {
        auto const lines = Lines(frame);
        auto const [first, last] = ChartLineRange(lines);
        auto content = std::vector<std::string> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, lines.size()))
            if ((index < first || index >= last) && !Inside(lines[index]).empty())
                content.push_back(lines[index]);
        return content;
    };
    auto charted = std::size_t { 0 };
    for (auto const rows: std::views::iota(12, 61))
    {
        auto const ascii = DrawChart(RenderRung::Ascii, std::nullopt, 100, rows);
        auto const plain = DrawChart(RenderRung::Piped, std::nullopt, 100, rows);
        REQUIRE(ascii.frames.size() == 1);
        REQUIRE(plain.frames.size() == 1);
        INFO("rows " << rows << "\n" << ascii.frames.front() << plain.frames.front());
        auto const lines = Lines(ascii.frames.front());
        if (ChartLineRange(lines).first < lines.size())
            ++charted;
        CHECK(ChartLineRange(Lines(plain.frames.front())).first == Lines(plain.frames.front()).size());
        CHECK(kept(ascii.frames.front()) == kept(plain.frames.front()));
    }
    CHECK(charted > 0);
}

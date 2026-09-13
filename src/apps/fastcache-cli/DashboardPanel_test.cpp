// SPDX-License-Identifier: Apache-2.0
#include "DashboardGlyphs.hpp"
#include "DashboardPanel.hpp"
#include "DashboardPanels.hpp"
#include "DashboardRig.hpp"
#include "ScriptedCellWidth.hpp"
#include "StatsSource.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
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

    auto const middle = gappedSpark.size() - 2;
    CHECK(gappedSpark[middle] != zeroedSpark[middle]);
    CHECK(gappedSpark[middle] == " ");
    CHECK(zeroedSpark[middle] == "\xe2\x96\x81");
    CHECK(gappedSpark[middle - 1] == zeroedSpark[middle - 1]);
    CHECK(gappedSpark[middle + 1] == zeroedSpark[middle + 1]);
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
    CHECK(FigureOf(Unwrap(restarted)) == Absent);
    CHECK(SparkOf(Unwrap(restarted), "accepted").back() == " ");
    CHECK(FigureOf(Unwrap(after)) == "10");
    CHECK(SparkOf(Unwrap(after), "accepted").back() != " ");
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
    // measured at the old size (the trends' beside reserve) survives into the new layout.
    auto script = TwoTierScript();
    script.push_back(DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 50, .rows = 12 });
    script.push_back(Tick);
    auto const frames = FramesAt(CachePanel(), std::move(script), RenderRung::Unicode, 132, 40);
    REQUIRE(frames.size() == 2);
    CHECK(WidestLine(frames[0]) == 132);
    CHECK(WidestLine(frames[1]) == 50);
    CHECK(Lines(frames[1]).size() <= 12);
    CHECK(Lines(frames[0]).size() > Lines(frames[1]).size());
    CHECK(frames[1] == CacheFrameAt(50, 12));
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

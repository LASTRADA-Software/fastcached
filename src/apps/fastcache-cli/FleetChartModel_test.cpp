// SPDX-License-Identifier: Apache-2.0
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"
#include "FleetReading.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Distributed;
using FastCache::Testing::Unwrap;

namespace
{

/// The text of a document whose machines section has @p rows, each `{ endpoint, cpu-busy }`.
///
/// The header is the leader's own column list for the section, so the fixture carries every column a
/// real document does; only the two the chart reads are filled, and `-` is the absent cell.
/// @param rows The machines.
/// @return The text.
[[nodiscard]] std::string MachinesText(std::vector<std::pair<std::string, std::string>> const& rows)
{
    auto snapshot = FleetSnapshot {};
    snapshot.nodes.emplace_back();
    auto const columns = FleetColumnNames(FleetSection::Machines, snapshot);
    auto header = std::string {};
    for (auto const index: std::views::iota(std::size_t { 0 }, columns.size()))
        header += (index == 0 ? "" : "\t") + columns[index];
    auto text = std::format("# {}\n{}\n", FleetSectionTable[static_cast<std::size_t>(FleetSection::Machines)].key, header);
    for (auto const& [endpoint, busy]: rows)
    {
        auto line = std::string {};
        for (auto const index: std::views::iota(std::size_t { 0 }, columns.size()))
        {
            auto const& name = columns[index];
            auto cell = std::string { "-" };
            if (name == FleetChartMetrics.front().subjectColumn)
                cell = endpoint;
            else if (name == FleetChartMetrics.front().valueColumn)
                cell = busy;
            line += (index == 0 ? "" : "\t") + cell;
        }
        text += line + "\n";
    }
    return text;
}

/// The parse of `MachinesText(rows)`.
/// @param rows The machines.
/// @return The document.
[[nodiscard]] FleetDocument MachinesDocument(std::vector<std::pair<std::string, std::string>> const& rows)
{
    auto parsed = ParseFleetDocument(MachinesText(rows));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// A history entry holding @p document's chart points.
/// @param document The document.
/// @return The entry.
[[nodiscard]] HistoryEntry EntryOf(FleetDocument const& document)
{
    return HistoryEntry { .reading = RecordValue({}), .elapsed = {}, .points = FleetChartPoints(document) };
}

/// The alpha of pixel (@p x, @p y).
/// @param raster The raster.
/// @param x Column.
/// @param y Row.
/// @return Its alpha byte.
[[nodiscard]] std::uint8_t AlphaAt(ChartRaster const& raster, std::size_t x, std::size_t y)
{
    return raster.rgba.at((((y * raster.width) + x) * 4) + 3);
}

} // namespace

TEST_CASE("every chart metric reads columns the leader's machines table actually has", "[cli][fleet][chart]")
{
    // The metric names its two columns, so this is what stops a column renamed in `FleetView.cpp` from
    // leaving the chart empty in silence. WHAT DISTINGUISHES: the names are asked of the leader's own
    // column list for a snapshot with a machine in it, and the control is that the list is not empty.
    auto snapshot = FleetSnapshot {};
    snapshot.nodes.emplace_back();
    for (auto const& metric: FleetChartMetrics)
    {
        INFO("metric " << metric.key);
        auto const columns = FleetColumnNames(metric.section, snapshot);
        REQUIRE(!columns.empty());
        CHECK(std::ranges::find(columns, metric.subjectColumn) != columns.end());
        CHECK(std::ranges::find(columns, metric.valueColumn) != columns.end());
    }
}

TEST_CASE("a document gives one chart point per machine it read, and none for a machine it did not", "[cli][fleet][chart]")
{
    // WHAT DISTINGUISHES: an absent figure gives NO point -- the chart then draws a gap -- rather than a
    // point of zero, which would draw an unread machine as an idle one; and a value is scaled, so 853
    // thousandths is 0.853.
    auto const points = FleetChartPoints(MachinesDocument({ { "a:1", "853" }, { "b:1", "-" }, { "c:1", "0" } }));
    REQUIRE(points.size() == 2);
    CHECK(points[0].series == FleetChartMetrics.front().key);
    CHECK(points[0].subject == "a:1");
    CHECK(std::abs(points[0].value - 0.853) < 1e-9);
    CHECK(points[1].subject == "c:1");
    CHECK(points[1].value == 0.0);
}

TEST_CASE("a fleet reading carries the chart's points, taken before its parse is handed over", "[cli][fleet][chart]")
{
    // The reader is where points come from. WHAT DISTINGUISHES: the reading carries one point per machine
    // the document read, with their subjects -- a reader taking them after the parse moved into `document`
    // reads a moved-from document and carries none -- and it still hands the document over.
    auto const reading = ReadFleetSample(
        DashboardEvent { .kind = DashboardEventKind::Sample,
                         .document = std::optional<std::string> { MachinesText({ { "a:1", "500" }, { "b:1", "250" } }) } });
    REQUIRE(reading.outcome == Outcome::Affirmative);
    REQUIRE(reading.points.size() == 2);
    CHECK(reading.points[0].subject == "a:1");
    CHECK(reading.points[1].subject == "b:1");
    CHECK(std::abs(reading.points[1].value - 0.25) < 1e-9);
    CHECK(reading.document != nullptr);
}

namespace
{

/// The RGB of pixel (@p x, @p y).
/// @param raster The raster.
/// @param x Column.
/// @param y Row.
/// @return Red, green and blue in one number.
[[nodiscard]] std::uint32_t RgbAt(ChartRaster const& raster, std::size_t x, std::size_t y)
{
    auto const at = ((y * raster.width) + x) * 4;
    return (std::uint32_t { raster.rgba.at(at) } << 16) | (std::uint32_t { raster.rgba.at(at + 1) } << 8)
           | std::uint32_t { raster.rgba.at(at + 2) };
}

/// How many of column @p x's pixels between rows @p top and @p floor are not the colour at row @p top.
/// @param raster The raster.
/// @param x Column.
/// @param top The band's first drawn row, which is track under any bar short of full.
/// @param floor One past the band's last row.
/// @return The bar's height in pixels.
[[nodiscard]] std::size_t BarAt(ChartRaster const& raster, std::size_t x, std::size_t top, std::size_t floor)
{
    auto bar = std::size_t { 0 };
    for (auto const y: std::views::iota(top, floor))
        bar += RgbAt(raster, x, y) != RgbAt(raster, x, top) ? 1 : 0;
    return bar;
}

} // namespace

TEST_CASE("the chart's span is a fixed step that holds the history, not the history's own length", "[cli][fleet][chart]")
{
    // #134 F14. WHAT DISTINGUISHES: three samples are drawn across thirty sample widths, not three -- the
    // three wide blocks nobody could read -- and a history past the largest span keeps the largest.
    CHECK(ChartWindowFor(0) == ChartWindows.front());
    CHECK(ChartWindowFor(3) == ChartWindows.front());
    CHECK(ChartWindowFor(ChartWindows.front()) == ChartWindows.front());
    CHECK(ChartWindowFor(ChartWindows.front() + 1) == ChartWindows[1]);
    CHECK(ChartWindowFor(ChartWindows.back() * 2) == ChartWindows.back());
}

TEST_CASE("the chart bands every machine the span read, by name, with its newest figure or none", "[cli][fleet][chart]")
{
    // WHAT DISTINGUISHES: `b` is banded although the newest sample did not read it, and its newest figure
    // is none rather than the last one read -- a label saying 90 % for a machine that stopped reporting
    // would be a reading nobody gave -- and `z` read before the span is not banded at all.
    auto history = std::deque<HistoryEntry> {};
    history.push_back(EntryOf(MachinesDocument({ { "z:1", "100" } })));
    for (auto const index: std::views::iota(0, 2))
    {
        (void) index;
        history.push_back(EntryOf(MachinesDocument({ { "c:1", "500" } })));
    }
    history.push_back(EntryOf(MachinesDocument({ { "b:1", "900" }, { "a:1", "100" } })));
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "300" }, { "b:1", "-" }, { "c:1", "0" } })));

    auto const bands = FleetChartBands(history, FleetChartMetrics.front(), 4);
    REQUIRE(bands.size() == 3);
    CHECK(bands[0].subject == "a:1");
    CHECK(bands[1].subject == "b:1");
    CHECK(bands[2].subject == "c:1");
    REQUIRE(bands[0].latest.has_value());
    CHECK(std::abs(Unwrap(bands[0].latest) - 0.3) < 1e-9);
    CHECK_FALSE(bands[1].latest.has_value());
    CHECK(bands[2].latest == std::optional<double> { 0.0 });
}

TEST_CASE("the chart raster draws a reading as a bar over a grey track, zero as the track, and a gap as nothing",
          "[cli][fleet][chart]")
{
    // #134 F14, the owner's "block-like in one colour or another". WHAT DISTINGUISHES, over two machines in a
    // span of thirty samples, three of them read:
    //   - the samples not yet taken, left of the readings, are transparent: the chart fills from the right;
    //   - `b` unread in the middle sample is transparent there, while `a` beside it is drawn;
    //   - a bar is as tall as its value: 10 % is one pixel of nine and 90 % eight, over the track;
    //   - `a` at zero is the track alone, every pixel one colour and opaque -- visible, and not a gap;
    //   - each band's top row is transparent, so the two machines do not merge;
    //   - a sample that read nothing is transparent for every machine.
    auto history = std::deque<HistoryEntry> {};
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "100" }, { "b:1", "900" } })));
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "0" }, { "b:1", "-" } })));
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "300" }, { "b:1", "800" } })));
    auto const& metric = FleetChartMetrics.front();
    auto const bands = FleetChartBands(history, metric, 30);
    REQUIRE(bands.size() == 2);

    constexpr auto Width = std::size_t { 60 };
    constexpr auto Height = std::size_t { 20 };
    auto const raster = FleetChartRaster(history, metric, bands, 30, Width, Height);
    REQUIRE(raster.width == Width);
    REQUIRE(raster.height == Height);
    REQUIRE(raster.rgba.size() == Width * Height * 4);

    // Thirty samples of two pixels; the three read are the last six columns. Bands of ten rows, the first transparent.
    auto const sampleX = [](std::size_t sample) {
        return Width - ((3 - sample) * 2);
    };
    CHECK(AlphaAt(raster, 10, 5) == 0x00);
    CHECK(AlphaAt(raster, sampleX(0) - 1, 5) == 0x00);
    CHECK(AlphaAt(raster, sampleX(0), 0) == 0x00);
    CHECK(AlphaAt(raster, sampleX(0), 10) == 0x00);
    CHECK(AlphaAt(raster, sampleX(1), 15) == 0x00); // `b` not read
    CHECK(AlphaAt(raster, sampleX(1), 5) == 0xff);  // `a` at zero, beside it

    CHECK(BarAt(raster, sampleX(0), 1, 10) == 1);
    CHECK(BarAt(raster, sampleX(0), 11, 20) == 8);
    CHECK(BarAt(raster, sampleX(1), 1, 10) == 0);
    CHECK(RgbAt(raster, sampleX(1), 9) == RgbAt(raster, sampleX(0), 1)); // zero is the track
    CHECK(RgbAt(raster, sampleX(0), 9) != RgbAt(raster, sampleX(0), 1)); // a bar is not

    history.push_back(HistoryEntry {});
    auto const withFailure = FleetChartRaster(history, metric, bands, 30, Width, Height);
    CHECK(AlphaAt(withFailure, Width - 1, 5) == 0x00);
    CHECK(AlphaAt(withFailure, Width - 1, 15) == 0x00);
    CHECK(AlphaAt(withFailure, Width - 3, 5) == 0xff);
}

TEST_CASE("one machine at a steady load is one bar height across the span", "[cli][fleet][chart]")
{
    // #134 F14's own acceptance line. WHAT DISTINGUISHES: every sample column carries the same bar, of the
    // height its value is of the band, so the image reads as a steady load and not as blocks of colour.
    auto history = std::deque<HistoryEntry> {};
    for (auto const index: std::views::iota(0, 30))
    {
        (void) index;
        history.push_back(EntryOf(MachinesDocument({ { "a:1", "500" } })));
    }
    auto const& metric = FleetChartMetrics.front();
    auto const bands = FleetChartBands(history, metric, ChartWindowFor(history.size()));
    auto const raster = FleetChartRaster(history, metric, bands, ChartWindowFor(history.size()), 90, 24);
    for (auto const x: std::views::iota(std::size_t { 0 }, std::size_t { 90 }))
    {
        INFO("column " << x);
        CHECK(BarAt(raster, x, 1, 24) == 12); // half of the 23 rows under the gap, rounded
    }
}

TEST_CASE("the chart's colour scale runs coldest to hottest in whole steps", "[cli][fleet][chart]")
{
    // WHAT DISTINGUISHES: the left and right ends differ, the scale is opaque, and it has eight colours --
    // a legend of one colour, or of a smeared gradient the encoder must quantize, cannot be read as values.
    auto const scale = ChartScaleRaster(80, 4);
    REQUIRE(scale.rgba.size() == std::size_t { 80 } * 4 * 4);
    auto colours = std::vector<std::uint32_t> {};
    for (auto const x: std::views::iota(std::size_t { 0 }, std::size_t { 80 }))
    {
        CHECK(AlphaAt(scale, x, 3) == 0xff);
        if (colours.empty() || colours.back() != RgbAt(scale, x, 0))
            colours.push_back(RgbAt(scale, x, 0));
    }
    CHECK(colours.size() == 8);
    CHECK(colours.front() != colours.back());
}

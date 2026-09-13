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
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Distributed;

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
    auto const reading = ReadFleetSample(DashboardEvent {
        .kind = DashboardEventKind::Sample,
        .document = std::expected<std::string, AdminError> { MachinesText({ { "a:1", "500" }, { "b:1", "250" } }) } });
    REQUIRE(reading.outcome == Outcome::Affirmative);
    REQUIRE(reading.points.size() == 2);
    CHECK(reading.points[0].subject == "a:1");
    CHECK(reading.points[1].subject == "b:1");
    CHECK(std::abs(reading.points[1].value - 0.25) < 1e-9);
    CHECK(reading.document != nullptr);
}

TEST_CASE("the chart raster is one band per machine, time across, and a gap is transparent", "[cli][fleet][chart]")
{
    // WHAT DISTINGUISHES: in the middle sample machine `b` was not read, and exactly its band there is
    // transparent while `a`'s band beside it is drawn -- a raster filling a gap with a zero's colour, or
    // dropping the whole sample, fails one or the other; and the raster is the size it was asked for.
    auto history = std::deque<HistoryEntry> {};
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "100" }, { "b:1", "900" } })));
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "200" }, { "b:1", "-" } })));
    history.push_back(EntryOf(MachinesDocument({ { "a:1", "300" }, { "b:1", "800" } })));

    constexpr auto Width = std::size_t { 30 };
    constexpr auto Height = std::size_t { 20 };
    auto const raster = FleetChartRaster(history, FleetChartMetrics.front(), Width, Height);
    REQUIRE(raster.width == Width);
    REQUIRE(raster.height == Height);
    REQUIRE(raster.rgba.size() == Width * Height * 4);

    // Three samples of ten columns each; two bands of ten rows each, `a` above `b` by key.
    auto const sampleMiddle = [](std::size_t sample) {
        return (sample * 10) + 5;
    };
    constexpr auto BandA = std::size_t { 5 };
    constexpr auto BandB = std::size_t { 15 };
    CHECK(AlphaAt(raster, sampleMiddle(0), BandA) == 0xff);
    CHECK(AlphaAt(raster, sampleMiddle(0), BandB) == 0xff);
    CHECK(AlphaAt(raster, sampleMiddle(1), BandA) == 0xff);
    CHECK(AlphaAt(raster, sampleMiddle(1), BandB) == 0x00); // the gap
    CHECK(AlphaAt(raster, sampleMiddle(2), BandB) == 0xff);

    // A sample that read nothing at all is a gap for every machine.
    history.push_back(HistoryEntry {});
    auto const withFailure = FleetChartRaster(history, FleetChartMetrics.front(), 40, Height);
    CHECK(AlphaAt(withFailure, 35, BandA) == 0x00);
    CHECK(AlphaAt(withFailure, 25, BandA) == 0xff);
}

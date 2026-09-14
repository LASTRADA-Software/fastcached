// SPDX-License-Identifier: Apache-2.0
//
// The one chart renderer every live-stats panel draws with. What these cases hold is the chart's three promises
// on the text side -- a reading is a bar as tall as its share, zero is distinct from nothing read, and the newest
// sample is at the right edge -- and the shares a band is scaled into.
#include "DashboardGlyphs.hpp"
#include "HistoryChart.hpp"
#include "ScriptedCellWidth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Cli;
using FastCache::Cli::Testing::FakeCellWidth;

namespace
{

/// Three marks, so every rule is visible in ASCII: the zero mark, a low partial, and a full cell.
constexpr auto Marks = std::to_array<std::string_view>({ "_", ":", "#" });

} // namespace

TEST_CASE("a text chart draws a full share as a full column, zero as the floor mark, and nothing read as blanks",
          "[cli][dashboard][chart]")
{
    // WHAT DISTINGUISHES: the three columns differ in every row where one of the rules could collapse two of them --
    // zero is not blank, a small share is not zero, and a full share fills every row.
    auto const shares = std::vector<std::optional<double>> { 1.0, 0.0, std::nullopt, 0.01 };
    auto const rows = ChartTextRows(shares, 2, 4, Marks);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "#   ");
    CHECK(rows[1] == "#_ :");
}

TEST_CASE("a text chart stacks a bar from the floor, full cells first and the partial level on top",
          "[cli][dashboard][chart]")
{
    // Two rows of three levels are six: a half share fills three, which is the floor row full and nothing above; two
    // thirds fill four, the floor full and the lowest level above it.
    auto const half = ChartTextRows(std::vector<std::optional<double>> { 0.5 }, 2, 1, Marks);
    REQUIRE(half.size() == 2);
    CHECK(half[0] == " ");
    CHECK(half[1] == "#");
    auto const twoThirds = ChartTextRows(std::vector<std::optional<double>> { 4.0 / 6.0 }, 2, 1, Marks);
    REQUIRE(twoThirds.size() == 2);
    CHECK(twoThirds[0] == "_");
    CHECK(twoThirds[1] == "#");
}

TEST_CASE("a young text chart fills from the right, and a long history shows its newest samples", "[cli][dashboard][chart]")
{
    // A sample keeps its cell: two readings in a five-cell band are the two rightmost cells, never stretched.
    auto const young = ChartTextRows(std::vector<std::optional<double>> { 1.0, 0.0 }, 1, 5, Marks);
    REQUIRE(young.size() == 1);
    CHECK(young[0] == "   #_");
    // More samples than cells: the newest ones, so the right edge is always now.
    auto const old = ChartTextRows(std::vector<std::optional<double>> { 0.0, 0.0, 1.0, 1.0 }, 1, 2, Marks);
    REQUIRE(old.size() == 1);
    CHECK(old[0] == "##");
    // The Unicode rung's marks keep every line its cell count wide.
    auto const unicode = ChartTextRows(
        std::vector<std::optional<double>> { 0.3, std::nullopt, 1.0 }, 3, 6, GlyphsFor(RenderRung::Unicode).chartLevels);
    REQUIRE(unicode.size() == 3);
    for (auto const& line: unicode)
        CHECK(FakeCellWidth(line) == 6);
}

TEST_CASE("a band's shares are its readings against its top, clamped, and a top of zero is not a gap",
          "[cli][dashboard][chart]")
{
    auto const values = std::vector<std::optional<double>> { 50.0, std::nullopt, 200.0, -1.0 };
    CHECK(SharesOf(values, 100.0) == std::vector<std::optional<double>> { 0.5, std::nullopt, 1.0, 0.0 });
    // Every reading of an idle band is zero: its top is zero, and it draws zeroes rather than nothing read.
    CHECK(SharesOf(values, 0.0) == std::vector<std::optional<double>> { 0.0, std::nullopt, 0.0, 0.0 });
    CHECK(PeakOf(values) == std::optional { 200.0 });
    CHECK_FALSE(PeakOf(std::vector<std::optional<double>> { std::nullopt }).has_value());
}

TEST_CASE("a pixel chart colours a ramp band's bars by their share, and a plain band's bars all alike",
          "[cli][dashboard][chart]")
{
    // A colour is a claim. WHAT DISTINGUISHES: in the ramp band a low bar and a full bar differ in colour, and in the
    // plain band -- the same shares -- they are one colour, which is neither the grey track nor any ramp step drawn.
    auto const shares = std::vector<std::optional<double>> { 0.25, 1.0 };
    auto const tracks = std::vector<ChartTrack> {
        ChartTrack { .label = "ramp", .latest = {}, .top = {}, .shares = shares, .paint = ChartPaint::Ramp },
        ChartTrack { .label = "plain", .latest = {}, .top = {}, .shares = shares, .paint = ChartPaint::Plain },
    };
    // Two bands of 8 pixels, two samples of 2 pixels: each band's floor row is its 8th.
    auto const raster = ChartBandsRaster(tracks, 2, 4, 16);
    auto const colourAt = [&raster](std::size_t x, std::size_t y) {
        auto const at = ((y * raster.width) + x) * 4;
        return std::to_array({ raster.rgba[at], raster.rgba[at + 1], raster.rgba[at + 2], raster.rgba[at + 3] });
    };
    auto const rampLow = colourAt(0, 7);
    auto const rampFull = colourAt(2, 7);
    auto const plainLow = colourAt(0, 15);
    auto const plainFull = colourAt(2, 15);
    REQUIRE(rampLow[3] == 0xff);
    REQUIRE(plainLow[3] == 0xff);
    CHECK(rampLow != rampFull);
    CHECK(plainLow == plainFull);
    CHECK(plainLow != rampLow);
    CHECK(plainLow != rampFull);
    // The plain bar is not the track: above the low bar, the band's track is grey.
    CHECK(plainLow != colourAt(0, 9));
}

TEST_CASE("a pixel chart draws each track's bar in its own band, and a sample not read as nothing",
          "[cli][dashboard][chart]")
{
    // Two tracks of two samples in a 4 by 8 raster: a band is four pixels, its top row the gap, so a full share is
    // three opaque pixels, a zero is the grey track, and a sample not read leaves its pixels transparent.
    auto const tracks = std::vector<ChartTrack> {
        ChartTrack { .label = "a", .latest = {}, .top = {}, .shares = { 1.0, std::nullopt } },
        ChartTrack { .label = "b", .latest = {}, .top = {}, .shares = { 0.0, 1.0 } },
    };
    auto const raster = ChartBandsRaster(tracks, 2, 4, 8);
    auto const alpha = [&raster](std::size_t x, std::size_t y) {
        return raster.rgba[(((y * raster.width) + x) * 4) + 3];
    };
    // Band a, the older sample (x 0-1): full, below the gap row.
    CHECK(alpha(0, 0) == 0);
    CHECK(alpha(0, 1) == 0xff);
    CHECK(alpha(0, 3) == 0xff);
    // Band a, the newer sample (x 2-3): not read.
    CHECK(alpha(2, 3) == 0);
    // Band b, the older sample: zero is the track, opaque, in grey rather than a ramp colour.
    CHECK(alpha(0, 7) == 0xff);
    CHECK(raster.rgba[((7 * raster.width) + 0) * 4] == 0x5f);
    // Band b, the newer sample: full.
    CHECK(alpha(3, 5) == 0xff);
}

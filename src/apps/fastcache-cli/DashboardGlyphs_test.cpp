// SPDX-License-Identifier: Apache-2.0
#include "DashboardGlyphs.hpp"
#include "ScriptedCellWidth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Cli;
using FastCache::Cli::Testing::FakeCellWidth;

namespace
{

/// The bytes of U+2581 LOWER ONE EIGHTH BLOCK, spelled as bytes so the assertion does not depend
/// on how this file's own literals were decoded.
constexpr std::string_view LowestBlock = "\xe2\x96\x81";

/// The bytes of U+2588 FULL BLOCK.
constexpr std::string_view FullBlock = "\xe2\x96\x88";

/// The code points of @p line, one string each.
/// @param line Valid UTF-8.
/// @return Each code point's bytes, in order.
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

/// The glyph at @p index of a sparkline.
/// @param line The sparkline.
/// @param index Which cell.
/// @return That cell's bytes, or empty past the end.
[[nodiscard]] std::string CellAt(std::string_view line, std::size_t index)
{
    auto const points = CodePoints(line);
    return index < points.size() ? points[index] : std::string {};
}

} // namespace

TEST_CASE("a gap and a real zero draw different sparkline cells", "[cli][dashboard][glyphs]")
{
    // §9.2, at the glyph. WHAT DISTINGUISHES: the MIDDLE cell differs between the two series, and
    // each is the glyph its meaning names -- a space for no reading, the lowest block for zero. A
    // renderer drawing both as `▁` passes "a gap renders" and fails here; one drawing both as a
    // space fails the zero half.
    auto const gapped = std::vector<std::optional<double>> { 10.0, std::nullopt, 10.0 };
    auto const zeroed = std::vector<std::optional<double>> { 10.0, 0.0, 10.0 };
    auto const& unicode = GlyphsFor(RenderRung::Unicode);

    auto const withGap = Sparkline(gapped, unicode);
    auto const withZero = Sparkline(zeroed, unicode);

    CHECK(CellAt(withGap, 1) != CellAt(withZero, 1));
    CHECK(CellAt(withGap, 1) == " ");
    CHECK(CellAt(withZero, 1) == LowestBlock);
    // The neighbours are identical, so the difference is the middle cell's and nobody else's.
    CHECK(CellAt(withGap, 0) == CellAt(withZero, 0));
    CHECK(CellAt(withGap, 2) == CellAt(withZero, 2));
}

TEST_CASE("the ASCII rung draws no sparkline at all rather than a bar of one level", "[cli][dashboard][glyphs]")
{
    // §10: one level per cell carries no information and looks like data. WHAT DISTINGUISHES: the
    // ASCII rung returns NOTHING for a series the Unicode rung draws, not a run of `#`.
    auto const series = std::vector<std::optional<double>> { 1.0, 5.0, std::nullopt, 9.0 };
    CHECK(Sparkline(series, GlyphsFor(RenderRung::Ascii)).empty());
    CHECK(!Sparkline(series, GlyphsFor(RenderRung::Unicode)).empty());
}

TEST_CASE("a sparkline is scaled from zero so a flat series keeps its height", "[cli][dashboard][glyphs]")
{
    // Scaled from its minimum, a flat 1000/s and a flat 0/s would draw alike. WHAT DISTINGUISHES:
    // both flat series in one case, drawing at opposite ends of the scale.
    auto const& unicode = GlyphsFor(RenderRung::Unicode);
    auto const busy = Sparkline(std::vector<std::optional<double>> { 1000.0, 1000.0 }, unicode);
    auto const idle = Sparkline(std::vector<std::optional<double>> { 0.0, 0.0 }, unicode);
    CHECK(busy == std::string { FullBlock } + std::string { FullBlock });
    CHECK(idle == std::string { LowestBlock } + std::string { LowestBlock });
}

TEST_CASE("a small present value never borrows the zero glyph", "[cli][dashboard][glyphs]")
{
    // `▁` claims zero. A value one thousandth of the largest is small, not zero, so it is at least
    // one level up -- and the control is a real zero in the same series, drawn at the bottom.
    auto const& unicode = GlyphsFor(RenderRung::Unicode);
    auto const line = Sparkline(std::vector<std::optional<double>> { 0.0, 1.0, 1000.0 }, unicode);
    CHECK(CellAt(line, 0) == LowestBlock);
    CHECK(CellAt(line, 1) != LowestBlock);
    CHECK(CellAt(line, 1) != " ");
    CHECK(CellAt(line, 2) == FullBlock);
}

TEST_CASE("a value that is not finite draws as no reading", "[cli][dashboard][glyphs]")
{
    auto const& unicode = GlyphsFor(RenderRung::Unicode);
    auto const line = Sparkline(std::vector<std::optional<double>> { std::numeric_limits<double>::quiet_NaN(),
                                                                     4.0,
                                                                     std::numeric_limits<double>::infinity() },
                                unicode);
    CHECK(CellAt(line, 0) == " ");
    CHECK(CellAt(line, 1) == FullBlock); // the infinity did not become the scale
    CHECK(CellAt(line, 2) == " ");
}

TEST_CASE("the sparkline levels are the eight block elements in order", "[cli][dashboard][glyphs]")
{
    // A wire-like constant: the NAME is what the code spells, the BYTES are what a terminal draws.
    // Pinned as bytes so a source decoded in another code page cannot pass by comparing with itself.
    auto const expected = std::vector<std::string_view> { "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                                          "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88" };
    REQUIRE(BlockLevels.size() == expected.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, expected.size()))
        CHECK(BlockLevels[index] == expected[index]);
}

TEST_CASE("the ASCII rung's glyphs are all ASCII", "[cli][dashboard][glyphs]")
{
    // The rung exists for a terminal that cannot draw anything else, so one stray UTF-8 byte in its
    // row is mojibake on exactly the terminal it serves.
    auto const& ascii = GlyphsFor(RenderRung::Ascii);
    auto const pieces =
        std::vector<std::string_view> { ascii.noReading,   ascii.horizontal,  ascii.vertical,   ascii.topLeft,
                                        ascii.topRight,    ascii.bottomRight, ascii.bottomLeft, ascii.gaugeOpen,
                                        ascii.gaugeFilled, ascii.gaugeHeld,   ascii.gaugeEmpty, ascii.gaugeClose };
    for (auto const piece: pieces)
        for (auto const byte: piece)
            CHECK(static_cast<unsigned char>(byte) < 0x80U);
}

TEST_CASE("an absent figure is the marker in every format", "[cli][dashboard][glyphs]")
{
    // The marker is the operator's `--absent`, so the one that is passed is the one that comes back
    // -- never a zero, never a format's own idea of blank.
    for (auto const format: std::views::iota(0, static_cast<int>(FigureFormat::Last)))
    {
        auto const which = static_cast<FigureFormat>(format);
        CHECK(FormatFigure(std::nullopt, which, "n/a") == "n/a");
        CHECK(FormatFigure(std::numeric_limits<double>::quiet_NaN(), which, "?") == "?");
        CHECK(FormatFigure(0.0, which, "n/a") != "n/a");
    }
}

TEST_CASE("figures are written in their own units", "[cli][dashboard][glyphs]")
{
    CHECK(FormatFigure(1284991.0, FigureFormat::Count, "-") == "1 284 991");
    CHECK(FormatFigure(0.0, FigureFormat::Count, "-") == "0");
    CHECK(FormatFigure(999.0, FigureFormat::Count, "-") == "999");
    CHECK(FormatFigure(1000.0, FigureFormat::Count, "-") == "1 000");
    // A uint64 counter beyond what an integer rounding may return, written whole rather than wrapped.
    CHECK(FormatFigure(18446744073709551615.0, FigureFormat::Count, "-") == "18 446 744 073 709 551 616");

    CHECK(FormatFigure(12480.0, FigureFormat::Rate, "-") == "12 480");
    CHECK(FormatFigure(7.5, FigureFormat::Rate, "-") == "7.5");
    CHECK(FormatFigure(0.942, FigureFormat::Percent, "-") == "94.2 %");
    CHECK(FormatFigure(512.0, FigureFormat::Bytes, "-") == "512 B");
    CHECK(FormatFigure(3.41 * 1024 * 1024 * 1024, FigureFormat::Bytes, "-") == "3.41 GiB");
    CHECK(FormatFigure(248.1 * 1024 * 1024, FigureFormat::Bytes, "-") == "248.1 MiB");
    CHECK(FormatFigure(1.84, FigureFormat::Seconds, "-") == "1.84 s");
    // A lease out for an hour and a half is read in hours, never as thousands of seconds.
    CHECK(FormatFigure(90.0, FigureFormat::Seconds, "-") == "1.5 min");
    CHECK(FormatFigure(5400.0, FigureFormat::Seconds, "-") == "1.5 h");
}

TEST_CASE("every frame line is exactly the frame's width on both text rungs", "[cli][dashboard][glyphs]")
{
    // A frame line one column short or long pushes its right edge, and the next frame drawn over it
    // leaves the old edge behind. WHAT DISTINGUISHES: a line longer than the frame is CUT, one
    // shorter is PADDED, and a multi-byte line is measured in columns rather than bytes -- on both
    // rungs, whose glyphs differ in byte length.
    constexpr auto Width = std::size_t { 24 };
    auto const lines = std::vector<std::string> { "short",
                                                  "a line far too long to fit inside this frame",
                                                  "\xe2\x96\x81\xe2\x96\x88 blocks" };

    for (auto const rung: { RenderRung::Unicode, RenderRung::Ascii })
    {
        auto const frame = Frame("a title that is also far too long", lines, Width, GlyphsFor(rung), &FakeCellWidth);
        auto rows = std::size_t { 0 };
        for (auto const row: std::views::split(std::string_view { frame }, '\n'))
        {
            CHECK(FakeCellWidth(std::string_view { row.begin(), row.end() }) == Width);
            ++rows;
        }
        CHECK(rows == lines.size() + 2);
    }
}

TEST_CASE("fitting text never splits a code point", "[cli][dashboard][glyphs]")
{
    auto const fitted = FitRight("\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83", 2, &FakeCellWidth);
    CHECK(fitted == "\xe2\x96\x81\xe2\x96\x82");
    CHECK(FitRight("ab", 4, &FakeCellWidth) == "ab  ");
    CHECK(AlignRight("42", 5, &FakeCellWidth) == "   42");
    CHECK(AlignRight("123456", 3, &FakeCellWidth) == "123456"); // a figure is never cut
}

TEST_CASE("fitting text measures cells, not bytes or code points", "[cli][dashboard][glyphs]")
{
    // A hostname can be wide. WHAT DISTINGUISHES: a two-cell character that would straddle the edge
    // is left out and its half-cell padded, so the result is exactly the width; a zero-width mark
    // costs nothing, so it stays with the character it modifies; and a byte or code-point count
    // gives a different answer for each of the three lines below.
    auto const wideA = std::string { "\xef\xbc\xa1" };  // U+FF21 FULLWIDTH LATIN CAPITAL A, two cells
    auto const wideB = std::string { "\xef\xbc\xa2" };  // U+FF22, two cells
    auto const combining = std::string { "e\xcc\x81" }; // e + U+0301, one cell
    CHECK(FitRight(wideA + wideB + "c", 3, &FakeCellWidth) == wideA + " ");
    CHECK(FakeCellWidth(FitRight(wideA + wideB + "c", 3, &FakeCellWidth)) == 3);
    CHECK(FitRight(combining + "x", 2, &FakeCellWidth) == combining + "x");
    CHECK(AlignRight(wideA, 4, &FakeCellWidth) == "  " + wideA);
}

TEST_CASE("a slot gauge draws what runs, what is available beside it, and what a limit withdrew", "[cli][dashboard][glyphs]")
{
    // WHAT DISTINGUISHES: three runs of three glyphs whose lengths are the three shares, so a gauge drawing
    // `available` from the left edge, or folding withdrawn into held, reads a different string.
    auto const& ascii = GlyphsFor(RenderRung::Ascii);
    auto const joined = [](SlotGauge const& gauge) {
        return gauge.open + gauge.running + gauge.held + gauge.withdrawn + gauge.close;
    };
    CHECK(joined(SlotGaugeOf(6, 12, 16, 16, ascii)) == "[######======....]");
    // Nothing withdrawn: the node offers every slot it registered.
    CHECK(joined(SlotGaugeOf(3, 8, 8, 8, ascii)) == "[###=====]");
    // More running than is available now (a limit arrived under running work): no held cells, never fewer running.
    auto const over = SlotGaugeOf(6, 2, 16, 16, ascii);
    CHECK(over.running == "######");
    CHECK(over.held.empty());
    CHECK(over.withdrawn == "..........");
    // A node registered with no slots draws its whole width withdrawn rather than dividing by zero.
    CHECK(joined(SlotGaugeOf(0, 0, 0, 4, ascii)) == "[....]");
    // The width is kept on the Unicode rung, each glyph one cell.
    auto const unicode = SlotGaugeOf(6, 12, 16, 16, GlyphsFor(RenderRung::Unicode));
    CHECK(FakeCellWidth(unicode.running + unicode.held + unicode.withdrawn) == 16);
    CHECK(unicode.held == "▒▒▒▒▒▒");
}

TEST_CASE("a gauge fills in proportion and keeps its width", "[cli][dashboard][glyphs]")
{
    auto const& ascii = GlyphsFor(RenderRung::Ascii);
    CHECK(Gauge(0.5, 10, ascii) == "[#####.....]");
    CHECK(Gauge(-1.0, 4, ascii) == "[....]");
    CHECK(Gauge(7.0, 4, ascii) == "[####]");
    CHECK(FakeCellWidth(Gauge(0.853, 20, GlyphsFor(RenderRung::Unicode))) == 20);
}

// SPDX-License-Identifier: Apache-2.0
#include "DashboardGlyphs.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace FastCache::Cli
{

namespace
{
    /// Whether @p byte continues a UTF-8 sequence rather than starting one.
    /// @param byte The byte.
    /// @return True for `10xxxxxx`.
    [[nodiscard]] constexpr bool IsContinuation(char byte) noexcept
    {
        return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
    }

    /// @p text repeated @p count times.
    /// @param text The piece.
    /// @param count How many.
    /// @return The run.
    [[nodiscard]] std::string Repeat(std::string_view text, std::size_t count)
    {
        auto run = std::string {};
        run.reserve(text.size() * count);
        std::ranges::for_each(std::views::iota(std::size_t { 0 }, count), [&](std::size_t) { run.append(text); });
        return run;
    }

    /// The level @p value draws at, given the largest present value.
    /// @param value A present, finite value.
    /// @param largest The largest present value in the series.
    /// @param levels How many levels the rung has; at least two.
    /// @return The level's index.
    [[nodiscard]] std::size_t LevelOf(double value, double largest, std::size_t levels) noexcept
    {
        if (value <= 0.0 || largest <= 0.0)
            return 0;
        auto const above = static_cast<std::size_t>(std::floor((value / largest) * static_cast<double>(levels - 2)));
        return std::min(levels - 1, 1 + above);
    }

} // namespace

RungGlyphs const& GlyphsFor(RenderRung rung) noexcept
{
    return RungGlyphTable[static_cast<std::size_t>(rung)];
}

std::string FitRight(std::string_view text, std::size_t width, CellWidth cellWidth)
{
    // The longest prefix, ending on a code-point boundary, that the width function measures within
    // `width`. Measured as a PREFIX rather than summed per code point, so whatever the function knows
    // about clusters -- a combining mark adds nothing, a joined emoji is two -- is what decides.
    auto kept = std::size_t { 0 };
    auto keptWidth = std::size_t { 0 };
    for (auto const end: std::views::iota(std::size_t { 1 }, text.size() + 1))
    {
        if (end < text.size() && IsContinuation(text[end]))
            continue;
        auto const prefixWidth = cellWidth(text.substr(0, end));
        if (prefixWidth > width)
            break;
        kept = end;
        keptWidth = prefixWidth;
    }
    auto fitted = std::string { text.substr(0, kept) };
    fitted.append(width - keptWidth, ' ');
    return fitted;
}

std::string AlignRight(std::string_view text, std::size_t width, CellWidth cellWidth)
{
    auto const cells = cellWidth(text);
    if (cells >= width)
        return std::string { text };
    return std::string(width - cells, ' ') + std::string { text };
}

std::string Sparkline(std::span<std::optional<double> const> cells, RungGlyphs const& glyphs)
{
    if (glyphs.sparkLevels.size() < 2)
        return {};

    auto largest = 0.0;
    for (auto const& cell: cells)
        if (cell.has_value() && std::isfinite(*cell))
            largest = std::max(largest, *cell);

    auto line = std::string {};
    for (auto const& cell: cells)
    {
        if (!cell.has_value() || !std::isfinite(*cell))
            line.append(glyphs.noReading);
        else
            line.append(glyphs.sparkLevels[LevelOf(*cell, largest, glyphs.sparkLevels.size())]);
    }
    return line;
}

std::string Gauge(double fraction, std::size_t width, RungGlyphs const& glyphs)
{
    auto const clamped = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0.0;
    auto const filled = std::min(width, static_cast<std::size_t>(std::lround(clamped * static_cast<double>(width))));
    return std::string { glyphs.gaugeOpen } + Repeat(glyphs.gaugeFilled, filled) + Repeat(glyphs.gaugeEmpty, width - filled)
           + std::string { glyphs.gaugeClose };
}

SlotGauge SlotGaugeOf(
    std::uint32_t inFlight, std::uint32_t available, std::uint32_t registered, std::size_t width, RungGlyphs const& glyphs)
{
    // Cells for @p slots of the registered ones, rounded, so a gauge of the same width reads the same share alike.
    auto const cells = [registered, width](std::uint32_t slots) {
        if (registered == 0)
            return std::size_t { 0 };
        auto const share = static_cast<double>(std::min(slots, registered)) / static_cast<double>(registered);
        return std::min(width, static_cast<std::size_t>(std::lround(share * static_cast<double>(width))));
    };
    auto const running = cells(inFlight);
    auto const offered = std::max(running, cells(available));
    return SlotGauge { .open = std::string { glyphs.gaugeOpen },
                       .running = Repeat(glyphs.gaugeFilled, running),
                       .held = Repeat(glyphs.gaugeHeld, offered - running),
                       .withdrawn = Repeat(glyphs.gaugeEmpty, width - offered),
                       .close = std::string { glyphs.gaugeClose } };
}

WrittenFigure FormatFigure(std::optional<double> value, FigureFormat format, std::string_view absent)
{
    if (!value.has_value() || !std::isfinite(*value))
        return WrittenFigure { .number = std::string { absent } };
    return WriteFigure(*value, format);
}

std::string Frame(std::string_view title,
                  std::span<std::string const> lines,
                  std::size_t width,
                  RungGlyphs const& glyphs,
                  CellWidth cellWidth)
{
    return Frame(title, {}, lines, width, glyphs, cellWidth);
}

std::string Frame(std::string_view title,
                  std::string_view trailing,
                  std::span<std::string const> lines,
                  std::size_t width,
                  RungGlyphs const& glyphs,
                  CellWidth cellWidth)
{
    auto const inside = std::max<std::size_t>(width, 4) - 2;

    // `┌─ title ─── trailing ─┐`: one edge glyph, the title padded by a space each side, fill, the trailing
    // half padded the same way and one edge glyph before the corner. A title too long for the frame is cut,
    // never allowed to push the corner out; the trailing half is left off before that happens.
    auto const label = title.empty() ? std::string {} : std::format(" {} ", title);
    auto const right = trailing.empty() ? std::string {} : std::format(" {} ", trailing);
    auto const rightFits = !right.empty() && cellWidth(label) + cellWidth(right) + 3 <= inside;
    auto const rightCells = rightFits ? cellWidth(right) + 1 : 0;
    auto const shownLabel = FitRight(label, std::min(cellWidth(label), inside - 1 - rightCells), cellWidth);
    auto frame = std::string { glyphs.topLeft } + std::string { glyphs.horizontal } + shownLabel
                 + Repeat(glyphs.horizontal, inside - 1 - cellWidth(shownLabel) - rightCells);
    if (rightFits)
        frame += right + std::string { glyphs.horizontal };
    frame += std::string { glyphs.topRight } + "\n";

    // One blank column is kept before the right edge, so no content ever touches it: a figure
    // written up against the edge reads as one word with it, to a person and to a script alike.
    for (auto const& line: lines)
        frame += std::string { glyphs.vertical } + FitRight(line, ContentColumns(width), cellWidth) + " "
                 + std::string { glyphs.vertical } + "\n";

    frame += std::string { glyphs.bottomLeft } + Repeat(glyphs.horizontal, inside) + std::string { glyphs.bottomRight };
    return frame;
}

} // namespace FastCache::Cli

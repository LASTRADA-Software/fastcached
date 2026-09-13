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

    /// @p value rounded to a whole number and grouped in thousands with a space, which reads in
    /// every locale and is never mistaken for a decimal separator.
    ///
    /// Rounded by FORMATTING rather than through an integer: a counter is a `uint64_t` and can
    /// exceed what `llround` may return, which is unspecified rather than an error.
    /// @param value A finite number.
    /// @return The grouped text.
    [[nodiscard]] std::string Grouped(double value)
    {
        auto const whole = std::format("{:.0f}", std::fabs(value));
        auto grouped = std::string {};
        grouped.reserve(whole.size() + (whole.size() / 3) + 1);
        if (std::signbit(value) && whole != "0")
            grouped.push_back('-');
        for (auto const index: std::views::iota(std::size_t { 0 }, whole.size()))
        {
            if (index != 0 && (whole.size() - index) % 3 == 0)
                grouped.push_back(' ');
            grouped.push_back(whole[index]);
        }
        return grouped;
    }

    /// One binary unit.
    struct ByteUnit
    {
        double scale;          ///< Bytes per unit.
        std::string_view name; ///< What it is called.
    };

    /// The units a byte figure may be written in, largest first.
    constexpr auto ByteUnits = std::array {
        ByteUnit { .scale = 1024.0 * 1024.0 * 1024.0 * 1024.0, .name = "TiB" },
        ByteUnit { .scale = 1024.0 * 1024.0 * 1024.0, .name = "GiB" },
        ByteUnit { .scale = 1024.0 * 1024.0, .name = "MiB" },
        ByteUnit { .scale = 1024.0, .name = "KiB" },
    };

    /// A byte count in the largest unit it fills at least once.
    /// @param value Bytes.
    /// @return The text.
    [[nodiscard]] std::string Bytes(double value)
    {
        for (auto const& unit: ByteUnits)
        {
            auto const scaled = value / unit.scale;
            if (scaled >= 1.0)
                return scaled < 100.0 ? std::format("{:.2f} {}", scaled, unit.name)
                                      : std::format("{:.1f} {}", scaled, unit.name);
        }
        return std::format("{:.0f} B", value);
    }

    /// How one figure format writes a present, finite value.
    struct FigureFormatSpec
    {
        FigureFormat format;                ///< The enumerator this row describes.
        std::string (*write)(double value); ///< The writer.
    };

    /// The figure formats, one row per enumerator, in enumerator order.
    constexpr EnumTable<FigureFormat, FigureFormatSpec> FigureFormatTable { {
        { .format = FigureFormat::Count, .write = [](double value) { return Grouped(value); } },
        { .format = FigureFormat::Rate,
          .write = [](double value) { return std::fabs(value) >= 10.0 ? Grouped(value) : std::format("{:.1f}", value); } },
        { .format = FigureFormat::Percent, .write = [](double value) { return std::format("{:.1f} %", value * 100.0); } },
        { .format = FigureFormat::Bytes, .write = &Bytes },
        { .format = FigureFormat::Seconds, .write = [](double value) { return std::format("{:.2f} s", value); } },
    } };

    static_assert(RowsInEnumeratorOrder(FigureFormatTable, &FigureFormatSpec::format),
                  "FigureFormatTable must hold one row per FigureFormat, in enumerator order");

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

std::size_t DisplayWidth(std::string_view text) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char byte) { return !IsContinuation(byte); }));
}

std::string FitRight(std::string_view text, std::size_t width)
{
    auto fitted = std::string {};
    fitted.reserve(text.size() + width);
    auto columns = std::size_t { 0 };
    for (auto const byte: text)
    {
        if (!IsContinuation(byte))
        {
            if (columns == width)
                break;
            ++columns;
        }
        fitted.push_back(byte);
    }
    fitted.append(width - columns, ' ');
    return fitted;
}

std::string AlignRight(std::string_view text, std::size_t width)
{
    auto const columns = DisplayWidth(text);
    if (columns >= width)
        return std::string { text };
    return std::string(width - columns, ' ') + std::string { text };
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

std::string FormatFigure(std::optional<double> value, FigureFormat format, std::string_view absent)
{
    if (!value.has_value() || !std::isfinite(*value))
        return std::string { absent };
    return FigureFormatTable[static_cast<std::size_t>(format)].write(*value);
}

std::string Frame(std::string_view title, std::span<std::string const> lines, std::size_t width, RungGlyphs const& glyphs)
{
    auto const inside = std::max<std::size_t>(width, 4) - 2;

    // `┌─ title ───┐`: one edge glyph, the title padded by a space each side, then edge to the
    // corner. A title too long for the frame is cut, never allowed to push the corner out.
    auto const label = title.empty() ? std::string {} : std::format(" {} ", title);
    auto const shownLabel = FitRight(label, std::min(DisplayWidth(label), inside - 1));
    auto frame = std::string { glyphs.topLeft } + std::string { glyphs.horizontal } + shownLabel
                 + Repeat(glyphs.horizontal, inside - 1 - DisplayWidth(shownLabel)) + std::string { glyphs.topRight } + "\n";

    for (auto const& line: lines)
        frame += std::string { glyphs.vertical } + FitRight(line, inside) + std::string { glyphs.vertical } + "\n";

    frame += std::string { glyphs.bottomLeft } + Repeat(glyphs.horizontal, inside) + std::string { glyphs.bottomRight };
    return frame;
}

} // namespace FastCache::Cli

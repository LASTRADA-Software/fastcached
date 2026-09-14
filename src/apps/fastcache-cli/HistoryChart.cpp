// SPDX-License-Identifier: Apache-2.0
#include "HistoryChart.hpp"

#include <FastCache/Core/Ranges.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    /// One colour of the chart's ramp.
    struct RampColour
    {
        std::uint8_t red;   ///< Red.
        std::uint8_t green; ///< Green.
        std::uint8_t blue;  ///< Blue.
    };

    /// Cold to hot, in eight steps: few enough that a 16-colour palette holds every one exactly, so the
    /// encoder's quantizer never merges two steps a reader is meant to tell apart.
    constexpr auto Ramp = std::to_array<RampColour>({
        { .red = 0x1f, .green = 0x3b, .blue = 0x73 },
        { .red = 0x25, .green = 0x6d, .blue = 0x9e },
        { .red = 0x2a, .green = 0x9d, .blue = 0x8f },
        { .red = 0x5c, .green = 0xb8, .blue = 0x5c },
        { .red = 0xb5, .green = 0xc9, .blue = 0x3a },
        { .red = 0xe9, .green = 0xc4, .blue = 0x3a },
        { .red = 0xe9, .green = 0x7f, .blue = 0x2a },
        { .red = 0xd6, .green = 0x3a, .blue = 0x2f },
    });

    /// The track under every reading: a neutral grey, so a reading of zero is visible and is no step of
    /// the ramp. With the ramp's eight it is nine colours, inside the 16 a chart is encoded with.
    constexpr auto Track = RampColour { .red = 0x5f, .green = 0x66, .blue = 0x73 };

    /// A plain band's bars: a light slate, no step of the ramp and brighter than the track, so a plain bar is never
    /// read as a share on the scale nor lost against the track. Ten colours with the ramp and the track.
    constexpr auto PlainBar = RampColour { .red = 0xb4, .green = 0xbe, .blue = 0xcc };

    /// A band this many pixels tall or taller leaves its top row transparent, between it and the band above.
    constexpr auto BandGapFrom = std::size_t { 4 };

    /// What a text chart's cell holds where nothing is drawn.
    constexpr std::string_view BlankCell = " ";

    /// Paint one pixel of @p raster opaque.
    /// @param raster The raster.
    /// @param x Column; inside the raster.
    /// @param y Row; inside the raster.
    /// @param colour The colour.
    void Paint(ChartRaster& raster, std::size_t x, std::size_t y, RampColour colour) noexcept
    {
        auto const at = ((y * raster.width) + x) * 4;
        raster.rgba[at] = colour.red;
        raster.rgba[at + 1] = colour.green;
        raster.rgba[at + 2] = colour.blue;
        raster.rgba[at + 3] = 0xff;
    }

    /// The ramp colour a share stands for.
    /// @param share In [0, 1].
    /// @return The colour.
    [[nodiscard]] RampColour ColourOf(double share) noexcept
    {
        auto const step = std::min(Ramp.size() - 1, static_cast<std::size_t>(share * static_cast<double>(Ramp.size())));
        return Ramp[step];
    }
} // namespace

std::size_t ChartWindowFor(std::size_t samples) noexcept
{
    auto const* const window = FindIfOrNull(ChartWindows, [samples](std::size_t span) { return span >= samples; });
    return window != nullptr ? *window : ChartWindows.back();
}

std::vector<std::optional<double>> SharesOf(std::span<std::optional<double> const> values, double top)
{
    auto shares = std::vector<std::optional<double>> {};
    shares.reserve(values.size());
    for (auto const& value: values)
    {
        if (!value.has_value() || !std::isfinite(*value))
            shares.emplace_back();
        else
            shares.emplace_back(top > 0.0 ? std::clamp(*value / top, 0.0, 1.0) : 0.0);
    }
    return shares;
}

std::optional<double> PeakOf(std::span<std::optional<double> const> values) noexcept
{
    auto peak = std::optional<double> {};
    for (auto const& value: values)
        if (value.has_value() && std::isfinite(*value))
            peak = std::max(peak.value_or(*value), *value);
    return peak;
}

ChartRaster ChartBandsRaster(std::span<ChartTrack const> tracks, std::size_t window, std::size_t width, std::size_t height)
{
    auto raster = ChartRaster { .rgba = std::vector<std::uint8_t>(width * height * 4, 0), .width = width, .height = height };
    if (width == 0 || height == 0 || tracks.empty() || window == 0)
        return raster;

    auto const sampleWidth = std::max<std::size_t>(1, width / window);
    auto const bandHeight = std::max<std::size_t>(1, height / tracks.size());
    auto const gap = bandHeight >= BandGapFrom ? std::size_t { 1 } : std::size_t { 0 };
    auto const inner = bandHeight - gap;
    for (auto const band: std::views::iota(std::size_t { 0 }, tracks.size()))
    {
        auto const floor = (band + 1) * bandHeight;
        if (floor > height)
            break;
        // The newest shares the width holds, right-aligned: the oldest of them `shown` sample widths from the edge.
        auto const& shares = tracks[band].shares;
        auto const shown = std::min({ shares.size(), window, width / sampleWidth });
        for (auto const sample: std::views::iota(std::size_t { 0 }, shown))
        {
            auto const& share = shares[shares.size() - shown + sample];
            if (!share.has_value())
                continue;
            auto const left = width - ((shown - sample) * sampleWidth);
            auto const fraction = std::isfinite(*share) ? std::clamp(*share, 0.0, 1.0) : 0.0;
            auto bar = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(inner)));
            if (fraction > 0.0)
                bar = std::max<std::size_t>(bar, 1);
            auto const colour = tracks[band].paint == ChartPaint::Ramp ? ColourOf(fraction) : PlainBar;
            for (auto const y: std::views::iota(floor - inner, floor))
                for (auto const x: std::views::iota(left, left + sampleWidth))
                    Paint(raster, x, y, y >= floor - bar ? colour : Track);
        }
    }
    return raster;
}

ChartRaster ChartScaleRaster(std::size_t width, std::size_t height)
{
    auto raster = ChartRaster { .rgba = std::vector<std::uint8_t>(width * height * 4, 0), .width = width, .height = height };
    for (auto const x: std::views::iota(std::size_t { 0 }, width))
    {
        auto const& colour = Ramp[std::min(Ramp.size() - 1, (x * Ramp.size()) / width)];
        for (auto const y: std::views::iota(std::size_t { 0 }, height))
            Paint(raster, x, y, colour);
    }
    return raster;
}

std::vector<std::string> ChartTextRows(std::span<std::optional<double> const> shares,
                                       std::size_t rows,
                                       std::size_t cells,
                                       std::span<std::string_view const> levels)
{
    if (levels.size() < 2 || rows == 0)
        return {};
    auto lines = std::vector<std::string>(rows);
    auto const perRow = levels.size();
    auto const shown = std::min(shares.size(), cells);
    for (auto& line: lines)
        for ([[maybe_unused]] auto const cell: std::views::iota(std::size_t { 0 }, cells - shown))
            line.append(BlankCell);

    for (auto const sample: std::views::iota(std::size_t { 0 }, shown))
    {
        auto const& share = shares[shares.size() - shown + sample];
        // How many levels of the whole band the bar fills, every row's together; a share above zero fills at least
        // two, so its floor cell is a level above the zero mark.
        auto filled = std::optional<std::size_t> {};
        if (share.has_value() && std::isfinite(*share))
        {
            auto const fraction = std::clamp(*share, 0.0, 1.0);
            filled = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(rows * perRow)));
            if (fraction > 0.0)
                filled = std::max<std::size_t>(*filled, 2);
        }
        for (auto const row: std::views::iota(std::size_t { 0 }, rows))
        {
            // Row 0 is the floor; lines are written top first.
            auto& line = lines[rows - 1 - row];
            if (!filled.has_value())
            {
                line.append(BlankCell);
                continue;
            }
            auto const below = row * perRow;
            if (*filled == 0)
                line.append(row == 0 ? levels.front() : BlankCell);
            else if (*filled >= below + perRow)
                line.append(levels.back());
            else if (*filled > below)
                line.append(levels[*filled - below - 1]);
            else
                line.append(BlankCell);
        }
    }
    return lines;
}

} // namespace FastCache::Cli

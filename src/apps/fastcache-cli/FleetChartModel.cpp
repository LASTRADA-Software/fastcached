// SPDX-License-Identifier: Apache-2.0
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"

#include <FastCache/Core/NumericText.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
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

    /// A band this many pixels tall or taller leaves its top row transparent, between it and the band above.
    constexpr auto BandGapFrom = std::size_t { 4 };

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

    /// The ramp colour a scaled value stands for.
    /// @param value The scaled value.
    /// @param full The value the hottest colour stands for.
    /// @return The colour.
    [[nodiscard]] RampColour ColourOf(double value, double full) noexcept
    {
        auto const fraction = full > 0.0 && std::isfinite(value) ? std::clamp(value / full, 0.0, 1.0) : 0.0;
        auto const step = std::min(Ramp.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(Ramp.size())));
        return Ramp[step];
    }

    /// Where the column named @p name is in @p table.
    /// @param table The table.
    /// @param name The column's name.
    /// @return Its index, or nullopt.
    [[nodiscard]] std::optional<std::size_t> ColumnOf(Value const& table, std::string_view name)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, table.columns.size()))
            if (table.columns[index] == name)
                return index;
        return std::nullopt;
    }
} // namespace

std::vector<SeriesPoint> FleetChartPoints(FleetDocument const& document)
{
    auto points = std::vector<SeriesPoint> {};
    for (auto const& metric: FleetChartMetrics)
    {
        auto const* table = document.Section(metric.section);
        if (table == nullptr)
            continue;
        auto const subjectAt = ColumnOf(*table, metric.subjectColumn);
        auto const valueAt = ColumnOf(*table, metric.valueColumn);
        if (!subjectAt.has_value() || !valueAt.has_value())
            continue;
        auto const format = Distributed::FleetColumnFormat(metric.section, metric.valueColumn);
        auto const scale = format.has_value() ? Distributed::CellFormatTable[static_cast<std::size_t>(*format)].scale : 1.0;
        for (auto const& row: table->rows)
        {
            auto const& subject = row[*subjectAt];
            auto const& cell = row[*valueAt];
            auto raw = 0.0;
            if (subject.kind == CellKind::Absent || cell.kind == CellKind::Absent || !ParseFiniteDouble(cell.lexical, raw))
                continue;
            points.push_back(SeriesPoint { .series = metric.key, .subject = subject.lexical, .value = raw * scale });
        }
    }
    return points;
}

std::size_t ChartWindowFor(std::size_t samples) noexcept
{
    auto const* const window = FindIfOrNull(ChartWindows, [samples](std::size_t span) { return span >= samples; });
    return window != nullptr ? *window : ChartWindows.back();
}

std::vector<ChartBand> FleetChartBands(std::deque<HistoryEntry> const& history,
                                       FleetChartMetric const& metric,
                                       std::size_t window)
{
    auto found = std::map<std::string, std::optional<double>> {};
    auto const shown = std::min(history.size(), window);
    for (auto const index: std::views::iota(history.size() - shown, history.size()))
        for (auto const& point: history[index].points)
            if (point.series == metric.key)
                found.try_emplace(point.subject);
    if (!history.empty())
        for (auto const& point: history.back().points)
            if (point.series == metric.key)
                found[point.subject] = point.value;

    auto bands = std::vector<ChartBand> {};
    bands.reserve(found.size());
    for (auto& [subject, latest]: found)
        bands.push_back(ChartBand { .subject = subject, .latest = latest });
    return bands;
}

ChartRaster FleetChartRaster(std::deque<HistoryEntry> const& history,
                             FleetChartMetric const& metric,
                             std::span<ChartBand const> bands,
                             std::size_t window,
                             std::size_t width,
                             std::size_t height)
{
    auto raster = ChartRaster { .rgba = std::vector<std::uint8_t>(width * height * 4, 0), .width = width, .height = height };
    if (width == 0 || height == 0 || history.empty() || bands.empty() || window == 0)
        return raster;

    // The newest samples the width holds, right-aligned: the oldest of them `shown` sample widths from the edge.
    auto const sampleWidth = std::max<std::size_t>(1, width / window);
    auto const shown = std::min({ history.size(), window, width / sampleWidth });
    auto const bandHeight = std::max<std::size_t>(1, height / bands.size());
    auto const gap = bandHeight >= BandGapFrom ? std::size_t { 1 } : std::size_t { 0 };
    auto const inner = bandHeight - gap;
    for (auto const sample: std::views::iota(std::size_t { 0 }, shown))
    {
        auto const& entry = history[history.size() - shown + sample];
        auto const left = width - ((shown - sample) * sampleWidth);
        for (auto const band: std::views::iota(std::size_t { 0 }, bands.size()))
        {
            auto const floor = (band + 1) * bandHeight;
            if (floor > height)
                break;
            auto const* const point = FindIfOrNull(entry.points, [&](SeriesPoint const& candidate) {
                return candidate.series == metric.key && candidate.subject == bands[band].subject;
            });
            if (point == nullptr)
                continue;
            auto const fraction =
                metric.full > 0.0 && std::isfinite(point->value) ? std::clamp(point->value / metric.full, 0.0, 1.0) : 0.0;
            auto bar = static_cast<std::size_t>(std::lround(fraction * static_cast<double>(inner)));
            if (fraction > 0.0)
                bar = std::max<std::size_t>(bar, 1);
            auto const colour = ColourOf(point->value, metric.full);
            for (auto const y: std::views::iota(floor - inner, floor))
                for (auto const x: std::views::iota(left, left + sampleWidth))
                    Paint(raster, x, y, y >= floor - bar ? colour : Track);
        }
    }
    return raster;
}

ChartRaster FleetChartScale(std::size_t width, std::size_t height)
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

} // namespace FastCache::Cli

// SPDX-License-Identifier: Apache-2.0
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"

#include <FastCache/Core/NumericText.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <ranges>
#include <string>

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
        for (auto const& row: table->rows)
        {
            auto const& subject = row[*subjectAt];
            auto const& cell = row[*valueAt];
            auto raw = 0.0;
            if (subject.kind == CellKind::Absent || cell.kind == CellKind::Absent || !ParseFiniteDouble(cell.lexical, raw))
                continue;
            points.push_back(SeriesPoint { .series = metric.key, .subject = subject.lexical, .value = raw * metric.scale });
        }
    }
    return points;
}

ChartRaster FleetChartRaster(std::deque<HistoryEntry> const& history,
                             FleetChartMetric const& metric,
                             std::size_t width,
                             std::size_t height)
{
    auto raster = ChartRaster { .rgba = std::vector<std::uint8_t>(width * height * 4, 0), .width = width, .height = height };
    if (width == 0 || height == 0 || history.empty())
        return raster;

    // The newest samples that fit, oldest first, each as its own machine-to-value map.
    auto const shown = std::min(history.size(), width);
    auto samples = std::vector<std::map<std::string, double>>(shown);
    auto bands = std::map<std::string, std::size_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, shown))
        for (auto const& point: history[history.size() - shown + index].points)
            if (point.series == metric.key)
            {
                samples[index][point.subject] = point.value;
                bands.try_emplace(point.subject, 0);
            }
    if (bands.empty())
        return raster;

    // Bands in key order, so a machine keeps its band as others come and go.
    auto next = std::size_t { 0 };
    for (auto& [subject, band]: bands)
        band = next++;

    auto const bandHeight = std::max<std::size_t>(1, height / bands.size());
    auto const sampleWidth = std::max<std::size_t>(1, width / shown);
    auto const left = width - std::min(width, sampleWidth * shown); // the newest sample ends at the right edge
    for (auto const sample: std::views::iota(std::size_t { 0 }, shown))
        for (auto const& [subject, value]: samples[sample])
        {
            auto const band = bands.at(subject);
            auto const colour = ColourOf(value, metric.full);
            for (auto const y: std::views::iota(band * bandHeight, std::min(height, (band + 1) * bandHeight)))
                for (auto const x:
                     std::views::iota(left + (sample * sampleWidth), std::min(width, left + ((sample + 1) * sampleWidth))))
                {
                    auto const at = ((y * width) + x) * 4;
                    raster.rgba[at] = colour.red;
                    raster.rgba[at + 1] = colour.green;
                    raster.rgba[at + 2] = colour.blue;
                    raster.rgba[at + 3] = 0xff;
                }
        }
    return raster;
}

} // namespace FastCache::Cli

// SPDX-License-Identifier: Apache-2.0
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"

#include <FastCache/Core/NumericText.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache::Cli
{

namespace
{
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

std::vector<ChartTrack> FleetChartTracks(std::deque<HistoryEntry> const& history,
                                         FleetChartMetric const& metric,
                                         std::span<ChartBand const> bands,
                                         std::size_t window)
{
    auto tracks = std::vector<ChartTrack> {};
    tracks.reserve(bands.size());
    auto const shown = std::min(history.size(), window);
    for (auto const& band: bands)
    {
        auto values = std::vector<std::optional<double>> {};
        values.reserve(shown);
        for (auto const index: std::views::iota(history.size() - shown, history.size()))
        {
            auto const* const point = core::findIfOrNull(history[index].points, [&](SeriesPoint const& candidate) {
                return candidate.series == metric.key && candidate.subject == band.subject;
            });
            values.push_back(point == nullptr ? std::nullopt : std::optional { point->value });
        }
        tracks.push_back(
            ChartTrack { .label = band.subject, .latest = {}, .top = {}, .shares = SharesOf(values, metric.full) });
    }
    return tracks;
}

ChartRaster FleetChartRaster(std::deque<HistoryEntry> const& history,
                             FleetChartMetric const& metric,
                             std::span<ChartBand const> bands,
                             std::size_t window,
                             std::size_t width,
                             std::size_t height)
{
    return ChartBandsRaster(FleetChartTracks(history, metric, bands, window), window, width, height);
}

} // namespace FastCache::Cli

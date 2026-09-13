// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardLoop.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

struct FleetDocument;

/// @file FleetChartModel.hpp
/// The fleet chart as data: which per-machine figure it draws, the points a reading keeps for it,
/// and the pixels a window of history becomes.
///
/// **The Sixel rung's one image** (#134, the decision on Sixel): 40 machines across a time window
/// does not fit in text cells, so this is the figure drawn at pixel resolution, and the cache and node
/// panels stay on their sparklines. Pure: points in, pixels out, so every rule below is a case over
/// literals, and the encoder that turns pixels into Sixel stays behind `ISixelEncoder`.

/// One per-machine figure the fleet chart can draw.
///
/// **A row, so a second figure is a row rather than a branch.** Its columns are named here because
/// the chart needs one figure per machine and the document carries dozens: a test requires both
/// columns to be ones the leader's own renderer writes (`FleetColumnNames`), so a column renamed there
/// fails here instead of drawing an empty chart.
struct FleetChartMetric
{
    std::string_view key;              ///< The series its points carry; static storage.
    Distributed::FleetSection section; ///< The table the figure is read from.
    std::string_view subjectColumn;    ///< The column naming the machine a row describes.
    std::string_view valueColumn;      ///< The column holding the figure.
    double scale { 1.0 };              ///< Applied to the raw cell: `cpu-busy` travels in thousandths.
    double full { 1.0 };               ///< The scaled value the chart's hottest colour stands for.
};

/// Every figure the fleet chart can draw; the first is the one it does.
///
/// `cpu-busy` because it is the per-machine figure `/fleet.txt` carries for every machine, and the
/// browser's charts (`FleetChartTable`) are fleet-wide series with no per-machine counterpart to
/// borrow: host-wide CPU in use says at a glance which machines are saturated and which idle.
inline constexpr auto FleetChartMetrics = std::to_array<FleetChartMetric>({
    { .key = "cpu-busy",
      .section = Distributed::FleetSection::Machines,
      .subjectColumn = "endpoint",
      .valueColumn = "cpu-busy",
      .scale = 0.001,
      .full = 1.0 },
});

/// The points one parsed document gives every chart metric: one per machine whose figure it carries.
///
/// A machine whose cell is absent, or not a number, gives NO point -- which the chart draws as a gap,
/// never as a zero: `cpu-busy -` is a machine nobody read, not an idle one.
/// @param document The parsed document.
/// @return The points, metric by metric, in the document's row order.
[[nodiscard]] std::vector<SeriesPoint> FleetChartPoints(FleetDocument const& document);

/// A chart's pixels: RGBA, row-major, four bytes a pixel.
struct ChartRaster
{
    std::vector<std::uint8_t> rgba; ///< `width * height * 4` bytes.
    std::size_t width { 0 };        ///< Pixels across.
    std::size_t height { 0 };       ///< Pixels down.
};

/// How @p metric's points across @p history look as a heatmap of @p width by @p height pixels.
///
/// **One band per machine, time across, the newest at the right edge.** Machines are ordered by key,
/// so a machine keeps its band from frame to frame; a sample gets `width / samples` pixel columns,
/// at least one, and a history longer than the width shows its newest `width` samples. A machine a
/// sample carried no point for -- including a sample that read nothing at all -- is TRANSPARENT there:
/// the gap the terminal's background shows through, never a colour a value could have. Pixels no band
/// or sample reaches are transparent too.
/// @param history The model's history.
/// @param metric Which figure.
/// @param width Pixels across; at least one.
/// @param height Pixels down; at least one.
/// @return The raster.
[[nodiscard]] ChartRaster FleetChartRaster(std::deque<HistoryEntry> const& history,
                                           FleetChartMetric const& metric,
                                           std::size_t width,
                                           std::size_t height);

} // namespace FastCache::Cli

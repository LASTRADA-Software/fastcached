// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardLoop.hpp"
#include "HistoryChart.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

struct FleetDocument;

/// @file FleetChartModel.hpp
/// The fleet chart as data: which per-machine figure it draws, the points a reading keeps for it,
/// and the pixels a window of history becomes.
///
/// **The fleet's history chart** (#134): a band per machine across a time window, laid out by the panels' one
/// chart layout -- pixels on the Sixel rung, the rung's chart marks below it. Pure: points in, tracks and pixels
/// out, so every rule below is a case over literals, and the encoder that turns pixels into Sixel stays behind
/// `ISixelEncoder`.

/// One per-machine figure the fleet chart can draw.
///
/// **A row, so a second figure is a row rather than a branch.** Its columns are named here because
/// the chart needs one figure per machine and the document carries dozens: a test requires both
/// columns to be ones the leader's own renderer writes (`FleetColumnNames`), so a column renamed there
/// fails here instead of drawing an empty chart.
///
/// **No scale column**: the raw cell is scaled by the value column's own `Distributed::CellFormat`, looked up in
/// the leader's tables (`FleetColumnFormat`), so `cpu-busy` is in thousandths because the leader says so and not
/// because this row restates it.
struct FleetChartMetric
{
    std::string_view key;              ///< The series its points carry; static storage.
    Distributed::FleetSection section; ///< The table the figure is read from.
    std::string_view subjectColumn;    ///< The column naming the machine a row describes.
    std::string_view valueColumn;      ///< The column holding the figure.
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
      .full = 1.0 },
});

/// The points one parsed document gives every chart metric: one per machine whose figure it carries.
///
/// A machine whose cell is absent, or not a number, gives NO point -- which the chart draws as a gap,
/// never as a zero: `cpu-busy -` is a machine nobody read, not an idle one.
/// @param document The parsed document.
/// @return The points, metric by metric, in the document's row order.
[[nodiscard]] std::vector<SeriesPoint> FleetChartPoints(FleetDocument const& document);

/// One machine the chart draws a band for.
struct ChartBand
{
    std::string subject {};          ///< The machine's key, as the document names it.
    std::optional<double> latest {}; ///< Its scaled figure in the newest sample; none when that sample did not read it.
};

/// Every machine @p metric has a point for in the newest @p window samples of @p history, in key order.
///
/// Key order, so a machine keeps its band from frame to frame as others come and go.
/// @param history The model's history.
/// @param metric Which figure.
/// @param window How many of the newest samples count.
/// @return The bands; empty when no sample read the figure.
[[nodiscard]] std::vector<ChartBand> FleetChartBands(std::deque<HistoryEntry> const& history,
                                                     FleetChartMetric const& metric,
                                                     std::size_t window);

/// @p bands as the chart's tracks: each machine's shares of @p metric's `full` over the newest @p window samples.
///
/// The fleet's side of the one chart renderer (`HistoryChart.hpp`): a machine a sample did not read has no share
/// there, which the renderer draws as nothing read, never as zero.
/// @param history The model's history.
/// @param metric Which figure.
/// @param bands The machines, top to bottom.
/// @param window How many of the newest samples count.
/// @return One track per band, its label the machine's key; `latest` and `top` are the layout's to write.
[[nodiscard]] std::vector<ChartTrack> FleetChartTracks(std::deque<HistoryEntry> const& history,
                                                       FleetChartMetric const& metric,
                                                       std::span<ChartBand const> bands,
                                                       std::size_t window);

/// How @p metric's points across @p history look for @p bands, as @p width by @p height pixels.
///
/// **One band per machine, in @p bands' order, time across with the newest sample at the right edge.** A
/// sample is `width / window` pixel columns wide, at least one, so the chart fills from the right as the
/// history grows.
///
/// **Within a band a reading is a bar rising from the band's floor**, as tall as the value is of
/// `metric.full` -- at least a pixel for any value above zero -- in the ramp colour for it, over a grey
/// track the band's height. So:
///   - zero is the track alone;
///   - a machine a sample did not read is TRANSPARENT there, including a sample that read nothing at all:
///     the terminal's background, never a colour a value could have;
///   - one machine at a steady load is a bar of one height across time.
///
/// A band four or more pixels tall leaves its top row transparent, so neighbouring machines do not merge.
/// Pixels no band or sample reaches are transparent.
/// @param history The model's history.
/// @param metric Which figure.
/// @param bands The machines, top to bottom.
/// @param window The samples the width stands for; see `ChartWindowFor`.
/// @param width Pixels across.
/// @param height Pixels down.
/// @return The raster.
[[nodiscard]] ChartRaster FleetChartRaster(std::deque<HistoryEntry> const& history,
                                           FleetChartMetric const& metric,
                                           std::span<ChartBand const> bands,
                                           std::size_t window,
                                           std::size_t width,
                                           std::size_t height);

} // namespace FastCache::Cli

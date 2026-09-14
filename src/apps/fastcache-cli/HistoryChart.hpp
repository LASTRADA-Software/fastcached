// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardLoop.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file HistoryChart.hpp
/// A chart of history, as data: bands of shares across time, and what they become as pixels or as text cells.
///
/// **The one chart renderer every panel draws with** (#134 F14). A band is a row of the chart: a machine of the
/// fleet, or a figure of a node or a cache. Each band holds its readings' SHARES of the band's top, oldest first,
/// so the renderer knows nothing of what a band stands for, and the fleet and the other panels cannot come to
/// draw a reading, a zero or a gap differently.
///
/// What every chart keeps, whatever draws it:
///   - **a reading is a bar rising from the band's floor**, as tall as its share of the band's top;
///   - **zero is distinct from nothing read**: a zero is the track (pixels) or the lowest mark (text), a
///     sample that did not read the figure is blank;
///   - **the newest sample is at the right edge**, and a sample keeps its width, so a young history fills from the
///     right rather than stretching.
///
/// Pure: shares in, pixels or text out. The encoder that turns pixels into Sixel stays behind `ISixelEncoder`.

/// How a band's bars are coloured on a pixel chart. Private to a process: transmitted and persisted nowhere.
///
/// **A colour is a claim**, so a band makes one only where its figure has one to make. The ramp runs cold to hot,
/// and hot reads as trouble: right for a machine's CPU or a cache's fill against its limit, and wrong for a rate
/// scaled to its own peak, which would draw ordinary load as an alarm. Such a band's height already says its share.
enum class ChartPaint : std::uint8_t
{
    Plain, ///< Every bar one neutral colour: the height is the whole claim.
    Ramp,  ///< Each bar the ramp colour for its share, hot where a high share is a warning.
};

/// One band of a chart: what it is, its newest figure, what its top stands for, and its shares over time.
struct ChartTrack
{
    std::string label {};  ///< The machine or the figure the band is, as a person reads it.
    std::string latest {}; ///< Its newest figure, written; the absent marker when the newest sample did not read it.
    /// What the band's top stands for, written: `1 400` for a band scaled to its own peak. Empty where the chart
    /// states one scale for every band in its legend.
    std::string top {};
    /// One share per sample, oldest first: the reading's share of the top, in [0, 1]; nullopt where not read.
    std::vector<std::optional<double>> shares {};
    /// How its bars are coloured on a pixel chart; the fleet's machines are a load against a whole, so a ramp.
    ChartPaint paint { ChartPaint::Ramp };
};

/// A chart's pixels: RGBA, row-major, four bytes a pixel.
struct ChartRaster
{
    std::vector<std::uint8_t> rgba; ///< `width * height * 4` bytes.
    std::size_t width { 0 };        ///< Pixels across.
    std::size_t height { 0 };       ///< Pixels down.
};

/// How many samples a pixel chart's width can stand for: the chart draws the fewest of these that holds its history.
///
/// **A fixed span rather than whatever the history holds.** Sized to the history, three samples filled the
/// whole width as three blocks, which is the "block-like in one colour or another" nobody could read. With a
/// fixed span a sample keeps its width until the span steps up, the readings not yet taken are blank at the
/// left, and the axis can name a span a person reads.
inline constexpr auto ChartWindows = std::to_array<std::size_t>({ 30, 60, 120, HistoryCapacity });

/// The span a pixel chart of @p samples draws.
/// @param samples How many history entries there are.
/// @return The first `ChartWindows` entry holding them; the last when none does.
[[nodiscard]] std::size_t ChartWindowFor(std::size_t samples) noexcept;

/// The shares of @p values against @p top: each present, finite value's share, clamped into [0, 1].
///
/// A value above @p top is a full bar rather than a bar taller than its band; a negative one is zero. A top that is
/// not positive gives every present value a share of zero, which draws as zero rather than as nothing read.
/// @param values The readings, oldest first; nullopt where not read.
/// @param top What a full bar stands for.
/// @return One share per value.
[[nodiscard]] std::vector<std::optional<double>> SharesOf(std::span<std::optional<double> const> values, double top);

/// The highest present, finite value of @p values, for a band scaled to its own peak.
/// @param values The readings.
/// @return The peak, or nullopt where nothing was read.
[[nodiscard]] std::optional<double> PeakOf(std::span<std::optional<double> const> values) noexcept;

/// @p tracks as @p width by @p height pixels: one band per track, top to bottom, time across with the newest
/// sample at the right edge.
///
/// A sample is `width / window` pixel columns wide, at least one, so the chart fills from the right as the history
/// grows. Within a band a reading is a bar rising from the band's floor, as tall as its share of the band -- at
/// least a pixel for any share above zero -- in the ramp colour for its share, or one neutral colour for a
/// `ChartPaint::Plain` band, over a grey track the band's
/// height. So zero is the track alone; a sample not read is TRANSPARENT, the terminal's background, never a colour
/// a value could have. A band four or more pixels tall leaves its top row transparent, so neighbouring bands do not
/// merge. Pixels no band or sample reaches are transparent.
/// @param tracks The bands, top to bottom.
/// @param window The samples the width stands for; see `ChartWindowFor`.
/// @param width Pixels across.
/// @param height Pixels down.
/// @return The raster.
[[nodiscard]] ChartRaster ChartBandsRaster(std::span<ChartTrack const> tracks,
                                           std::size_t window,
                                           std::size_t width,
                                           std::size_t height);

/// The legend's colour scale: the chart ramp's steps across @p width by @p height pixels, coldest at the left.
/// @param width Pixels across.
/// @param height Pixels down.
/// @return The raster.
[[nodiscard]] ChartRaster ChartScaleRaster(std::size_t width, std::size_t height);

/// One band of a text chart: @p rows lines of @p cells cells, top line first, the newest share at the right edge.
///
/// A sample is one cell and a column of cells is its bar, built from @p levels -- the lowest mark first, the full
/// cell last. The bar is as tall as the share is of every row's levels together: full cells from the floor up, then
/// the partial level on top. **A share of zero is the lowest mark on the floor**, and any share above zero draws at
/// least the next level up, so a small reading is never mistaken for zero; a sample not read is a column of blanks.
/// Cells left of the oldest share are blank: a young history fills from the right.
/// @param shares The band's shares, oldest first; the newest @p cells of them are drawn.
/// @param rows Lines of the band; at least one.
/// @param cells Cells across.
/// @param levels The marks, lowest first; at least two.
/// @return @p rows lines, each @p cells cells wide; empty when @p levels cannot draw a bar.
[[nodiscard]] std::vector<std::string> ChartTextRows(std::span<std::optional<double> const> shares,
                                                     std::size_t rows,
                                                     std::size_t cells,
                                                     std::span<std::string_view const> levels);

} // namespace FastCache::Cli

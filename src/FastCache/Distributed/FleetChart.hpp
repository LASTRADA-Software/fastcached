// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Distributed
{

/// What a series does with the raw slots behind it.
enum class FleetSeriesKind : std::uint8_t
{
    /// The difference between two buckets, per minute. Absent when the counter went
    /// backwards, which is what a restart looks like.
    Rate = 0,
    /// The bucket's own reading.
    Level,
    /// One counter's delta over the sum of two, as a percentage. Absent when the
    /// denominator's delta is zero: a bucket that served no reads has no hit rate,
    /// which is not the same as one that served only misses.
    Share,
};

/// One line or band on a chart.
struct FleetSeriesRow
{
    std::string_view key;   ///< JSON key.
    std::string_view label; ///< Legend text.
    /// A palette token, **without** its leading `--`: the page turns it into a
    /// class name and the SVG into a `var(--...)`. One spelling, because a token
    /// spelled two ways is one that goes stale in whichever place is edited less.
    std::string_view colour;
    FleetSeriesKind kind;    ///< How to derive it.
    FleetMetric numerator;   ///< The slot it reads.
    FleetMetric denominator; ///< The second slot, for `Share`; ignored otherwise.
};

/// Which chart a series belongs to, and how that chart is drawn.
enum class FleetChartId : std::uint8_t
{
    Dispatched = 0, ///< Is the fleet doing work at all.
    Refusals,       ///< Never one line: four reasons, four fixes.
    Capacity,       ///< The gap between offerable and in-flight is headroom.
    HitRate,        ///< Per bucket, so a regression cannot hide behind a running total.
    Last
};

/// How a chart stacks or overlays its series.
enum class FleetChartShape : std::uint8_t
{
    Line = 0, ///< Each series its own filled line.
    Stacked,  ///< Series summed into bands, bottom-up in table order.
};

/// One chart: its identity, its prose, and the series it draws.
struct FleetChartRow
{
    FleetChartId id;          ///< The chart this row describes.
    std::string_view key;     ///< What the URL says, and the JSON key.
    std::string_view title;   ///< The panel heading.
    std::string_view caption; ///< The line under it: what the chart is *for*.
    std::string_view unit;    ///< Suffix on the gridline labels, e.g. `%`.
    FleetChartShape shape;    ///< Line or stacked.
    std::size_t first;        ///< Index into `FleetSeriesTable` of its first series.
    std::size_t count;        ///< How many consecutive series it draws.
};

/// Every series, grouped so each chart's are consecutive.
///
/// The four refusal reasons keep `LeaseOutcomeTable`'s vocabulary and its colour
/// logic: red is a configuration mistake, cobalt is our own saturation, amber is
/// somebody else's work, grey is a duplicate and not a shortage at all. Collapsing
/// them into one line is what hides which of four different problems a fleet has.
inline constexpr std::array<FleetSeriesRow, 8> FleetSeriesTable {
    FleetSeriesRow { .key = "dispatched",
                     .label = "dispatched",
                     .colour = "accent",
                     .kind = FleetSeriesKind::Rate,
                     .numerator = FleetMetric::DispatchGranted,
                     .denominator = FleetMetric::DispatchGranted },
    FleetSeriesRow { .key = "no-worker",
                     .label = "no worker",
                     .colour = "crit",
                     .kind = FleetSeriesKind::Rate,
                     .numerator = FleetMetric::DispatchNoWorker,
                     .denominator = FleetMetric::DispatchNoWorker },
    FleetSeriesRow { .key = "no-capacity",
                     .label = "no capacity",
                     .colour = "accent",
                     .kind = FleetSeriesKind::Rate,
                     .numerator = FleetMetric::DispatchNoCapacity,
                     .denominator = FleetMetric::DispatchNoCapacity },
    FleetSeriesRow { .key = "withdrawn",
                     .label = "withdrawn",
                     .colour = "warn",
                     .kind = FleetSeriesKind::Rate,
                     .numerator = FleetMetric::DispatchWithdrawn,
                     .denominator = FleetMetric::DispatchWithdrawn },
    FleetSeriesRow { .key = "duplicate",
                     .label = "duplicate",
                     .colour = "inert",
                     .kind = FleetSeriesKind::Rate,
                     .numerator = FleetMetric::DispatchDuplicate,
                     .denominator = FleetMetric::DispatchDuplicate },
    FleetSeriesRow { .key = "offerable",
                     .label = "offerable slots",
                     .colour = "muted",
                     .kind = FleetSeriesKind::Level,
                     .numerator = FleetMetric::OfferableSlots,
                     .denominator = FleetMetric::OfferableSlots },
    FleetSeriesRow { .key = "in-flight",
                     .label = "in flight",
                     .colour = "accent",
                     .kind = FleetSeriesKind::Level,
                     .numerator = FleetMetric::JobsInFlight,
                     .denominator = FleetMetric::JobsInFlight },
    FleetSeriesRow { .key = "hit-rate",
                     .label = "hit rate",
                     .colour = "ok",
                     .kind = FleetSeriesKind::Share,
                     .numerator = FleetMetric::CacheHits,
                     .denominator = FleetMetric::CacheMisses },
};

/// Every chart, in enumerator order. Titles and captions are the mockup's.
inline constexpr EnumTable<FleetChartId, FleetChartRow> FleetChartTable {
    FleetChartRow { .id = FleetChartId::Dispatched,
                    .key = "dispatched",
                    .title = "Compiles dispatched",
                    .caption = "Is the fleet doing work at all. The shape of a working day.",
                    .unit = "",
                    .shape = FleetChartShape::Line,
                    .first = 0,
                    .count = 1 },
    FleetChartRow { .id = FleetChartId::Refusals,
                    .key = "refusals",
                    .title = "Refusals, by reason",
                    .caption = "Never one line. Four reasons, four different fixes.",
                    .unit = "",
                    .shape = FleetChartShape::Stacked,
                    .first = 1,
                    .count = 4 },
    FleetChartRow { .id = FleetChartId::Capacity,
                    .key = "capacity",
                    .title = "Capacity vs. in flight",
                    .caption = "The gap is headroom. When it closes, the fleet is the bottleneck.",
                    .unit = "",
                    .shape = FleetChartShape::Line,
                    .first = 5,
                    .count = 2 },
    FleetChartRow { .id = FleetChartId::HitRate,
                    .key = "hit-rate",
                    .title = "Cache hit rate",
                    .caption = "Per bucket, not cumulative — a running total stops moving and hides a regression.",
                    .unit = "%",
                    .shape = FleetChartShape::Line,
                    .first = 7,
                    .count = 1 },
};
static_assert(RowsInEnumeratorOrder(FleetChartTable, &FleetChartRow::id));

/// Look a chart up by what the URL said.
/// @param key The path tail, without `.svg`.
/// @return The chart, or nullopt — never a silent default.
[[nodiscard]] std::optional<FleetChartId> FleetChartFromKey(std::string_view key) noexcept;

/// One series' values across a range, gaps included.
///
/// `optional` per point rather than a parallel presence array: a gap has to be
/// impossible to read as a number by accident, and that is what a caller forgets.
using FleetSeriesValues = std::vector<std::optional<double>>;

/// Derive one series from the buckets behind it.
/// @param series Which series.
/// @param buckets What `FleetHistory::Buckets` returned.
/// @param bucketSeconds How wide one bucket is, for a per-minute rate.
/// @return One value per bucket, absent where it cannot be known.
[[nodiscard]] FleetSeriesValues ValuesFor(FleetSeriesRow const& series,
                                          std::vector<FleetBucket> const& buckets,
                                          std::int64_t bucketSeconds);

/// Which palette an SVG carries, since it cannot see the page's.
enum class FleetTheme : std::uint8_t
{
    Auto = 0, ///< Ships both and lets `prefers-color-scheme` choose.
    Light,
    Dark,
    Last
};

/// What a URL calls one theme.
struct FleetThemeRow
{
    FleetTheme theme;     ///< The theme this row describes.
    std::string_view key; ///< What `theme=` says, and what the `ETag` carries.
};

/// Every theme, in enumerator order.
inline constexpr EnumTable<FleetTheme, FleetThemeRow> FleetThemeTable {
    FleetThemeRow { .theme = FleetTheme::Auto, .key = "auto" },
    FleetThemeRow { .theme = FleetTheme::Light, .key = "light" },
    FleetThemeRow { .theme = FleetTheme::Dark, .key = "dark" },
};
static_assert(RowsInEnumeratorOrder(FleetThemeTable, &FleetThemeRow::theme));

/// Look a theme up by what the URL said; anything unknown is `Auto`.
///
/// Unlike a range or a chart this one *does* have a safe default, and the
/// difference is worth stating: `Auto` renders correctly under either setting, so a
/// typo costs a viewer nothing. A silently-substituted range would put them on the
/// wrong axis without saying so, which is why that one is refused instead.
/// @param key What `theme=` said.
/// @return The theme; `Auto` for anything unrecognised.
[[nodiscard]] FleetTheme FleetThemeFromKey(std::string_view key) noexcept;

/// What a theme is called, for the URL and the `ETag`.
/// @param theme The theme.
/// @return Its key.
[[nodiscard]] std::string_view FleetThemeKey(FleetTheme theme) noexcept;

/// Render one chart as a standalone SVG document.
///
/// Standalone is the whole difficulty: served as its own resource it cannot see
/// the page's custom properties, so it carries its own palette and its own
/// `prefers-color-scheme` block — which is why theme is part of the URL.
/// @param chart Which chart.
/// @param buckets The range's buckets.
/// @param range Which range, for the axis labels.
/// @param theme Which palette.
/// @return A complete `image/svg+xml` document.
[[nodiscard]] std::string RenderChartSvg(FleetChartRow const& chart,
                                         std::vector<FleetBucket> const& buckets,
                                         FleetRange range,
                                         FleetTheme theme);

/// Render the sparkline the Dispatched tile carries.
///
/// No theme, unlike the charts: this one is inlined into the page rather than
/// referenced, so it is part of that document and inherits its custom properties.
/// Carrying a palette of its own would freeze the tile into one theme while the
/// tile around it followed the viewer's.
/// @param buckets The day range's buckets.
/// @return A small inline SVG, no axes and no grid.
[[nodiscard]] std::string RenderSparklineSvg(std::vector<FleetBucket> const& buckets);

/// Render every series across a range as JSON.
/// @param buckets The range's buckets.
/// @param range Which range.
/// @return An object carrying the range, the bucket starts and each series.
[[nodiscard]] std::string RenderSeriesJson(std::vector<FleetBucket> const& buckets, FleetRange range);

/// One series folded across a whole range, for a KPI tile.
///
/// Each kind folds the only way that is honest for it: a `Rate` to the **total** it
/// accounts for, a `Share` to the share over the whole range, and a `Level` to its
/// newest known reading.
///
/// A `Share` is emphatically not the mean of the per-bucket shares. That average
/// weights a bucket that served four reads exactly as heavily as one that served
/// forty thousand, so a single idle bucket at 0% drags a headline figure that the
/// chart beside it visibly contradicts.
/// @param series Which series.
/// @param buckets The range's buckets.
/// @param bucketSeconds Bucket width.
/// @return The folded value, or nullopt when nothing in the range is known.
[[nodiscard]] std::optional<double> RangeValueOf(FleetSeriesRow const& series,
                                                 std::vector<FleetBucket> const& buckets,
                                                 std::int64_t bucketSeconds);

/// Look a series up by its key.
/// @param key The series' `key`.
/// @return The row, or nullptr when nothing matches.
[[nodiscard]] FleetSeriesRow const* FleetSeriesFromKey(std::string_view key) noexcept;

/// The most recent known value of a series, for a KPI tile.
/// @param series Which series.
/// @param buckets The range's buckets.
/// @param bucketSeconds Bucket width.
/// @return The newest value that is known, or nullopt when none is.
[[nodiscard]] std::optional<double> LatestOf(FleetSeriesRow const& series,
                                             std::vector<FleetBucket> const& buckets,
                                             std::int64_t bucketSeconds);

} // namespace FastCache::Distributed

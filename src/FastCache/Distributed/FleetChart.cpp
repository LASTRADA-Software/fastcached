// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetChart.hpp>
#include <FastCache/Distributed/FleetText.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>

namespace FastCache::Distributed
{

namespace
{
    // The mockup's geometry, kept exactly: a wider viewBox would rescale every
    // stroke and a taller one would change where the captions sit.
    constexpr double ChartWidth = 640.0;
    constexpr double ChartHeight = 150.0;
    constexpr double PadTop = 8.0;
    constexpr double PadBottom = 18.0;
    constexpr int GridLines = 3;

    constexpr double SparkWidth = 120.0;
    constexpr double SparkHeight = 24.0;

    /// The box a series is drawn into.
    ///
    /// A descriptor rather than two sets of constants read by two loops, because the
    /// tile sparkline and the full chart split their readings into runs by exactly
    /// the same rules -- and the copy that did it a second time by hand drew a lone
    /// reading as a bare `M x y`, which strokes nothing at all.
    struct ChartGeometry
    {
        double width;     ///< viewBox width.
        double height;    ///< viewBox height.
        double padTop;    ///< Room above the tallest reading.
        double padBottom; ///< Room below the baseline; the axis labels sit in it.
        double dotRadius; ///< Radius of the dot a run of one reading becomes.
    };

    constexpr ChartGeometry FullChart {
        .width = ChartWidth, .height = ChartHeight, .padTop = PadTop, .padBottom = PadBottom, .dotRadius = 1.6
    };

    /// One line of a tile: a pixel of padding, and a dot small enough to read as one.
    constexpr ChartGeometry SparkChart {
        .width = SparkWidth, .height = SparkHeight, .padTop = 1.0, .padBottom = 1.0, .dotRadius = 1.2
    };

    // A `var(...)` reference ends in `)`, so spelling one inside a raw string puts
    // the sequence `)"` into the literal and terminates it early -- silently, at the
    // wrong place. Named here and passed as format arguments instead, which keeps
    // that sequence out of every literal in this file.
    constexpr std::string_view HairlineVar = "var(--hairline)";
    constexpr std::string_view FaintVar = "var(--faint)";
    constexpr std::string_view AccentVar = "var(--accent)";

    [[nodiscard]] std::uint64_t SlotOf(FleetBucket const& bucket, FleetMetric metric) noexcept
    {
        return bucket.values[static_cast<std::size_t>(metric)];
    }

    /// Whether this bucket holds a reading for this slot at all.
    ///
    /// `present` answers it for a window this leader sampled. A BACKFILLED window is
    /// different: it was assembled by summing what the machines reported for a window
    /// this leader was not elected for, and a machine cannot answer for a dispatch
    /// outcome -- only a scheduler produces one, and `BackfillInto` leaves those slots
    /// at zero for exactly that reason.
    ///
    /// Zero is a legitimate reading for every slot here, so a renderer that took it
    /// would draw a fleet that granted nothing between one that granted a thousand
    /// and one that granted two thousand: a rate running backwards, read as a
    /// restart, and then a spike of two thousand. Neither happened, and both would
    /// sit in the twelve-month ring long after the election that caused them.
    /// @param bucket The window.
    /// @param metric The slot.
    /// @return True when the number in that slot means something.
    [[nodiscard]] bool Answers(FleetBucket const& bucket, FleetMetric metric) noexcept
    {
        return !bucket.backfilled || FleetMetricTable[static_cast<std::size_t>(metric)].scope == FleetMetricScope::Node;
    }

    /// Whether a bucket can answer for every slot a series reads.
    ///
    /// Both slots, unconditionally: `denominator` is set on every row -- a `Rate` row
    /// names its numerator twice -- so there is no kind to special-case here, and a
    /// `Share` whose two slots disagreed about who can answer would be half a
    /// reading.
    /// @param bucket The window.
    /// @param series What is being drawn.
    /// @return True when the window is a reading for it.
    [[nodiscard]] bool Answers(FleetBucket const& bucket, FleetSeriesRow const& series) noexcept
    {
        return bucket.present && Answers(bucket, series.numerator) && Answers(bucket, series.denominator);
    }

    /// Whether a counter's difference may be taken between these two windows.
    ///
    /// Adjacent and answering is not enough: the two must also be assembled from the
    /// same POPULATION. A window this leader sampled sums a counter over the workers
    /// registered at that moment; a backfilled one sums it over every machine that has
    /// ever handed history over, and nothing erases an entry. Across that boundary the
    /// difference of two cumulative totals is a difference between two different sets
    /// of machines -- with ten workstations and two of them on a build new enough to
    /// hand history over, one five-minute bucket gets credited with the eight million
    /// cache reads the other eight had accumulated since they started.
    ///
    /// Unknowable rather than large, which is the same answer a restart gets.
    /// @param here The newer window.
    /// @param prior The window immediately before it.
    /// @param series What is being drawn.
    /// @return True when the pair is a difference somebody can act on.
    [[nodiscard]] bool Comparable(FleetBucket const& here, FleetBucket const& prior, FleetSeriesRow const& series) noexcept
    {
        return Answers(here, series) && Answers(prior, series) && here.backfilled == prior.backfilled;
    }

    /// The palette an SVG carries, since it cannot reach the page's.
    ///
    /// The same hexes as the stylesheet, which is a duplication with a reason: a
    /// document referenced by `<img>` is a separate document and inherits nothing.
    /// The alternative is inlining every chart into the page, which is what serving
    /// them as cacheable resources exists to avoid.
    struct Palette
    {
        std::string_view ink;
        std::string_view muted;
        std::string_view faint;
        std::string_view hairline;
        std::string_view surface;
        std::string_view accent;
        std::string_view ok;
        std::string_view warn;
        std::string_view crit;
        std::string_view inert;
    };

    constexpr Palette LightPalette { .ink = "#10141A",
                                     .muted = "#616B78",
                                     .faint = "#8A939F",
                                     .hairline = "#E3E8EE",
                                     .surface = "#FFFFFF",
                                     .accent = "#2F5FA8",
                                     .ok = "#197A4B",
                                     .warn = "#A15C07",
                                     .crit = "#B3261E",
                                     .inert = "#93A0B0" };

    constexpr Palette DarkPalette { .ink = "#E3E8EE",
                                    .muted = "#8D97A5",
                                    .faint = "#6B7686",
                                    .hairline = "#212932",
                                    .surface = "#151A21",
                                    .accent = "#6E9BE0",
                                    .ok = "#45AE79",
                                    .warn = "#D2913A",
                                    .crit = "#E36B6B",
                                    .inert = "#5B6675" };

    [[nodiscard]] Palette const& PaletteFor(FleetTheme theme) noexcept
    {
        return theme == FleetTheme::Dark ? DarkPalette : LightPalette;
    }

    /// One palette as a CSS custom-property block.
    [[nodiscard]] std::string Vars(Palette const& palette)
    {
        return std::format("--ink:{};--muted:{};--faint:{};--hairline:{};--surface:{};"
                           "--accent:{};--ok:{};--warn:{};--crit:{};--inert:{};",
                           palette.ink,
                           palette.muted,
                           palette.faint,
                           palette.hairline,
                           palette.surface,
                           palette.accent,
                           palette.ok,
                           palette.warn,
                           palette.crit,
                           palette.inert);
    }

    /// The document's own stylesheet.
    ///
    /// Custom properties rather than literal fills, because an `<img>`-referenced
    /// SVG is a separate document that inherits nothing from the page -- so the
    /// only way an `Auto` chart can follow the viewer's setting is to carry both
    /// palettes and let its own media query choose.
    [[nodiscard]] std::string StyleBlock(FleetTheme theme)
    {
        auto out = std::format("<style>svg{{{}}}", Vars(PaletteFor(theme)));
        if (theme == FleetTheme::Auto)
            out += std::format("@media (prefers-color-scheme:dark){{svg{{{}}}}}", Vars(DarkPalette));
        out += "</style>";
        return out;
    }

    [[nodiscard]] double Baseline(ChartGeometry const& geometry) noexcept
    {
        return geometry.height - geometry.padBottom;
    }

    [[nodiscard]] double ScaleY(double value, double max, ChartGeometry const& geometry) noexcept
    {
        if (max <= 0.0)
            return Baseline(geometry);
        auto const usable = geometry.height - geometry.padTop - geometry.padBottom;
        return geometry.padTop + (usable * (1.0 - std::min(value / max, 1.0)));
    }

    /// Round a maximum up to something a gridline label can say plainly.
    [[nodiscard]] double NiceMax(double value) noexcept
    {
        if (!(value > 0.0))
            return 1.0;
        auto const magnitude = std::pow(10.0, std::floor(std::log10(value)));
        return std::ceil(value / magnitude) * magnitude;
    }

    [[nodiscard]] double XAt(std::size_t index, std::size_t count, ChartGeometry const& geometry) noexcept
    {
        if (count <= 1)
            return 0.0;
        return geometry.width * static_cast<double>(index) / static_cast<double>(count - 1);
    }

    /// The geometry of one series: path data, and the readings that have no line.
    ///
    /// Two strings and not one, because they are two different KINDS of markup.
    /// `path` is the value of a `d` attribute; `dots` is a run of elements. Handing
    /// back a single string concatenating both -- which this did -- puts
    /// `<circle .../>` inside `d="..."`, and a browser parsing an `<img>`-referenced
    /// SVG as XML refuses the entire document over it. The chart then arrives with a
    /// 200 and a plausible length and renders as a broken image saying nothing.
    struct SeriesGeometry
    {
        std::string path; ///< `d` data: one `M`/`L` run per unbroken stretch of readings.
        std::string dots; ///< `<circle>` elements for the runs of one, which have no line.
    };

    /// One polyline per unbroken run of known points, plus a dot per run of one.
    ///
    /// Runs and not one path, because a single path across a gap would draw a line
    /// through hours nobody observed -- which is the zero-versus-absent confusion
    /// this whole surface is built to avoid, in its most misleading form.
    ///
    /// @param values   The series, absent where nothing was observed.
    /// @param max      The value the vertical axis tops out at.
    /// @param close    Fill each run down to the baseline. Does not change what a run
    ///                 of one becomes: a single reading has no width to fill either.
    /// @param geometry The box to draw into: a full chart, or a tile's sparkline.
    /// @return The `d` data and the standalone dot elements, never concatenated.
    [[nodiscard]] SeriesGeometry RunsGeometry(FleetSeriesValues const& values,
                                              double max,
                                              bool close,
                                              ChartGeometry const& geometry)
    {
        SeriesGeometry out;
        std::size_t index = 0;
        while (index < values.size())
        {
            auto const& first = values[index];
            if (!first.has_value())
            {
                ++index;
                continue;
            }
            auto const runStart = index;
            auto const firstOfRun = *first;
            std::string path;
            while (index < values.size())
            {
                auto const& reading = values[index];
                if (!reading.has_value())
                    break;
                path += std::format("{}{:.2f} {:.2f}",
                                    index == runStart ? "M" : "L",
                                    XAt(index, values.size(), geometry),
                                    ScaleY(*reading, max, geometry));
                ++index;
            }
            if (index - runStart < 2)
            {
                // A single known point has no line to draw, so it gets a dot -- a
                // run of one is still a reading and dropping it would under-report.
                // Closing one instead does not rescue it: `M x y L x bottom L x
                // bottom Z` is a shape with no width, which draws nothing while
                // leaving a non-empty `d` that says the series WAS observed.
                // An ELEMENT, kept apart from the path data the caller is about to
                // put in a `d` attribute.
                out.dots += std::format(R"(<circle cx="{:.2f}" cy="{:.2f}" r="{}"/>)",
                                        XAt(runStart, values.size(), geometry),
                                        ScaleY(firstOfRun, max, geometry),
                                        geometry.dotRadius);
                continue;
            }
            if (close)
                path += std::format("L{:.2f} {:.2f}L{:.2f} {:.2f}Z",
                                    XAt(index - 1, values.size(), geometry),
                                    Baseline(geometry),
                                    XAt(runStart, values.size(), geometry),
                                    Baseline(geometry));
            out.path += path;
        }
        return out;
    }

    [[nodiscard]] std::string Gridlines(double max, std::string_view unit)
    {
        std::string out;
        for (auto const step: std::views::iota(0, GridLines + 1))
        {
            auto const value = max * static_cast<double>(step) / GridLines;
            auto const y = ScaleY(value, max, FullChart);
            out += std::format(R"(<line x1="0" x2="{:.0f}" y1="{:.1f}" y2="{:.1f}" stroke="{}" stroke-width="1"/>)",
                               ChartWidth,
                               y,
                               y,
                               HairlineVar);
            if (step > 0)
                // Below its line, not above it. The topmost gridline sits `PadTop`
                // from the edge, and a label placed above that one has its ascenders
                // outside the viewBox -- clipped, silently, and only for the line
                // carrying the largest number on the chart.
                out += std::format(
                    R"(<text x="4" y="{:.1f}" font-size="9" fill="{}" font-family="ui-monospace,monospace">{:.0f}{}</text>)",
                    y + 9.0,
                    FaintVar,
                    value,
                    EscapeMarkup(unit));
        }
        return out;
    }

    /// The civil date and time one instant falls on, in UTC.
    ///
    /// Hand-rolled from the epoch rather than through `localtime`, which is not
    /// thread-safe, and through UTC rather than local time because a chart served as
    /// its own resource is read on machines in other zones than the one that drew it.
    struct CivilTime
    {
        int month;  ///< 1-12.
        int day;    ///< 1-31.
        int hour;   ///< 0-23.
        int minute; ///< 0-59.
    };

    /// @param millis Milliseconds since the epoch.
    /// @return Its UTC civil fields.
    [[nodiscard]] CivilTime CivilFromMillis(std::int64_t millis) noexcept
    {
        auto const days = std::chrono::floor<std::chrono::days>(std::chrono::milliseconds { millis });
        auto const ymd = std::chrono::year_month_day { std::chrono::sys_days { days } };
        auto const rest = std::chrono::milliseconds { millis } - days;
        auto const hours = std::chrono::duration_cast<std::chrono::hours>(rest);
        auto const minutes = std::chrono::duration_cast<std::chrono::minutes>(rest - hours);
        return CivilTime { .month = static_cast<int>(static_cast<unsigned>(ymd.month())),
                           .day = static_cast<int>(static_cast<unsigned>(ymd.day())),
                           .hour = static_cast<int>(hours.count()),
                           .minute = static_cast<int>(minutes.count()) };
    }

    /// Whether a chart's ticks need a DATE rather than a clock.
    ///
    /// A tick every twenty-four hours lands at midnight every time, so `HH:MM` prints
    /// `00:00` six times and the axis says nothing at all -- which is what the two
    /// short ranges never revealed, and what every range past a week does. The
    /// threshold is the span between ticks rather than the range's name, so it stays
    /// right as rows are added.
    ///
    /// @param buckets What is being drawn.
    /// @param step How many buckets apart the ticks are.
    /// @return True when the labels should read `MM-DD`.
    [[nodiscard]] bool TicksNeedADate(std::vector<FleetBucket> const& buckets, std::size_t step) noexcept
    {
        if (buckets.size() < 2 || step == 0)
            return false;
        auto const width = buckets[1].startMillis - buckets[0].startMillis;
        constexpr std::int64_t DayMillis = 24 * 60 * 60 * 1000;
        return width * static_cast<std::int64_t>(step) >= DayMillis;
    }

    /// Labels along the bottom, spaced so they do not collide.
    [[nodiscard]] std::string AxisLabels(std::vector<FleetBucket> const& buckets)
    {
        constexpr std::size_t Wanted = 6;
        if (buckets.size() < 2)
            return {};
        auto const step = std::max<std::size_t>(1, buckets.size() / Wanted);
        auto const dated = TicksNeedADate(buckets, step);

        std::string out;
        for (std::size_t index = 0; index < buckets.size(); index += step)
        {
            auto const civil = CivilFromMillis(buckets[index].startMillis);
            auto const label = dated ? std::format("{:02}-{:02}", civil.month, civil.day)
                                     : std::format("{:02}:{:02}", civil.hour, civil.minute);
            out += std::format(
                R"(<text x="{:.1f}" y="{:.0f}" font-size="9" fill="{}" font-family="ui-monospace,monospace" text-anchor="{}">{}</text>)",
                XAt(index, buckets.size(), FullChart),
                ChartHeight - 5.0,
                FaintVar,
                index == 0 ? "start" : "middle",
                label);
        }
        return out;
    }

    /// The document shell: both palettes, so a viewer's own setting decides.
    [[nodiscard]] std::string OpenSvg(double width, double height, FleetTheme theme, std::string_view label)
    {
        auto out = std::format(
            R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {:.0f} {:.0f}" preserveAspectRatio="none" role="img" aria-label="{}">)",
            width,
            height,
            EscapeMarkup(label));
        out += StyleBlock(theme);
        return out;
    }
} // namespace

std::optional<FleetChartId> FleetChartFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetChartTable)
        if (row.key == key)
            return row.id;
    return std::nullopt;
}

FleetTheme FleetThemeFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetThemeTable)
        if (row.key == key)
            return row.theme;
    return FleetTheme::Auto;
}

std::string_view FleetThemeKey(FleetTheme theme) noexcept
{
    return FleetThemeTable[static_cast<std::size_t>(theme)].key;
}

FleetSeriesValues ValuesFor(FleetSeriesRow const& series,
                            std::vector<FleetBucket> const& buckets,
                            std::int64_t bucketSeconds)
{
    FleetSeriesValues out(buckets.size(), std::nullopt);
    auto const minutes = static_cast<double>(bucketSeconds) / 60.0;

    for (auto const index: std::views::iota(std::size_t { 0 }, buckets.size()))
    {
        auto const& here = buckets[index];
        if (!Answers(here, series))
            continue;

        if (series.kind == FleetSeriesKind::Level)
        {
            out[index] = static_cast<double>(SlotOf(here, series.numerator));
            continue;
        }

        // A rate and a share both need the bucket before this one, and it must be
        // the one immediately before: a delta taken across a gap would spread work
        // over hours nobody observed and report a rate nobody saw.
        if (index == 0 || !Comparable(here, buckets[index - 1], series))
            continue;
        auto const& prior = buckets[index - 1];

        auto const numeratorNow = SlotOf(here, series.numerator);
        auto const numeratorWas = SlotOf(prior, series.numerator);
        // Backwards means the counter restarted. Absent, not zero and not a spike.
        if (numeratorNow < numeratorWas)
            continue;
        auto const numerator = numeratorNow - numeratorWas;

        if (series.kind == FleetSeriesKind::Rate)
        {
            // The span BETWEEN THE TWO SAMPLES, not the nominal bucket width. The
            // newest bucket is always still open -- a 7-day view one minute past
            // the hour holds two readings two minutes apart and would divide them
            // by sixty, reporting a thirtieth of the rate on the point labelled
            // "now" and on the headline beside the chart. A window that was only
            // partly sampled is understated the same way, less dramatically.
            //
            // Falls back to the nominal width when the two carry no usable span,
            // which is what a bucket assembled by hand rather than by `Buckets()`
            // has.
            auto const observed = static_cast<double>(here.sampleMillis - prior.sampleMillis) / 60'000.0;
            auto const divisor = observed > 0.0 ? observed : minutes;
            out[index] = divisor > 0.0 ? static_cast<double>(numerator) / divisor : 0.0;
            continue;
        }

        auto const otherNow = SlotOf(here, series.denominator);
        auto const otherWas = SlotOf(prior, series.denominator);
        if (otherNow < otherWas)
            continue;
        auto const total = numerator + (otherNow - otherWas);
        // A bucket that served no reads has no hit rate. Reporting 0% would say the
        // cache missed everything, which is a different and alarming claim.
        if (total == 0)
            continue;
        out[index] = 100.0 * static_cast<double>(numerator) / static_cast<double>(total);
    }
    return out;
}

FleetSeriesRow const* FleetSeriesFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetSeriesTable)
        if (row.key == key)
            return &row;
    return nullptr;
}

std::optional<double> RangeValueOf(FleetSeriesRow const& series,
                                   std::vector<FleetBucket> const& buckets,
                                   std::int64_t bucketSeconds)
{
    if (series.kind == FleetSeriesKind::Level)
        return LatestOf(series, buckets, bucketSeconds);
    // A delta needs two buckets. Guarded before the walk rather than inside it,
    // because `iota(1, 0)` on an empty range is not an empty range -- it is
    // undefined behaviour, and a node with no history yet renders exactly that.
    if (buckets.size() < 2)
        return std::nullopt;

    // Deltas summed from the raw slots rather than from `ValuesFor`, so a share is
    // taken over the range's whole traffic instead of averaging per-bucket shares.
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 0;
    bool known = false;
    for (auto const index: std::views::iota(std::size_t { 1 }, buckets.size()))
    {
        auto const& here = buckets[index];
        auto const& prior = buckets[index - 1];
        // The same rule the per-bucket values follow, for the same two reasons: a
        // window backfilled from what the machines reported cannot answer for a
        // dispatch outcome, and a difference taken across that boundary is a
        // difference between two different sets of machines.
        if (!Comparable(here, prior, series))
            continue;

        auto const numeratorNow = SlotOf(here, series.numerator);
        auto const numeratorWas = SlotOf(prior, series.numerator);
        if (numeratorNow < numeratorWas)
            continue; // A restart, and its step is unknowable rather than zero.
        numerator += numeratorNow - numeratorWas;
        known = true;

        if (series.kind != FleetSeriesKind::Share)
            continue;
        auto const otherNow = SlotOf(here, series.denominator);
        auto const otherWas = SlotOf(prior, series.denominator);
        if (otherNow >= otherWas)
            denominator += otherNow - otherWas;
    }
    if (!known)
        return std::nullopt;

    if (series.kind == FleetSeriesKind::Rate)
        return static_cast<double>(numerator);
    auto const total = numerator + denominator;
    // Nothing was read in the whole range, so there is no hit rate to report. 0%
    // would claim the cache missed everything.
    if (total == 0)
        return std::nullopt;
    return 100.0 * static_cast<double>(numerator) / static_cast<double>(total);
}

std::optional<double> LatestOf(FleetSeriesRow const& series,
                               std::vector<FleetBucket> const& buckets,
                               std::int64_t bucketSeconds)
{
    auto const values = ValuesFor(series, buckets, bucketSeconds);
    for (auto const& value: std::views::reverse(values))
        if (value.has_value())
            return value;
    return std::nullopt;
}

std::string RenderChartSvg(FleetChartRow const& chart,
                           std::vector<FleetBucket> const& buckets,
                           FleetRange range,
                           FleetTheme theme)
{
    auto const seconds =
        std::chrono::duration_cast<std::chrono::seconds>(FleetRangeTable[static_cast<std::size_t>(range)].bucket).count();

    std::vector<FleetSeriesValues> series;
    series.reserve(chart.count);
    for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count))
        series.push_back(ValuesFor(FleetSeriesTable[chart.first + offset], buckets, seconds));

    // A share is a percentage and its axis is fixed: rescaling it to whatever the
    // window happened to contain would make a dip from 95% to 90% look like a
    // collapse.
    auto const fixedAxis = FleetSeriesTable[chart.first].kind == FleetSeriesKind::Share;
    double max = 0.0;
    if (fixedAxis)
        max = 100.0;
    else if (chart.shape == FleetChartShape::Stacked)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, buckets.size()))
        {
            double total = 0.0;
            for (auto const& values: series)
                total += values[index].value_or(0.0);
            max = std::max(max, total);
        }
        max = NiceMax(max * 1.15);
    }
    else
    {
        for (auto const& values: series)
            for (auto const& value: values)
                max = std::max(max, value.value_or(0.0));
        max = NiceMax(max * 1.15);
    }

    auto out = OpenSvg(ChartWidth, ChartHeight, theme, std::format("{} over the selected range", chart.title));
    out += Gridlines(max, chart.unit);

    if (chart.shape == FleetChartShape::Stacked)
    {
        // Every band's cumulative top, computed bottom-up in table order.
        std::vector<FleetSeriesValues> tops;
        tops.reserve(chart.count);
        FleetSeriesValues running(buckets.size(), 0.0);
        for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count))
        {
            FleetSeriesValues top(buckets.size(), std::nullopt);
            for (auto const index: std::views::iota(std::size_t { 0 }, buckets.size()))
            {
                auto const& here = series[offset][index];
                if (!here.has_value())
                    continue;
                running[index] = running[index].value_or(0.0) + *here;
                top[index] = running[index];
            }
            tops.push_back(std::move(top));
        }

        // Drawn **top band first**, each filled all the way down to the baseline, so
        // the band below paints over the part that is not its neighbour's. Drawing
        // bottom-up instead leaves every band overlapping every one above it, and
        // translucent fills then multiply into a colour that belongs to no series --
        // exactly the "four reasons collapse into one" this chart exists to prevent,
        // reintroduced by the renderer.
        for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count) | std::views::reverse)
        {
            auto const& row = FleetSeriesTable[chart.first + offset];
            auto const band = RunsGeometry(tops[offset], max, true, FullChart);
            auto const colour = std::format("var(--{})", row.colour);
            // An element with an empty `d` is not nothing: it is a shape a renderer
            // still has to consider, and it makes "this series was never observed"
            // indistinguishable from "this series was flat" in the output.
            if (!band.path.empty())
                out += std::format(R"(<path d="{}" fill="{}" fill-opacity="0.85"/>)", band.path, colour);
            // A band's isolated readings are dots too, at the band's own top and at
            // the band's own opacity -- a reading a stacked chart cannot fill is
            // still a reading, and the alternative here is drawing nothing.
            if (!band.dots.empty())
                out += std::format(R"(<g fill="{}" fill-opacity="0.85">{}</g>)", colour, band.dots);
        }
    }
    else
        for (auto const offset: std::views::iota(std::size_t { 0 }, chart.count))
        {
            auto const& row = FleetSeriesTable[chart.first + offset];
            auto const colour = std::format("var(--{})", row.colour);
            // A ceiling is drawn as a ceiling: dashed, and with barely any fill under
            // it. The gap between the two lines is what this chart is about, and a
            // reader who cannot tell which line is the limit reads that gap backwards.
            auto const ceiling = row.stroke == FleetSeriesStroke::Dashed;
            // `.path` only: the same isolated readings come back from both calls, and
            // the line pass below is where they are drawn.
            if (auto const area = RunsGeometry(series[offset], max, true, FullChart); !area.path.empty())
                out += std::format(
                    R"(<path d="{}" fill="{}" fill-opacity="{}"/>)", area.path, colour, ceiling ? "0.10" : "0.14");
            auto const line = RunsGeometry(series[offset], max, false, FullChart);
            if (!line.path.empty())
                out += std::format(R"(<path d="{}" fill="none" stroke="{}" stroke-width="1.6" )"
                                   R"(stroke-linejoin="round"{}/>)",
                                   line.path,
                                   colour,
                                   ceiling ? R"( stroke-dasharray="4 3")" : "");
            // Siblings of the path rather than part of it, and grouped so the fill is
            // named once: a dot inherits nothing from the path it belongs to, and an
            // unfilled circle renders black -- a colour belonging to no series here.
            if (!line.dots.empty())
                out += std::format(R"(<g fill="{}">{}</g>)", colour, line.dots);
        }

    out += AxisLabels(buckets);
    out += "</svg>";
    return out;
}

std::string RenderSparklineSvg(std::vector<FleetBucket> const& buckets)
{
    auto const seconds =
        std::chrono::duration_cast<std::chrono::seconds>(FleetRangeTable[static_cast<std::size_t>(FleetRange::Day)].bucket)
            .count();
    auto const values = ValuesFor(FleetSeriesTable[0], buckets, seconds);

    double max = 0.0;
    for (auto const& value: values)
        max = std::max(max, value.value_or(0.0));
    max = NiceMax(max * 1.1);

    std::string out = std::format(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {:.0f} {:.0f}" preserveAspectRatio="none" aria-hidden="true">)",
        SparkWidth,
        SparkHeight);

    // The same run-splitter the full charts use, at the tile's size: a second copy
    // of this loop is what left a lone reading here as a bare `M x y`, which strokes
    // nothing -- the tile then showed an empty strip on exactly the quiet stretch
    // the reading was reporting.
    auto const line = RunsGeometry(values, max, false, SparkChart);
    if (!line.path.empty())
        out += std::format(
            R"(<path d="{}" fill="none" stroke="{}" stroke-width="1.5" stroke-linejoin="round"/>)", line.path, AccentVar);
    if (!line.dots.empty())
        out += std::format(R"(<g fill="{}">{}</g>)", AccentVar, line.dots);
    out += "</svg>";
    return out;
}

namespace
{
    /// One per-bucket array the series JSON carries beside `series`.
    struct BucketArrayRow
    {
        std::string_view key;                      ///< Its JSON name.
        std::string (*render)(FleetBucket const&); ///< What one bucket contributes.
    };

    /// Every array that runs parallel to `series`, in the order they are written.
    ///
    /// A table because a consumer reads them by index against the series arrays, so
    /// one that walked a different set of buckets than another would misalign
    /// silently -- the loop is written once and every row rides it.
    constexpr std::array<BucketArrayRow, 3> BucketArrayTable {
        BucketArrayRow { .key = "start",
                         .render = [](FleetBucket const& bucket) { return std::format("{}", bucket.startMillis); } },
        // `null` for a window nobody sampled, and never 0: a fleet that did nothing
        // and a fleet nobody was watching are different facts, which is the same
        // distinction the series values themselves make.
        BucketArrayRow { .key = "coverage",
                         .render =
                             [](FleetBucket const& bucket) {
                                 return bucket.present ? std::format("{}", bucket.coverage) : std::string { "null" };
                             } },
        // Which windows this leader did not sample itself, and holds only because the
        // machines handed their own records over. Their scheduler-scoped series are
        // `null` throughout, because no machine can answer for a dispatch outcome.
        BucketArrayRow {
            .key = "backfilled",
            .render = [](FleetBucket const& bucket) { return std::string { bucket.backfilled ? "true" : "false" }; } },
    };
} // namespace

std::string RenderSeriesJson(std::vector<FleetBucket> const& buckets, FleetRange range)
{
    auto const& row = FleetRangeTable[static_cast<std::size_t>(range)];
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(row.bucket).count();

    std::string out = "{";
    AppendJsonText(out, "range");
    out += ':';
    AppendJsonText(out, row.key);
    // `covers` is what a window nobody missed a sample of holds, so a consumer can
    // read `coverage` as a fraction without knowing this build's sample interval.
    out += std::format(R"(,"bucketSeconds":{},"points":{},"covers":{})", seconds, buckets.size(), FullCoverageOf(range));

    // One row per per-bucket array, so a fourth is a row rather than a fourth
    // hand-written loop spelling "comma-separated over the buckets" a fourth way.
    for (auto const& array: BucketArrayTable)
    {
        out += ',';
        AppendJsonText(out, array.key);
        out += ":[";
        for (auto const index: std::views::iota(std::size_t { 0 }, buckets.size()))
        {
            if (index != 0)
                out += ',';
            out += array.render(buckets[index]);
        }
        out += ']';
    }
    out += R"(,"series":{)";

    for (auto const index: std::views::iota(std::size_t { 0 }, FleetSeriesTable.size()))
    {
        auto const& series = FleetSeriesTable[index];
        if (index != 0)
            out += ',';
        AppendJsonText(out, series.key);
        out += ":[";
        auto const values = ValuesFor(series, buckets, seconds);
        for (auto const point: std::views::iota(std::size_t { 0 }, values.size()))
        {
            if (point != 0)
                out += ',';
            // `null` and never 0: a consumer that could not tell them apart would
            // average a gap into the rest, which is exactly the mistake the page
            // renders differently.
            auto const& value = values[point];
            if (value.has_value())
                out += std::format("{:.4g}", *value);
            else
                out += "null";
        }
        out += ']';
    }
    out += "}}";
    return out;
}

} // namespace FastCache::Distributed

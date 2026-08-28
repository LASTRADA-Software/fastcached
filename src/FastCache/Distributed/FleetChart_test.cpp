// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetChart.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::Unwrap;

namespace
{

/// Bucket width the Day range uses, so a rate in a case is arithmetic a reader can
/// check rather than a constant taken on trust.
constexpr std::int64_t DaySeconds = 300;

/// One present bucket carrying the slots a case names; everything else zero.
[[nodiscard]] FleetBucket At(std::int64_t millis, std::initializer_list<std::pair<FleetMetric, std::uint64_t>> slots)
{
    FleetBucket bucket {};
    bucket.startMillis = millis;
    // A window sampled right at its start: `Buckets()` would produce this for a
    // bucket whose only reading landed in its first sub-slot. Cases about a
    // PARTLY-filled window set `sampleMillis` themselves.
    bucket.sampleMillis = millis;
    bucket.present = true;
    for (auto const& [metric, value]: slots)
        bucket.values[static_cast<std::size_t>(metric)] = value;
    return bucket;
}

/// A bucket nobody sampled.
[[nodiscard]] FleetBucket Gap(std::int64_t millis)
{
    FleetBucket bucket {};
    bucket.startMillis = millis;
    return bucket;
}

[[nodiscard]] FleetSeriesRow const& SeriesByKey(std::string_view key)
{
    for (auto const& row: FleetSeriesTable)
        if (row.key == key)
            return row;
    FAIL("no series named " << key);
    return FleetSeriesTable[0];
}

[[nodiscard]] FleetChartRow const& ChartByKey(std::string_view key)
{
    for (auto const& row: FleetChartTable)
        if (row.key == key)
            return row;
    FAIL("no chart named " << key);
    return FleetChartTable[0];
}

/// The offset of the first `<` sitting inside an attribute value, if there is one.
///
/// A browser parses an `<img>`-referenced SVG as XML and refuses the whole document
/// over one stray `<` there. The chart then arrives with a 200 and a plausible byte
/// count and renders as a broken image, with nothing anywhere saying why -- so the
/// assertion has to be on the markup rather than on what the server returned with
/// it. Deliberately narrow: this is the one malformation the renderer can produce,
/// by putting an element where path data belongs.
[[nodiscard]] std::optional<std::size_t> MarkupInAttribute(std::string_view svg)
{
    bool inTag = false;
    bool inValue = false;
    for (auto const index: std::views::iota(std::size_t { 0 }, svg.size()))
    {
        auto const character = svg[index];
        if (inValue)
        {
            if (character == '"')
                inValue = false;
            else if (character == '<')
                return index;
        }
        else if (character == '<')
        {
            if (inTag)
                return index; // A tag opening inside a tag: the same fault, unquoted.
            inTag = true;
        }
        else if (character == '>')
            inTag = false;
        else if (character == '"' && inTag)
            inValue = true;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("A chart is looked up by what the URL said, and an unknown one is refused", "[distributed][fleetchart]")
{
    for (auto const& row: FleetChartTable)
        CHECK(FleetChartFromKey(row.key) == row.id);
    // Never a silent default: an unknown tail must 404 rather than quietly serve
    // whichever chart happened to be first.
    CHECK_FALSE(FleetChartFromKey("dispatch").has_value());
    CHECK_FALSE(FleetChartFromKey("").has_value());
}

TEST_CASE("An unknown theme falls back to Auto rather than being refused", "[distributed][fleetchart]")
{
    CHECK(FleetThemeFromKey("dark") == FleetTheme::Dark);
    CHECK(FleetThemeFromKey("light") == FleetTheme::Light);
    // Unlike a range or a chart, a theme has a safe default: Auto ships both
    // palettes, so a typo renders correctly instead of failing the request.
    CHECK(FleetThemeFromKey("solarized") == FleetTheme::Auto);
    CHECK(FleetThemeFromKey("") == FleetTheme::Auto);
}

TEST_CASE("A rate is the delta between adjacent buckets, per minute", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::DispatchGranted, 100 } }),
        At(300'000, { { FleetMetric::DispatchGranted, 130 } }),
    };

    auto const values = ValuesFor(SeriesByKey("dispatched"), buckets, DaySeconds);
    REQUIRE(values.size() == 2);
    // The first bucket has nothing before it, so its rate is unknowable -- absent,
    // and emphatically not the cumulative 100.
    CHECK_FALSE(values[0].has_value());
    REQUIRE(values[1].has_value());
    CHECK(Unwrap(values[1]) == 6.0); // 30 compiles over five minutes.
}

TEST_CASE("A partly-filled newest bucket is a rate over the span actually seen", "[distributed][fleetchart]")
{
    // The newest bucket is ALWAYS still open, and `Buckets()` stamps the window's
    // start on whatever it folded, so the delta and the divisor come from
    // different clocks. A 7-day view a minute past the hour holds the sample from
    // h:01 and the one from (h-1):59 -- two minutes of work -- and dividing that
    // by the nominal sixty reports a thirtieth of the rate, on the point labelled
    // "now" and on the `HeadlineOf` figure beside the chart.
    //
    // Here in miniature: five-minute windows, but the two readings are one minute
    // apart, so 30 compiles is 30 per minute rather than 6.
    auto prior = At(0, { { FleetMetric::DispatchGranted, 100 } });
    auto latest = At(300'000, { { FleetMetric::DispatchGranted, 130 } });
    prior.sampleMillis = 240'000;  // sampled near the end of its window
    latest.sampleMillis = 300'000; // sampled at the start of its own: one minute later

    std::vector<FleetBucket> const buckets { prior, latest };

    auto const values = ValuesFor(SeriesByKey("dispatched"), buckets, DaySeconds);
    REQUIRE(values.size() == 2);
    REQUIRE(values[1].has_value());
    CHECK(Unwrap(values[1]) == 30.0);
}

TEST_CASE("A counter that goes backwards is a restart, and renders as a gap", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::DispatchGranted, 5'000 } }),
        At(300'000, { { FleetMetric::DispatchGranted, 12 } }),
        At(600'000, { { FleetMetric::DispatchGranted, 42 } }),
    };

    auto const values = ValuesFor(SeriesByKey("dispatched"), buckets, DaySeconds);
    // Storing counters raw is what makes this detectable at all: a delta stored at
    // sample time would have recorded a negative as an enormous unsigned spike.
    CHECK_FALSE(values[1].has_value());
    REQUIRE(values[2].has_value());
    CHECK(Unwrap(values[2]) == 6.0);
}

TEST_CASE("A rate is never taken across a gap", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::DispatchGranted, 10 } }),
        Gap(300'000),
        At(600'000, { { FleetMetric::DispatchGranted, 610 } }),
    };

    auto const values = ValuesFor(SeriesByKey("dispatched"), buckets, DaySeconds);
    // 600 compiles arrived at some point in ten minutes, and spreading them over the
    // five nobody observed would report a rate that never happened.
    CHECK_FALSE(values[2].has_value());
}

TEST_CASE("A gauge is the bucket's own reading, with no bucket before it needed", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets { At(0, { { FleetMetric::JobsInFlight, 7 } }) };

    auto const values = ValuesFor(SeriesByKey("in-flight"), buckets, DaySeconds);
    REQUIRE(values[0].has_value());
    CHECK(Unwrap(values[0]) == 7.0);
}

TEST_CASE("A share with no reads in the bucket is absent, not zero", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::CacheHits, 90 }, { FleetMetric::CacheMisses, 10 } }),
        At(300'000, { { FleetMetric::CacheHits, 90 }, { FleetMetric::CacheMisses, 10 } }),
        At(600'000, { { FleetMetric::CacheHits, 93 }, { FleetMetric::CacheMisses, 11 } }),
    };

    auto const values = ValuesFor(SeriesByKey("hit-rate"), buckets, DaySeconds);
    // Nothing was read in the second bucket. Rendering 0% would claim the cache
    // missed everything, which is a different and much more alarming statement.
    CHECK_FALSE(values[1].has_value());
    REQUIRE(values[2].has_value());
    CHECK(Unwrap(values[2]) == 75.0); // 3 hits of 4 reads.
}

TEST_CASE("The newest known value is what a KPI tile shows", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::JobsInFlight, 3 } }),
        At(300'000, { { FleetMetric::JobsInFlight, 9 } }),
        Gap(600'000),
    };

    auto const latest = LatestOf(SeriesByKey("in-flight"), buckets, DaySeconds);
    // The tile reads back past the trailing gap rather than reporting nothing: the
    // newest *known* value is what an operator is asking for.
    REQUIRE(latest.has_value());
    CHECK(Unwrap(latest) == 9.0);

    CHECK_FALSE(LatestOf(SeriesByKey("in-flight"), { Gap(0), Gap(300'000) }, DaySeconds).has_value());
}

TEST_CASE("An all-absent series draws nothing at all", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets { Gap(0), Gap(300'000), Gap(600'000) };

    auto const svg = RenderChartSvg(ChartByKey("dispatched"), buckets, FleetRange::Day, FleetTheme::Light);
    // Gridlines and axis labels still render -- the chart's frame is not a claim
    // about the data -- but no path and no dot, because nothing was observed.
    CHECK_FALSE(svg.contains("<path d=\"\""));
    CHECK_FALSE(svg.contains("<circle"));
    CHECK_FALSE(svg.contains('M'));
    CHECK(svg.starts_with("<svg "));
    CHECK(svg.ends_with("</svg>"));
}

TEST_CASE("A reading with no neighbours is a dot beside the path, never inside it", "[distributed][fleetchart]")
{
    // Two adjacent samples with a gap either side. A share needs the bucket before
    // it, so exactly one point is known and it has no line to draw. The live
    // dashboard's hit-rate chart is full of these -- a mostly idle daemon samples in
    // short adjacent runs -- and every one of them was emitted as a `<circle>`
    // element INSIDE the `d` attribute of the path, which made the whole document
    // unparseable and the chart a broken image.
    std::vector<FleetBucket> const buckets {
        Gap(0),
        At(300'000, { { FleetMetric::CacheHits, 10 }, { FleetMetric::CacheMisses, 5 } }),
        At(600'000, { { FleetMetric::CacheHits, 14 }, { FleetMetric::CacheMisses, 5 } }),
        Gap(900'000),
    };

    auto const svg = RenderChartSvg(ChartByKey("hit-rate"), buckets, FleetRange::Day, FleetTheme::Light);
    INFO(svg);
    CHECK_FALSE(MarkupInAttribute(svg).has_value());

    // The reading is still drawn: a run of one is a reading, and dropping it would
    // under-report. It carries the series colour itself, because a document loaded
    // through `<img>` inherits nothing -- an unfilled circle renders black, which on
    // this page is a colour belonging to no series at all.
    CHECK(svg.contains("<circle"));
    CHECK(svg.contains("<g fill=\"var(--ok)\">"));
}

TEST_CASE("A lone reading in a stacked band is a dot, not a shape with no width", "[distributed][fleetchart]")
{
    // A rate needs the bucket before it, so these four windows hold exactly one
    // known value each, at index 2. Closing a run of one gives
    // `M x y L x bottom L x bottom Z` -- a shape with no width, so it draws nothing,
    // while its non-empty `d` still says the series was observed. Refusals are the
    // stacked chart, and a single refusal in an otherwise quiet hour is precisely
    // the reading somebody came to this chart for.
    std::vector<FleetBucket> const buckets {
        Gap(0),
        At(300'000, { { FleetMetric::DispatchNoWorker, 4 } }),
        At(600'000, { { FleetMetric::DispatchNoWorker, 9 } }),
        Gap(900'000),
    };

    auto const svg = RenderChartSvg(ChartByKey("refusals"), buckets, FleetRange::Day, FleetTheme::Light);
    INFO(svg);
    CHECK_FALSE(MarkupInAttribute(svg).has_value());
    CHECK(svg.contains("<circle"));
    // At the band's own opacity, so a dot reads as part of its band rather than as a
    // fifth thing on a chart whose entire point is four distinguishable reasons.
    CHECK(svg.contains("<g fill=\"var(--crit)\" fill-opacity=\"0.85\">"));
    // Nothing is left drawing nothing: every run here is of one, so there is no
    // filled shape to emit at all.
    CHECK_FALSE(svg.contains("<path"));
}

TEST_CASE("A chart is a document a browser may load as an image", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> buckets;
    for (auto const index: std::views::iota(0, 12))
        buckets.push_back(At(index * 300'000,
                             { { FleetMetric::DispatchGranted, static_cast<std::uint64_t>(index) * 20 },
                               { FleetMetric::CacheHits, static_cast<std::uint64_t>(index) * 7 },
                               { FleetMetric::CacheMisses, static_cast<std::uint64_t>(index) },
                               { FleetMetric::OfferableSlots, 24 },
                               { FleetMetric::JobsInFlight, static_cast<std::uint64_t>(index) % 5 } }));

    // The same readings with every third window unobserved. Gaps are the ordinary
    // case on a real dashboard rather than an edge one -- a node samples only while
    // it runs -- and they are what produce a run of a single point. A renderer that
    // is well-formed only over dense data is a renderer that is broken on the live
    // page and passing here.
    auto gapped = buckets;
    for (auto const index: std::views::iota(std::size_t { 0 }, gapped.size()))
        if (index % 3 == 0)
            gapped[index] = Gap(gapped[index].startMillis);

    for (auto const& chart: FleetChartTable)
    {
        auto const svg = RenderChartSvg(chart, buckets, FleetRange::Day, FleetTheme::Auto);
        INFO("chart " << chart.key);
        CHECK_FALSE(MarkupInAttribute(svg).has_value());
        CHECK_FALSE(MarkupInAttribute(RenderChartSvg(chart, gapped, FleetRange::Day, FleetTheme::Auto)).has_value());
        // Served as its own resource and loaded through <img>, so a script would not
        // run anyway -- but nothing here has any business emitting one, and a chart
        // that started to would be worth failing over rather than discovering later.
        CHECK_FALSE(svg.contains("<script"));
        CHECK(svg.contains("<path"));
        // Auto ships both palettes and lets the viewer's own setting choose, because
        // an <img>-referenced document inherits nothing from the page around it.
        CHECK(svg.contains("prefers-color-scheme:dark"));
    }
}

TEST_CASE("A fixed theme carries one palette and no media query", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets { At(0, { { FleetMetric::DispatchGranted, 1 } }) };

    auto const dark = RenderChartSvg(ChartByKey("dispatched"), buckets, FleetRange::Day, FleetTheme::Dark);
    CHECK_FALSE(dark.contains("prefers-color-scheme"));
    CHECK(dark.contains("--surface:#151A21"));

    auto const light = RenderChartSvg(ChartByKey("dispatched"), buckets, FleetRange::Day, FleetTheme::Light);
    CHECK(light.contains("--surface:#FFFFFF"));
}

TEST_CASE("A hostile unit is escaped rather than interpolated", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::CacheHits, 1 }, { FleetMetric::CacheMisses, 1 } }),
        At(300'000, { { FleetMetric::CacheHits, 2 }, { FleetMetric::CacheMisses, 2 } }),
    };

    // The table's own units are safe; this proves the renderer escapes rather than
    // relying on that, since a series label is one refactor away from carrying a
    // toolchain fingerprint a peer chose.
    auto hostile = ChartByKey("hit-rate");
    hostile.unit = "\"><script>alert(1)</script>";

    auto const svg = RenderChartSvg(hostile, buckets, FleetRange::Day, FleetTheme::Light);
    CHECK_FALSE(svg.contains("<script"));
    CHECK(svg.contains("&lt;script&gt;"));
}

TEST_CASE("The series JSON carries every key the table names", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::DispatchGranted, 4 } }),
        At(300'000, { { FleetMetric::DispatchGranted, 9 } }),
    };

    auto const json = RenderSeriesJson(buckets, FleetRange::Day);
    // One table drives the SVGs, the legends and this: a series added for a chart
    // and forgotten here would be a consumer silently missing a column.
    for (auto const& series: FleetSeriesTable)
    {
        INFO("series " << series.key);
        CHECK(json.contains(std::format(R"("{}":[)", series.key)));
    }
    CHECK(json.contains("\"range\":\"24h\""));
    CHECK(json.contains("\"bucketSeconds\":300"));
    // Absent is null and never 0, so a consumer cannot average a gap into the rest.
    CHECK(json.contains("[null,1"));
}

TEST_CASE("The sparkline inherits the page's palette rather than carrying one", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::DispatchGranted, 1 } }),
        At(300'000, { { FleetMetric::DispatchGranted, 5 } }),
        At(600'000, { { FleetMetric::DispatchGranted, 6 } }),
    };

    auto const spark = RenderSparklineSvg(buckets);
    // Inlined into the page, so it is part of that document: no <style>, no palette,
    // and a var() the tile around it resolves.
    CHECK_FALSE(spark.contains("<style>"));
    CHECK(spark.contains("var(--accent)"));
    CHECK(spark.contains("<path"));

    CHECK_FALSE(RenderSparklineSvg({ Gap(0), Gap(300'000) }).contains("<path"));
}

TEST_CASE("The sparkline draws a lone reading rather than an empty strip", "[distributed][fleetchart]")
{
    // A rate needs the bucket before it, so this holds exactly one value. The tile
    // built its path with its own copy of the run-splitting loop, which emitted
    // `M x y` and nothing else -- a move-to strokes no ink, so the strip came out
    // blank on precisely the quiet stretch the one reading was there to report.
    std::vector<FleetBucket> const buckets {
        Gap(0),
        At(300'000, { { FleetMetric::DispatchGranted, 2 } }),
        At(600'000, { { FleetMetric::DispatchGranted, 20 } }),
        Gap(900'000),
    };

    auto const spark = RenderSparklineSvg(buckets);
    INFO(spark);
    CHECK_FALSE(MarkupInAttribute(spark).has_value());
    CHECK(spark.contains("<circle"));
    CHECK(spark.contains("<g fill=\"var(--accent)\">"));
    // Nothing but the dot: a run of one has no line, and a lone `M` would be a `d`
    // that says the series was drawn while drawing nothing.
    CHECK_FALSE(spark.contains("<path"));
}

TEST_CASE("A range with nothing in it folds to nothing, and does not walk backwards", "[distributed][fleetchart]")
{
    // A node that has just started leading has no buckets at all, and every KPI
    // tile folds every series over exactly that. The delta walk starts at index one,
    // so an empty range is not an empty loop -- it is `iota(1, 0)`, which is
    // undefined behaviour rather than nothing.
    std::vector<FleetBucket> const nothing;
    for (auto const& series: FleetSeriesTable)
    {
        INFO("series " << series.key);
        CHECK_FALSE(RangeValueOf(series, nothing, DaySeconds).has_value());
        CHECK_FALSE(LatestOf(series, nothing, DaySeconds).has_value());
        CHECK(ValuesFor(series, nothing, DaySeconds).empty());
    }

    // One bucket is one reading and no delta: a gauge still reads, a rate cannot.
    std::vector<FleetBucket> const one { At(0, { { FleetMetric::DispatchGranted, 9 }, { FleetMetric::JobsInFlight, 2 } }) };
    CHECK_FALSE(RangeValueOf(SeriesByKey("dispatched"), one, DaySeconds).has_value());
    CHECK(RangeValueOf(SeriesByKey("in-flight"), one, DaySeconds) == 2.0);
}

TEST_CASE("A range's share is taken over its whole traffic, not averaged per bucket", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::CacheHits, 0 }, { FleetMetric::CacheMisses, 0 } }),
        // A quiet bucket: one read, and it missed.
        At(300'000, { { FleetMetric::CacheHits, 0 }, { FleetMetric::CacheMisses, 1 } }),
        // A busy one: 99 reads, all hits.
        At(600'000, { { FleetMetric::CacheHits, 99 }, { FleetMetric::CacheMisses, 1 } }),
    };

    auto const share = RangeValueOf(SeriesByKey("hit-rate"), buckets, DaySeconds);
    REQUIRE(share.has_value());
    // 99 of 100 reads hit. The mean of the per-bucket shares would be 49.5%, which
    // weights one read exactly as heavily as ninety-nine and reports a headline the
    // chart beside it visibly contradicts.
    CHECK(Unwrap(share) == 99.0);
}

TEST_CASE("A rate folds to the total it accounts for", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets {
        At(0, { { FleetMetric::DispatchGranted, 100 } }),
        At(300'000, { { FleetMetric::DispatchGranted, 130 } }),
        At(600'000, { { FleetMetric::DispatchGranted, 175 } }),
    };

    // 30 + 45 compiles, and not the per-minute rate the chart draws: a tile that
    // said "6" where 75 compiles ran would be read as the fleet having stalled.
    CHECK(RangeValueOf(SeriesByKey("dispatched"), buckets, DaySeconds) == 75.0);
}

TEST_CASE("A series is looked up by its key", "[distributed][fleetchart]")
{
    for (auto const& row: FleetSeriesTable)
    {
        auto const* const found = FleetSeriesFromKey(row.key);
        REQUIRE(found != nullptr);
        CHECK(found->key == row.key);
    }
    CHECK(FleetSeriesFromKey("granted") == nullptr);
}

TEST_CASE("Every chart's series sit inside the table it indexes into", "[distributed][fleetchart]")
{
    // `first` and `count` are indices into a second table, which is the one shape
    // of table-driven design that can go out of bounds. A chart whose window ran
    // past the end would read whatever followed it in memory.
    for (auto const& chart: FleetChartTable)
    {
        INFO("chart " << chart.key);
        CHECK(chart.count >= 1);
        CHECK(chart.first + chart.count <= FleetSeriesTable.size());
    }
}

TEST_CASE("A theme's key round-trips", "[distributed][fleetchart]")
{
    for (auto const& row: FleetThemeTable)
    {
        CHECK(FleetThemeFromKey(row.key) == row.theme);
        CHECK(FleetThemeKey(row.theme) == row.key);
    }
}

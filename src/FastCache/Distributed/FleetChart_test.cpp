// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetChart.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Distributed;

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
    CHECK(*values[1] == 6.0); // 30 compiles over five minutes.
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
    CHECK(*values[2] == 6.0);
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
    CHECK(*values[0] == 7.0);
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
    CHECK(*values[2] == 75.0); // 3 hits of 4 reads.
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
    CHECK(*latest == 9.0);

    CHECK_FALSE(LatestOf(SeriesByKey("in-flight"), { Gap(0), Gap(300'000) }, DaySeconds).has_value());
}

TEST_CASE("An all-absent series draws nothing at all", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets { Gap(0), Gap(300'000), Gap(600'000) };

    auto const svg = RenderChartSvg(ChartByKey("dispatched"), buckets, FleetRange::Day, FleetTheme::Light);
    // Gridlines and axis labels still render -- the chart's frame is not a claim
    // about the data -- but no path and no dot, because nothing was observed.
    CHECK(svg.find("<path d=\"\"") == std::string::npos);
    CHECK(svg.find("<circle") == std::string::npos);
    CHECK(svg.find("M") == std::string::npos);
    CHECK(svg.starts_with("<svg "));
    CHECK(svg.ends_with("</svg>"));
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

    for (auto const& chart: FleetChartTable)
    {
        auto const svg = RenderChartSvg(chart, buckets, FleetRange::Day, FleetTheme::Auto);
        INFO("chart " << chart.key);
        // Served as its own resource and loaded through <img>, so a script would not
        // run anyway -- but nothing here has any business emitting one, and a chart
        // that started to would be worth failing over rather than discovering later.
        CHECK(svg.find("<script") == std::string::npos);
        CHECK(svg.find("<path") != std::string::npos);
        // Auto ships both palettes and lets the viewer's own setting choose, because
        // an <img>-referenced document inherits nothing from the page around it.
        CHECK(svg.find("prefers-color-scheme:dark") != std::string::npos);
    }
}

TEST_CASE("A fixed theme carries one palette and no media query", "[distributed][fleetchart]")
{
    std::vector<FleetBucket> const buckets { At(0, { { FleetMetric::DispatchGranted, 1 } }) };

    auto const dark = RenderChartSvg(ChartByKey("dispatched"), buckets, FleetRange::Day, FleetTheme::Dark);
    CHECK(dark.find("prefers-color-scheme") == std::string::npos);
    CHECK(dark.find("--surface:#151A21") != std::string::npos);

    auto const light = RenderChartSvg(ChartByKey("dispatched"), buckets, FleetRange::Day, FleetTheme::Light);
    CHECK(light.find("--surface:#FFFFFF") != std::string::npos);
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
    CHECK(svg.find("<script") == std::string::npos);
    CHECK(svg.find("&lt;script&gt;") != std::string::npos);
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
        CHECK(json.find(std::string { "\"" } + std::string { series.key } + "\":[") != std::string::npos);
    }
    CHECK(json.find("\"range\":\"24h\"") != std::string::npos);
    CHECK(json.find("\"bucketSeconds\":300") != std::string::npos);
    // Absent is null and never 0, so a consumer cannot average a gap into the rest.
    CHECK(json.find("[null,1") != std::string::npos);
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
    CHECK(spark.find("<style>") == std::string::npos);
    CHECK(spark.find("var(--accent)") != std::string::npos);
    CHECK(spark.find("<path") != std::string::npos);

    CHECK(RenderSparklineSvg({ Gap(0), Gap(300'000) }).find("<path") == std::string::npos);
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
    CHECK(*share == 99.0);
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

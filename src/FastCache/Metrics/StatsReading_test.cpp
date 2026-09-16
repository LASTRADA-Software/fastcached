// SPDX-License-Identifier: Apache-2.0
//
// #1484: a counter this process has no writer for reads ABSENT, not zero -- and the absence says
// WHICH kind it is, because the scrape surface treats one of them as a fault and the other as an
// ordinary fact about the binary.
//
// Every case here has a CONTROL that must pass in the other direction. The fix's failure mode is
// the mirror of the bug's: too wide renders a plausible zero, too narrow invents a `-` that reads
// as *this process does not do that*, and a test written only for the second direction passes
// over a capture that has stopped reporting anything at all.
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>

using namespace FastCache;

namespace
{

/// A process serving none of the surfaces any catalogue row is attributed to: a compile node.
///
/// Spelled here rather than reaching for `Node::NodeServedSurfaces`, which lives in an app this
/// translation unit does not link. That the two agree is what `NodeStatusResponder_test` asserts,
/// against the node's own constant.
constexpr std::array<MetricsSurface, 0> ServesNothingAttributed {};

/// The row #1484 was found on, and the one whose sole writers were positively established.
constexpr auto Connections = IMetricsSink::Counter::ConnectionsTotal;

} // namespace

TEST_CASE("A counter a process could write reads an honest zero", "[metrics][stats-reading]")
{
    // THE CONTROL, and the direction that gets skipped when a fix like this is written: a counter
    // is a tally, so zero is the truth about events that never happened. A daemon that has
    // accepted nothing must report zero here, not `-`. Skipping this case is how the fix becomes
    // "stop reporting a useful counter".
    AtomicMetricsSink sink;
    auto const reading = CaptureStatsReading(sink, MetricsSnapshot {}, EverySurface);

    auto const* const cell = reading.counters.Find(Connections);
    REQUIRE(cell != nullptr);
    REQUIRE(cell->Present());
    CHECK(cell->Value() == 0);
}

TEST_CASE("A counter no surface of this process writes reads ABSENT, not zero", "[metrics][stats-reading]")
{
    AtomicMetricsSink sink;
    sink.Increment(IMetricsSink::Counter::WorkerJobsCompleted, 7);

    auto const reading = CaptureStatsReading(sink, MetricsSnapshot {}, ServesNothingAttributed);

    auto const* const connections = reading.counters.Find(Connections);
    REQUIRE(connections != nullptr);
    CHECK_FALSE(connections->Present());
    // WHICH absence. Asserting only that there is one passes under a capture that reported it as
    // a SKEW, which is the state this whole change exists to stop a healthy node claiming.
    CHECK(connections->Why() == CounterAbsence::NoWriterInThisProcess);

    // BOTH, or a fix that blanked the row passes -- the acceptance says so in as many words.
    auto const* const completed = reading.counters.Find(IMetricsSink::Counter::WorkerJobsCompleted);
    REQUIRE(completed != nullptr);
    REQUIRE(completed->Present());
    CHECK(completed->Value() == 7);
}

TEST_CASE("One connection arrives at each and only the daemon's figure moves", "[metrics][stats-reading]")
{
    // Asserted as the daemon's going 0 -> 1 while the node's stays absent, never as "the two
    // readings differ": they differ under the bug as well, since one would read 1 and the other 0.
    AtomicMetricsSink daemonSink;
    AtomicMetricsSink nodeSink;

    auto const daemonBefore = CaptureStatsReading(daemonSink, MetricsSnapshot {}, EverySurface);
    auto const nodeBefore = CaptureStatsReading(nodeSink, MetricsSnapshot {}, ServesNothingAttributed);
    REQUIRE(daemonBefore.counters.Find(Connections)->Present());
    REQUIRE(daemonBefore.counters.Find(Connections)->Value() == 0);
    REQUIRE_FALSE(nodeBefore.counters.Find(Connections)->Present());

    // The node's sink is incremented too, and it still reads absent. That is the DESIGN and its
    // cost, stated rather than hidden: the ATTRIBUTION decides, not the value, so a row wrongly
    // attributed hides a figure that is real. It is why `CounterSoleWriterTable` holds only rows
    // whose sole writers were established by reading the tree -- in the shipped node this counter
    // cannot be incremented, because neither of its two writers is constructed there.
    daemonSink.Increment(Connections);
    nodeSink.Increment(Connections);

    auto const daemonAfter = CaptureStatsReading(daemonSink, MetricsSnapshot {}, EverySurface);
    auto const nodeAfter = CaptureStatsReading(nodeSink, MetricsSnapshot {}, ServesNothingAttributed);
    CHECK(daemonAfter.counters.Find(Connections)->Present());
    CHECK(daemonAfter.counters.Find(Connections)->Value() == 1);
    CHECK_FALSE(nodeAfter.counters.Find(Connections)->Present());
}

TEST_CASE("A scrape tells a skew from a surface this process does not serve", "[metrics][prometheus]")
{
    auto const snapshot = MetricsSnapshot {};
    AtomicMetricsSink sink;

    // THE CONTROL. Without it every assertion below would pass against an exporter that had
    // stopped emitting the row at all, which is the failure this case is nearest to.
    auto const everything = RenderPrometheus(sink, snapshot, EverySurface);
    INFO("the control: a process serving every surface exports the row and claims no absence");
    REQUIRE(everything.contains("fastcached_connections_total 0\n"));
    REQUIRE(everything.contains("fastcached_metrics_catalogue_skew 0\n"));
    REQUIRE(everything.contains("fastcached_metrics_surface_absent 0\n"));
    REQUIRE_FALSE(everything.contains("# ABSENT"));

    auto const node = RenderPrometheus(sink, snapshot, ServesNothingAttributed);
    INFO("node body:\n" << node);

    // What the bug produced, and the one line that must be gone.
    CHECK_FALSE(node.contains("fastcached_connections_total 0\n"));
    CHECK(node.contains("# ABSENT fastcached_connections_total"));

    // And NOT a skew. This is the assertion the whole design rests on: reusing the one absence
    // `std::optional` could carry would have made this counter nonzero on every healthy compile
    // node, and the skew series would have stopped meaning *catalogue and sink disagree*.
    CHECK(node.contains("fastcached_metrics_catalogue_skew 0\n"));
    CHECK_FALSE(node.contains("# SKEW"));

    // DERIVED from the table, so attributing a fourth row does not silently leave this passing
    // against a stale number.
    CHECK(node.contains(std::format("fastcached_metrics_surface_absent {}\n", CounterSoleWriterTable.size())));
}

TEST_CASE("The reason an absence has survives the wire", "[metrics][stats-reading]")
{
    // The renderer is not always in the process that captured the reading -- `fastcache-cli`'s
    // poll rung decodes one and re-renders Prometheus from it -- so the reason has to travel.
    // This is the case that fails if it is ever made local again.
    AtomicMetricsSink sink;
    auto const reading = CaptureStatsReading(sink, MetricsSnapshot {}, ServesNothingAttributed);

    auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == reading);

    // Asserted on the REASON as well, and not only through equality. `CounterReading::operator==`
    // does discriminate today, which is what makes the line above meaningful -- but a future cell
    // type that compared only presence would leave it passing while the wire carried one absence
    // for both, and this is the assertion that would fail.
    auto const* const cell = decoded->counters.Find(Connections);
    REQUIRE(cell != nullptr);
    REQUIRE_FALSE(cell->Present());
    CHECK(cell->Why() == CounterAbsence::NoWriterInThisProcess);

    // The round trip must not flatten the OTHER direction either: a row this process does write
    // comes back present, with its value.
    auto const* const present = decoded->counters.Find(IMetricsSink::Counter::WorkerJobsCompleted);
    REQUIRE(present != nullptr);
    CHECK(present->Present());
}

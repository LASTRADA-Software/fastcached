// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <string>

namespace FastCache
{

/// Render the Prometheus text exposition format (version 0.0.4) for the given
/// connection-level counters and storage statistics.
///
/// Pure and free of I/O so it can be unit-tested directly. Command-level
/// counters (gets, sets, hit/miss splits, evictions, capacity) come from the
/// storage snapshot — the authoritative source — while connection-level
/// counters come from the metrics sink. Each metric is emitted with its
/// `# HELP` / `# TYPE` lines followed by the `fastcached_<name> <value>` sample.
///
/// A pure function of the reading: everything it renders was captured by `CaptureStatsReading`,
/// which is what lets a live-stats snapshot encoded from the same reading never disagree with it.
///
/// @param reading What to render.
/// @return A complete metrics body in Prometheus text exposition format.
[[nodiscard]] std::string RenderPrometheus(StatsReading const& reading);

/// Capture and render in one call: what a scrape route does.
/// @param metrics Connection-level counter sink.
/// @param snapshot Per-scrape storage stats and process uptime.
/// @return `RenderPrometheus(CaptureStatsReading(metrics, snapshot))`.
[[nodiscard]] std::string RenderPrometheus(IMetricsSink const& metrics, MetricsSnapshot const& snapshot);

} // namespace FastCache

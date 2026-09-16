// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <span>
#include <string>
#include <string_view>

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
///
/// @p surfaces defaults to `EverySurface`, which is what this tree assumed for every binary
/// before #1484, so a call site nobody has revisited renders exactly as it does today. A process
/// that serves only some surfaces states so -- and states it here rather than being detected,
/// because the two mistakes are not symmetric: too WIDE renders a plausible zero, the cost
/// already being paid, while too NARROW invents a `-` that reads as *this process does not do
/// that* and nobody re-checks.
/// @param metrics Connection-level counter sink.
/// @param snapshot Per-scrape storage stats and process uptime.
/// @param surfaces The surfaces this process serves.
/// @return `RenderPrometheus(CaptureStatsReading(metrics, snapshot, surfaces))`.
[[nodiscard]] std::string RenderPrometheus(IMetricsSink const& metrics,
                                           MetricsSnapshot const& snapshot,
                                           std::span<MetricsSurface const> surfaces = EverySurface);

struct InfoDescriptor;

/// Render one info series: its `# HELP` and `# TYPE` lines and the single sample, with
/// the fact escaped as a Prometheus label value.
///
/// Separate from `RenderPrometheus` so the escaping can be tested with a value no
/// build carries.
/// @param row The info series to render.
/// @param value The fact, unescaped, as the reading holds it.
/// @return The three exposition lines.
[[nodiscard]] std::string RenderInfoMetric(InfoDescriptor const& row, std::string_view value);

} // namespace FastCache

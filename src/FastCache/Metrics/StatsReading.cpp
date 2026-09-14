// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <cstddef>

namespace FastCache
{

StatsReading CaptureStatsReading(IMetricsSink const& metrics, MetricsSnapshot const& snapshot)
{
    StatsReading reading { .counters = {}, .snapshot = snapshot };
    // Driven by the catalogue, not by the enum's range: the catalogue is what every encoding
    // walks, so a row is captured exactly when an encoder could ask for it. `Carries` is asked
    // before `Read` for the reason `RenderPrometheus` gives -- a skewed sink answers `Read`
    // with a zero it cannot mean.
    for (auto const& row: CounterTable)
    {
        if (metrics.Carries(row.counter))
            reading.counters[static_cast<std::size_t>(row.counter)] = metrics.Read(row.counter);
    }
    return reading;
}

} // namespace FastCache

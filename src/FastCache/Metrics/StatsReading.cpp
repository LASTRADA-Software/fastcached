// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Version.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <cstddef>

namespace FastCache
{

StatsReading CaptureStatsReading(IMetricsSink const& metrics,
                                 MetricsSnapshot const& snapshot,
                                 std::span<MetricsSurface const> surfaces)
{
    StatsReading reading { .counters = {}, .snapshot = snapshot, .version = std::string { VersionString } };
    // Driven by the catalogue, not by the enum's range: the catalogue is what every encoding
    // walks, so a row is captured exactly when an encoder could ask for it. `Carries` is asked
    // before `Read` for the reason `RenderPrometheus` gives -- a skewed sink answers `Read`
    // with a zero it cannot mean.
    for (auto const& row: CounterTable)
    {
        auto* const cell = reading.counters.Find(row.counter);
        if (cell == nullptr)
            continue;
        // The ORDER is the one `CounterAbsence` declares, and it is not arbitrary: a build that
        // cannot represent the row AT ALL outranks a process that merely never writes it. Asked
        // the other way round, a skewed build serving no cache would report its skew as an
        // ordinary component absence and nothing would count it.
        if (!metrics.Carries(row.counter))
            *cell = CounterReading::None(CounterAbsence::NoSlotInThisBuild);
        else if (!CounterHasAWriterIn(row.counter, surfaces))
            *cell = CounterReading::None(CounterAbsence::NoWriterInThisProcess);
        else
            *cell = CounterReading::Of(metrics.Read(row.counter));
    }
    return reading;
}

std::vector<std::string_view> CatalogueRowsWithoutASlot(IMetricsSink const& metrics)
{
    auto const reading = CaptureStatsReading(metrics, MetricsSnapshot {}, EverySurface);
    std::vector<std::string_view> rows;
    for (auto const& row: CounterTable)
    {
        // A null cell is a row the reading itself cannot hold, which `RenderPrometheus` counts as a
        // skew too; asking only `Why()` would miss the half of the skew that direction produces.
        auto const* const cell = reading.counters.Find(row.counter);
        if (cell == nullptr || (!cell->Present() && cell->Why() == CounterAbsence::NoSlotInThisBuild))
            rows.push_back(row.prometheusName);
    }
    return rows;
}

} // namespace FastCache

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>

#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>

namespace FastCache::Testing
{

/// @file CounterMovement.hpp
/// Which counters a sink moved, by their exported NAMES, for a case asserting that something was
/// counted -- or that nothing was.
///
/// By name and as text because that is what a failed comparison has to print: a map keyed by the
/// enumerator prints `{?}` for every row in Catch2, so a red "nothing moved" said nothing about
/// what had.

/// Every counter's reading, keyed by its exported name -- EVERY counter, so one that moved and was
/// not expected is in the answer as surely as one that was.
///
/// **Keyed by name, so it relies on every exported name being unique**: two counters sharing one
/// would collapse into a single row and a movement of either could hide behind the other. That is
/// pinned by "A counter's exported name is unique" (`Metrics/PrometheusFormatter_test.cpp`).
using CounterReadings = std::map<std::string_view, std::uint64_t>;

/// The name a counter is exported under.
/// @param counter A counter.
/// @return Its exported name, through the catalogue's one lookup; a counter with no row reads as a
///         name nobody could expect.
[[nodiscard]] inline std::string_view CounterName(IMetricsSink::Counter counter)
{
    auto const* const row = DescriptorOf(counter);
    return row != nullptr ? row->prometheusName : std::string_view { "<no catalogue row>" };
}

/// Read every counter of @p metrics once.
/// @param metrics The sink.
/// @return One reading per counter, by name.
[[nodiscard]] inline CounterReadings CounterReadingsOf(IMetricsSink const& metrics)
{
    CounterReadings readings;
    for (auto const counter: Enumerators<IMetricsSink::Counter>())
        readings.emplace(CounterName(counter), metrics.Read(counter));
    return readings;
}

/// The counters @p metrics moved since @p before was read, as text a failed assertion prints.
/// @param before What `CounterReadingsOf` read earlier, from the same sink.
/// @param metrics The sink now.
/// @return One `name +delta` line per counter that moved, in name order; empty when none did.
///         Assert it EMPTY under an `INFO` carrying it, so a red line names what moved.
[[nodiscard]] inline std::string CountersMoved(CounterReadings const& before, IMetricsSink const& metrics)
{
    std::string moved;
    for (auto const& [name, reading]: CounterReadingsOf(metrics))
        if (auto const was = before.at(name); reading != was)
            moved += std::format("{} +{}\n", name, reading - was);
    return moved;
}

} // namespace FastCache::Testing

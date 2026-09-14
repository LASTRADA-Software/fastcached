// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliValue.hpp"
#include "StatsSource.hpp"

#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <vector>

namespace FastCache::Cli::Testing
{

/// @file ScrapeFixture.hpp
/// A `/metrics` record for a test, made the way a live session gets one.
///
/// **Rendered by the daemon's own formatter from a model and read back by the client's own parser.** A
/// hand-written list of series can hold a record no daemon sends -- a cache block with half its series, a host
/// with one gauge -- and the live model (`StatsReading`) cannot state either, so a fixture built that way tests
/// a reading production never produces. One helper, so every fixture that needs a scrape gets the same one: a
/// hand-rolled copy is how a builder drifts from the thing it stands for.

/// The fields a scrape of @p reading parses into, in the order the formatter renders them.
/// @param reading The model to render.
/// @return The record's fields.
[[nodiscard]] inline std::vector<Field> ScrapeOf(StatsReading const& reading)
{
    return ParsePrometheus(RenderPrometheus(reading)).fields;
}

} // namespace FastCache::Cli::Testing

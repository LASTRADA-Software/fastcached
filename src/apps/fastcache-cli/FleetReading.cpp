// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "SocketExchange.hpp"

#include <format>
#include <string>
#include <utility>

namespace FastCache::Cli
{

SampleReading ReadFleetSample(DashboardEvent const& event)
{
    if (!event.document.has_value())
        // A fleet sample that carried no document was composed wrongly; the reading says so
        // rather than inventing a fleet.
        return SampleReading {
            .outcome = Outcome::Unreachable, .value = {}, .source = {}, .note = "the sample carried no fleet document"
        };

    auto const& fetched = *event.document;
    if (!fetched.has_value())
        return SampleReading {
            .outcome = OutcomeOf(fetched.error().kind), .value = {}, .source = {}, .note = fetched.error().detail
        };

    // Validation only: see the header for why the parsed document is not kept.
    if (auto const parsed = ParseFleetDocument(*fetched); !parsed.has_value())
        return SampleReading { .outcome = Outcome::Protocol,
                               .value = {},
                               .source = {},
                               .note = std::format("the leader's fleet document could not be read: {}", parsed.error()) };

    return SampleReading { .outcome = Outcome::Affirmative,
                           .value = ScalarValue(TextCell(*fetched)),
                           .source = std::string { FleetReadingSource },
                           .note = {} };
}

} // namespace FastCache::Cli

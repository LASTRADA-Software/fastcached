// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "SocketExchange.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

    auto parsed = ParseFleetDocument(*fetched);
    if (!parsed.has_value())
        return SampleReading { .outcome = Outcome::Protocol,
                               .value = {},
                               .source = {},
                               .note = std::format("the leader's fleet document could not be read: {}", parsed.error()) };

    return SampleReading { .outcome = Outcome::Affirmative,
                           .value = ScalarValue(TextCell(*fetched)),
                           .source = std::string { FleetReadingSource },
                           .note = {},
                           .document = std::make_shared<FleetDocument const>(std::move(*parsed)) };
}

Value FleetKpiFigures(DashboardModel const& model)
{
    auto fields = std::vector<Field> {};
    fields.push_back(Field { .name = "source",
                             .value = model.latestStamp.has_value() ? TextCell(model.latestStamp->source) : AbsentCell() });
    if (!model.latest.has_value() || model.latest->shape != Shape::Scalar)
        return RecordValue(std::move(fields));

    // A reading was validated when it was read, so a document refused here is one this build's
    // parser has changed its mind about -- drawn as nothing rather than as a guess.
    auto const document = ParseFleetDocument(model.latest->scalar.lexical);
    auto const* const kpi = document.has_value() ? document->Section(FleetSection::Kpi) : nullptr;
    if (kpi == nullptr)
        return RecordValue(std::move(fields));

    auto const column = [kpi](std::string_view name) {
        return static_cast<std::size_t>(std::ranges::find(kpi->columns, name) - kpi->columns.begin());
    };
    auto const nameAt = column("kpi");
    auto const valueAt = column("value");
    if (nameAt >= kpi->columns.size() || valueAt >= kpi->columns.size())
        return RecordValue(std::move(fields));
    for (auto const& row: kpi->rows)
        fields.push_back(Field { .name = row[nameAt].lexical, .value = row[valueAt] });
    return RecordValue(std::move(fields));
}

} // namespace FastCache::Cli

// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "SocketExchange.hpp"

#include <FastCache/Core/NumericText.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
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
    auto const ofAt = column("of");
    if (nameAt >= kpi->columns.size() || valueAt >= kpi->columns.size())
        return RecordValue(std::move(fields));

    // The parser reads every cell as text; a stream is read by a program, so a figure that is a
    // finite number is one -- `jq`'s numeric comparisons and `human`'s right alignment both key on it.
    auto const figure = [](Cell cell) {
        auto parsed = 0.0;
        if (cell.kind == CellKind::Text && ParseFiniteDouble(cell.lexical, parsed))
            cell.kind = CellKind::Number;
        return cell;
    };
    for (auto const& row: kpi->rows)
    {
        fields.push_back(Field { .name = row[nameAt].lexical, .value = figure(row[valueAt]) });
        if (ofAt < kpi->columns.size() && row[ofAt].kind != CellKind::Absent)
            fields.push_back(Field { .name = std::format("{}-of", row[nameAt].lexical), .value = figure(row[ofAt]) });
    }
    return RecordValue(std::move(fields));
}

} // namespace FastCache::Cli

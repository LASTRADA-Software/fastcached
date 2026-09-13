// SPDX-License-Identifier: Apache-2.0
#include "FleetChartModel.hpp"
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "SocketExchange.hpp"

#include <FastCache/Core/NumericText.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    /// The `kpi` section of @p document as fields: one per figure, `<kpi>-of` after one counted
    /// against something. None when the document carries no readable strip.
    /// @param document The parsed document.
    /// @return The fields.
    [[nodiscard]] std::vector<Field> KpiFields(FleetDocument const& document)
    {
        auto fields = std::vector<Field> {};
        auto const* const kpi = document.Section(FleetSection::Kpi);
        if (kpi == nullptr)
            return fields;

        auto const column = [kpi](std::string_view name) {
            return static_cast<std::size_t>(std::ranges::find(kpi->columns, name) - kpi->columns.begin());
        };
        auto const nameAt = column("kpi");
        auto const valueAt = column("value");
        auto const ofAt = column("of");
        if (nameAt >= kpi->columns.size() || valueAt >= kpi->columns.size())
            return fields;

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
        return fields;
    }
} // namespace

SampleReading ReadFleetSample(DashboardEvent const& event)
{
    return ReadFleetSampleThrough(event, &ParseFleetDocument);
}

SampleReading ReadFleetSampleThrough(DashboardEvent const& event, FleetParser parse)
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

    auto parsed = parse(*fetched);
    if (!parsed.has_value())
        return SampleReading { .outcome = Outcome::Protocol,
                               .value = {},
                               .source = {},
                               .note = std::format("the leader's fleet document could not be read: {}", parsed.error()) };

    auto reading = SampleReading { .outcome = Outcome::Affirmative,
                                   .value = RecordValue(KpiFields(*parsed)),
                                   .source = std::string { FleetReadingSource },
                                   .note = {},
                                   .document = nullptr,
                                   .points = FleetChartPoints(*parsed) };
    // The points are taken from the parse before it moves: the chart's history keeps these numbers,
    // and only the newest document is kept whole.
    reading.document = std::make_shared<FleetDocument const>(std::move(*parsed));
    return reading;
}

Value FleetKpiFigures(DashboardModel const& model)
{
    auto fields = std::vector<Field> {};
    fields.push_back(Field { .name = "source",
                             .value = model.latestStamp.has_value() ? TextCell(model.latestStamp->source) : AbsentCell() });
    // The reader made the strip a record when it parsed the document; nothing is parsed here.
    if (model.latest.has_value() && model.latest->shape == Shape::Record)
        std::ranges::copy(model.latest->fields, std::back_inserter(fields));
    return RecordValue(std::move(fields));
}

} // namespace FastCache::Cli

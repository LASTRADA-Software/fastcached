// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "SocketExchange.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Distributed;

namespace
{

/// A `fleet` sample carrying @p fetched.
/// @param fetched What the document fetch produced.
/// @return The event.
[[nodiscard]] DashboardEvent FleetSample(std::expected<std::string, AdminError> fetched)
{
    return DashboardEvent { .kind = DashboardEventKind::Sample, .document = std::move(fetched) };
}

/// A leader's whole document, from the leader's own renderer.
/// @return The document.
[[nodiscard]] std::string LeaderDocument()
{
    auto snapshot = FleetSnapshot {};
    snapshot.role = SchedulerRole::Leader;
    return RenderFleetText(snapshot, FleetHistoryView {}, std::nullopt);
}

} // namespace

TEST_CASE("a fleet reading is the leader's whole document as text, parsed only to be sure it can be",
          "[cli][fleet][reading]")
{
    auto const document = LeaderDocument();
    auto const reading = ReadFleetSample(FleetSample(document));

    REQUIRE(reading.outcome == Outcome::Affirmative);
    CHECK(reading.source == FleetReadingSource);
    CHECK(reading.note.empty());
    // The TEXT, verbatim: whoever draws it parses this same text, so it must be the document.
    REQUIRE(reading.value.shape == Shape::Scalar);
    CHECK(reading.value.scalar.lexical == document);
    CHECK(ParseFleetDocument(reading.value.scalar.lexical).has_value());
}

TEST_CASE("a fleet reading hands over the document it parsed, and a refused one hands over none", "[cli][fleet][reading]")
{
    // Parsed once, here, so a panel draws the parse instead of parsing again every frame. WHAT
    // DISTINGUISHES: the document arrives AND is this text's parse -- every section the text's own
    // parse carries is carried, table for table -- and a sample that is not a reading brings none,
    // so a reader handing over whatever it had parsed would fail the second half.
    auto const text = LeaderDocument();
    auto const reading = ReadFleetSample(FleetSample(text));
    REQUIRE(reading.outcome == Outcome::Affirmative);
    REQUIRE(reading.document != nullptr);

    auto const reparsed = ParseFleetDocument(text);
    REQUIRE(reparsed.has_value());
    auto sections = std::size_t { 0 };
    for (auto const& row: FleetSectionTable)
    {
        INFO("section " << row.key);
        auto const* handed = reading.document->Section(row.section);
        auto const* expected = reparsed->Section(row.section);
        REQUIRE((handed == nullptr) == (expected == nullptr));
        if (expected == nullptr)
            continue;
        CHECK(handed->columns == expected->columns);
        CHECK(handed->rows.size() == expected->rows.size());
        ++sections;
    }
    CHECK(sections > 0);

    auto snapshot = FleetSnapshot {};
    snapshot.role = SchedulerRole::Follower;
    snapshot.leaderEndpoint = "10.0.0.9:7071";
    CHECK(ReadFleetSample(FleetSample(RenderFleetText(snapshot, FleetHistoryView {}, std::nullopt))).document == nullptr);
    CHECK(
        ReadFleetSample(FleetSample(std::unexpected(AdminError { .kind = AdminFailure::Refused, .detail = "no" }))).document
        == nullptr);
}

TEST_CASE("a fleet document that does not parse is a protocol failure naming what was refused", "[cli][fleet][reading]")
{
    // A follower that answered 200 with its notes: no section, so no claim about the fleet.
    auto snapshot = FleetSnapshot {};
    snapshot.role = SchedulerRole::Follower;
    snapshot.leaderEndpoint = "10.0.0.9:7071";
    auto const followerNotes = RenderFleetText(snapshot, FleetHistoryView {}, std::nullopt);

    for (auto const& document: { followerNotes, std::string { "# machines\nendpoint\tname\nonly-one\n" } })
    {
        auto const reading = ReadFleetSample(FleetSample(document));
        CHECK(reading.outcome == Outcome::Protocol);
        CHECK(reading.note.contains("fleet document could not be read"));
        CHECK(reading.value.shape == Shape::Empty);
    }
}

TEST_CASE("a fleet fetch that produced no document keeps the fetch's own outcome and words", "[cli][fleet][reading]")
{
    auto const refused = ReadFleetSample(FleetSample(
        std::unexpected(AdminError { .kind = AdminFailure::Refused, .detail = "not the leader; ask 10.0.0.9:7071" })));
    CHECK(refused.outcome == Outcome::Refused);
    CHECK(refused.note.contains("10.0.0.9:7071"));

    auto const silent = ReadFleetSample(
        FleetSample(std::unexpected(AdminError { .kind = AdminFailure::Unreachable, .detail = "connection refused" })));
    CHECK(silent.outcome == Outcome::Unreachable);
    CHECK(silent.note.contains("connection refused"));

    auto const composedWrongly = ReadFleetSample(DashboardEvent { .kind = DashboardEventKind::Sample });
    CHECK(composedWrongly.outcome != Outcome::Affirmative);
}

TEST_CASE("a piped fleet record is the newest reading's KPI strip, parsed from its text", "[cli][fleet][reading]")
{
    auto const document = LeaderDocument();
    auto model = DashboardModel {};
    model.latest = ReadFleetSample(FleetSample(document)).value;
    model.latestStamp = ReadingStamp { .at = {}, .source = std::string { FleetReadingSource } };

    auto const record = FleetKpiFigures(model);
    REQUIRE(record.shape == Shape::Record);
    REQUIRE_FALSE(record.fields.empty());
    CHECK(record.fields[0].name == "source");
    CHECK(record.fields[0].value.lexical == FleetReadingSource);

    // The strip as the leader writes it on its own: read through the one-section form rather than the
    // whole document, so a record taking the wrong column or dropping a row cannot agree with it.
    auto snapshot = FleetSnapshot {};
    snapshot.role = SchedulerRole::Leader;
    auto const strip = FleetTable(RenderFleetText(snapshot, FleetHistoryView {}, FleetSection::Kpi));
    REQUIRE(strip.has_value());
    auto const at = [&strip](std::string_view name) {
        auto const found = std::ranges::find(strip->columns, name);
        REQUIRE(found != strip->columns.end());
        return static_cast<std::size_t>(found - strip->columns.begin());
    };
    auto const kpiAt = at("kpi");
    auto const valueAt = at("value");
    auto const ofAt = at("of");

    // Every figure in the leader's order, each followed by `<kpi>-of` exactly where the strip has one.
    auto expected = std::vector<std::string> { "source" };
    for (auto const& row: strip->rows)
    {
        expected.push_back(row[kpiAt].lexical);
        if (row[ofAt].kind != CellKind::Absent)
            expected.push_back(row[kpiAt].lexical + "-of");
    }
    auto names = std::vector<std::string> {};
    for (auto const& field: record.fields)
        names.push_back(field.name);
    CHECK(names == expected);
    REQUIRE(strip->rows.size() == FleetKpiKeys().size());

    // Each value is the strip's own lexical form, and a number where it reads as one.
    auto readsAsNumber = false;
    for (auto const& row: strip->rows)
    {
        auto const* const field = FindField(record, row[kpiAt].lexical);
        REQUIRE(field != nullptr);
        CHECK(field->value.lexical == row[valueAt].lexical);
        if (row[valueAt].kind != CellKind::Absent)
        {
            CHECK(field->value.kind == CellKind::Number);
            readsAsNumber = true;
        }
    }
    CHECK(readsAsNumber);

    // Before any reading there is only the source, and it is absent.
    auto const empty = FleetKpiFigures(DashboardModel {});
    REQUIRE(empty.fields.size() == 1);
    CHECK(empty.fields[0].value.kind == CellKind::Absent);
}

TEST_CASE("an admin fetch's failure is one outcome for every reader", "[cli][fleet][reading]")
{
    CHECK(OutcomeOf(AdminFailure::Refused) == Outcome::Refused);
    CHECK(OutcomeOf(AdminFailure::Unreachable) == Outcome::Unreachable);
}

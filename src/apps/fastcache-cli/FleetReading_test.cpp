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
#include <string_view>
#include <utility>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Distributed;

namespace
{

/// Where every fleet sample here says its stream answered: the leader it was dialled at.
constexpr auto Leader = std::string_view { "10.0.0.9:7071" };

/// A `fleet` sample carrying @p document, as the leader's stream pushed it.
/// @param document The document, whole.
/// @return The event.
[[nodiscard]] DashboardEvent FleetSample(std::string document)
{
    return DashboardEvent { .kind = DashboardEventKind::Sample,
                            .document = std::move(document),
                            .where = std::string { Leader } };
}

/// A `fleet` sample that carried no document: a composition fault, since every fleet reading is pushed with one.
/// @return The event.
[[nodiscard]] DashboardEvent DocumentlessSample()
{
    return DashboardEvent { .kind = DashboardEventKind::Sample, .where = std::string { Leader } };
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

TEST_CASE("a fleet reading's value is the leader's KPI strip, never the document's text", "[cli][fleet][reading]")
{
    // The value is what the model's HISTORY keeps for every sample. WHAT DISTINGUISHES: it is a record
    // of the strip's figures -- one field per `kpi` row, plus `-of` fields -- and no field holds the
    // document's text, so 256 samples of history are 256 small records rather than 256 documents.
    auto const document = LeaderDocument();
    auto const reading = ReadFleetSample(FleetSample(document));

    REQUIRE(reading.outcome == Outcome::Affirmative);
    CHECK(reading.note.empty());
    REQUIRE(reading.value.shape == Shape::Record);
    CHECK(reading.value.fields.size() >= FleetKpiKeys().size());
    for (auto const key: FleetKpiKeys())
        CHECK(FindField(reading.value, key) != nullptr);
    for (auto const& field: reading.value.fields)
        CHECK(field.value.lexical.size() < document.size() / 4);
}

TEST_CASE("a fleet reading names the subscription it came from and the leader that pushed it", "[cli][fleet][reading]")
{
    // A panel's source line and the run rule both read these. WHAT DISTINGUISHES: `where` is the SAMPLE's -- the
    // endpoint the stream was dialled at, which after a redirect is the leader rather than `--addr` -- so a reader
    // that named a constant, or dropped it, fails; and the source and route are the subscription's, never a fetch's.
    auto const reading = ReadFleetSample(FleetSample(LeaderDocument()));
    REQUIRE(reading.outcome == Outcome::Affirmative);
    CHECK(reading.source == SubscriptionSource);
    CHECK(reading.route == SubscriptionRoute);
    CHECK(reading.where == Leader);
    CHECK(reading.role == LeaderRole);

    auto moved = FleetSample(LeaderDocument());
    moved.where = "10.0.0.10:7071";
    CHECK(ReadFleetSample(moved).where == "10.0.0.10:7071");
}

TEST_CASE("a fleet reading hands over the document it parsed, and a sample that is not a reading hands over none",
          "[cli][fleet][reading]")
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
    CHECK(ReadFleetSample(DocumentlessSample()).document == nullptr);
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

TEST_CASE("a fleet sample that carried no document is unreachable, never a fleet of nothing", "[cli][fleet][reading]")
{
    // Whether the leader could be reached, or refused the watcher, is the source's to say, as a failed sample; a
    // `Sample` with no document can only be one composed wrongly. WHAT DISTINGUISHES: `Unreachable` and its own
    // words, where a document that does not parse is `Protocol` -- a reader that parsed an empty text would answer
    // `Protocol` here, and one that drew an empty fleet would answer `Affirmative`.
    auto const composedWrongly = ReadFleetSample(DocumentlessSample());
    CHECK(composedWrongly.outcome == Outcome::Unreachable);
    CHECK(composedWrongly.note.contains("carried no fleet document"));
    CHECK(composedWrongly.value.shape == Shape::Empty);
}

TEST_CASE("a piped fleet record is the newest reading's KPI strip, with its source first", "[cli][fleet][reading]")
{
    auto const document = LeaderDocument();
    auto model = DashboardModel {};
    model.latest = ReadFleetSample(FleetSample(document)).value;
    model.latestStamp = ReadingStamp { .at = {}, .source = std::string { SubscriptionSource } };

    auto const record = FleetKpiFigures(model);
    REQUIRE(record.shape == Shape::Record);
    REQUIRE_FALSE(record.fields.empty());
    CHECK(record.fields[0].name == "source");
    CHECK(record.fields[0].value.lexical == SubscriptionSource);

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

namespace
{

/// How many times `CountingParse` has parsed a document. A test counter, reset by the case that reads it.
std::size_t parsesTaken = 0;

/// `ParseFleetDocument`, counted.
/// @param document The text.
/// @return The parse.
[[nodiscard]] std::expected<FleetDocument, std::string> CountingParse(std::string_view document)
{
    ++parsesTaken;
    return ParseFleetDocument(document);
}

} // namespace

TEST_CASE("a fleet sample is parsed exactly once by its reader", "[cli][fleet][reading]")
{
    // The seam the parse-once property is measured through. WHAT DISTINGUISHES: one call reads one
    // parse, a sample with no document reads none -- and the piped record drawn from that reading parses nothing
    // more, so a record that re-parsed the text would move the count.
    parsesTaken = 0;
    auto const reading = ReadFleetSampleThrough(FleetSample(LeaderDocument()), &CountingParse);
    REQUIRE(reading.outcome == Outcome::Affirmative);
    CHECK(parsesTaken == 1);

    auto model = DashboardModel {};
    model.latest = reading.value;
    model.latestStamp = ReadingStamp { .at = {}, .source = std::string { SubscriptionSource } };
    auto const record = FleetKpiFigures(model);
    CHECK(record.fields.size() == reading.value.fields.size() + 1);
    CHECK(parsesTaken == 1);

    (void) ReadFleetSampleThrough(DocumentlessSample(), &CountingParse);
    CHECK(parsesTaken == 1);
}

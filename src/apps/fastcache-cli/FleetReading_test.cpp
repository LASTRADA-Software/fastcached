// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"
#include "FleetReading.hpp"
#include "SocketExchange.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <optional>
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

TEST_CASE("an admin fetch's failure is one outcome for every reader", "[cli][fleet][reading]")
{
    CHECK(OutcomeOf(AdminFailure::Refused) == Outcome::Refused);
    CHECK(OutcomeOf(AdminFailure::Unreachable) == Outcome::Unreachable);
}

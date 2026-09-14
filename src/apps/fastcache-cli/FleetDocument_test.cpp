// SPDX-License-Identifier: Apache-2.0
#include "FleetDocument.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Distributed;

namespace
{

/// An empty fleet, as the node that leads it describes it.
/// @return The snapshot.
[[nodiscard]] FleetSnapshot Leader()
{
    auto snapshot = FleetSnapshot {};
    snapshot.role = SchedulerRole::Leader;
    return snapshot;
}

/// A leader's whole document, rendered by the leader's own renderer from an empty fleet.
///
/// The renderer rather than a literal, so the parser is held to what `/fleet.txt` actually
/// writes: a section this build adds, a marker spelled differently or a header the KPI table
/// grows reaches this case the day it reaches the leader.
/// @return The document.
[[nodiscard]] std::string LeaderDocument()
{
    return RenderFleetText(Leader(), FleetHistoryView {}, std::nullopt);
}

/// Every column name of a table, in order.
/// @param table The table.
/// @return The names.
[[nodiscard]] std::vector<std::string> ColumnsOf(Value const& table)
{
    return table.columns;
}

} // namespace

TEST_CASE("the whole fleet document reads back one table per section the leader rendered", "[cli][fleet][document]")
{
    auto const document = LeaderDocument();
    auto const parsed = ParseFleetDocument(document);
    REQUIRE(parsed.has_value());

    // Every section the table names, each carrying exactly the header its own section form
    // renders: the whole document and `?section=` are two walks one parser must agree with.
    auto checked = std::size_t { 0 };
    auto absent = std::size_t { 0 };
    for (auto const& row: FleetSectionTable)
    {
        INFO("section " << row.key);
        auto const* const table = parsed->Section(row.section);
        // The history is not in the whole document; it is asked for by name.
        if (!row.inWhole)
        {
            CHECK(table == nullptr);
            ++absent;
            continue;
        }
        REQUIRE(table != nullptr);
        auto const alone = FleetTable(RenderFleetText(Leader(), FleetHistoryView {}, row.section));
        REQUIRE(alone.has_value());
        CHECK(ColumnsOf(*table) == ColumnsOf(*alone));
        CHECK(table->rows.size() == alone->rows.size());
        ++checked;
    }
    CHECK(checked + absent == FleetSectionTable.size());
    CHECK(checked > 1);

    // The strip arrives computed, one row per figure (#1302).
    auto const* const kpi = parsed->Section(FleetSection::Kpi);
    REQUIRE(kpi != nullptr);
    CHECK(kpi->rows.size() == FleetKpiKeys().size());
}

TEST_CASE("a follower's all-comment answer is not a fleet document", "[cli][fleet][document]")
{
    // A follower writes only comments, so that a reader stripping them is left with nothing;
    // an empty fleet would be a claim about the fleet, which a follower cannot make.
    auto snapshot = FleetSnapshot {};
    snapshot.role = SchedulerRole::Follower;
    snapshot.leaderEndpoint = "10.0.0.9:7071";
    auto const text = RenderFleetText(snapshot, FleetHistoryView {}, std::nullopt);
    REQUIRE(text.starts_with('#'));

    auto const parsed = ParseFleetDocument(text);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().contains("no fleet section"));
}

TEST_CASE("a fleet document refuses what it cannot attribute to a section", "[cli][fleet][document]")
{
    auto const outside = ParseFleetDocument("endpoint\tname\n# machines\nendpoint\tname\n");
    REQUIRE_FALSE(outside.has_value());
    CHECK(outside.error().contains("outside any section"));

    auto const twice = ParseFleetDocument("# machines\nendpoint\n\n# machines\nendpoint\n");
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().contains("appears twice"));

    // A malformed section names itself, and the refusal is the one-section parser's.
    auto const ragged = ParseFleetDocument("# kpi\nkpi\tvalue\tunit\tof\n# machines\nendpoint\tname\nonly-one\n");
    REQUIRE_FALSE(ragged.has_value());
    CHECK(ragged.error().contains("section `machines`"));
    CHECK(ragged.error().contains("row 1 carries 1 field"));

    auto const empty = ParseFleetDocument("");
    REQUIRE_FALSE(empty.has_value());
}

TEST_CASE("a fleet section this client does not know is read past, not refused", "[cli][fleet][document]")
{
    // A newer leader serving a section this build has no row for must not fail every sample:
    // the known sections either side of it still read, and its rows land in none of them.
    auto const parsed = ParseFleetDocument("# kpi\nkpi\tvalue\tunit\tof\ndispatched\t12\tcount\t-\n"
                                           "\n# later\ncolumn\tother\nrow\twith\n"
                                           "\n# machines\nendpoint\tname\nbuild-01:7070\tci-01\n");
    REQUIRE(parsed.has_value());
    auto const* const kpi = parsed->Section(FleetSection::Kpi);
    auto const* const machines = parsed->Section(FleetSection::Machines);
    REQUIRE(kpi != nullptr);
    REQUIRE(machines != nullptr);
    CHECK(kpi->rows.size() == 1);
    CHECK(machines->rows.size() == 1);
    CHECK(parsed->Section(FleetSection::Workers) == nullptr);
}

TEST_CASE("inside a fleet section a line starting with a hash is a row, not a comment", "[cli][fleet][document]")
{
    // A member id or a worker id is text a peer chose. The renderer writes no comment inside a
    // section, so a leading `#` there is data, and dropping the row would under-report the fleet
    // in silence. Outside any section the same line is a comment, as a follower's notes are.
    auto const parsed = ParseFleetDocument("# a note before anything\n"
                                           "# members\nid\tendpoint\n#node-7\t10.0.0.7:7071\n# node-8\t10.0.0.8:7071\n"
                                           "node-9\t10.0.0.9:7071\n");
    REQUIRE(parsed.has_value());
    auto const* const members = parsed->Section(FleetSection::Members);
    REQUIRE(members != nullptr);
    REQUIRE(members->rows.size() == 3);
    CHECK(members->rows[0][0].lexical == "#node-7");
    // Spelled exactly like a marker up to its tab: a key never holds whitespace, so this is a row.
    CHECK(members->rows[1][0].lexical == "# node-8");
}

TEST_CASE("one fleet section reads the dash as absent and keeps the leader's escaping", "[cli][fleet][document]")
{
    auto const table = FleetTable("endpoint\tname\nbuild-01:7070\t-\nbuild-02:7070\tci\\t02\n");
    REQUIRE(table.has_value());
    REQUIRE(table->rows.size() == 2);
    CHECK(table->rows[0][1].kind == CellKind::Absent);
    CHECK(table->rows[1][1].lexical == "ci\\t02");
}

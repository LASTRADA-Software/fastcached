// SPDX-License-Identifier: Apache-2.0
#include "DashboardPanel.hpp"
#include "DashboardPanels.hpp"
#include "DashboardRig.hpp"
#include "ScriptedCellWidth.hpp"
#include "TerminalCellWidth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;

namespace
{

/// A CJK character, U+7DE8: three bytes, one code point, two cells.
constexpr std::string_view Wide = "\xe7\xb7\xa8";

/// `e` and U+0301 COMBINING ACUTE ACCENT: three bytes, two code points, one cell.
constexpr std::string_view Combining = "e\xcc\x81";

/// U+FF21 FULLWIDTH LATIN CAPITAL LETTER A: three bytes, two cells.
constexpr std::string_view Fullwidth = "\xef\xbc\xa1";

} // namespace

TEST_CASE("the terminal's cell width counts cells, not bytes or code points", "[cli][dashboard][width]")
{
    // The production binding, asked directly. WHAT DISTINGUISHES: each fixture's cells differ from
    // its bytes, and the combining one differs from its code points too -- so a function counting
    // either gives a different answer on at least one row.
    struct Row
    {
        std::string_view text;
        std::size_t cells;
    };
    auto const rows = std::vector<Row> {
        { .text = Wide, .cells = 2 }, { .text = Combining, .cells = 1 }, { .text = Fullwidth, .cells = 2 },
        { .text = "ab", .cells = 2 }, { .text = "", .cells = 0 },
    };
    auto checked = std::size_t { 0 };
    for (auto const& row: rows)
    {
        INFO("fixture of " << row.text.size() << " bytes");
        CHECK(TerminalCellWidth(row.text) == row.cells);
        // The test fake must agree with production on every range it claims to model, or the layout
        // tests measure a terminal nobody has.
        CHECK(FakeCellWidth(row.text) == TerminalCellWidth(row.text));
        ++checked;
    }
    CHECK(checked == rows.size());
    CHECK(TerminalCellWidth(std::string { Wide } + std::string { Combining } + "x") == 4);
    CHECK(Wide.size() != 2);
    CHECK(Combining.size() != 1);
}

TEST_CASE("a panel laid out through the terminal's cell width fits the terminal exactly", "[cli][dashboard][width]")
{
    // The binding in use rather than in isolation. WHAT DISTINGUISHES: the endpoint is wider in bytes
    // than in cells, so a frame measured any other way puts its right corner somewhere else -- every
    // line is exactly the terminal's width by the production function.
    auto const endpoint = std::string { Wide } + std::string { Wide } + "-07:7070";
    REQUIRE(TerminalCellWidth(endpoint) == 12);
    REQUIRE(endpoint.size() != 12);

    auto view = PanelView {
        CachePanel(),
        PanelContext { .absent = "n/a", .endpoint = endpoint, .cellWidth = &TerminalCellWidth, .rung = RenderRung::Unicode }
    };
    auto sink = CollectingSink {};
    (void) Drive({ DashboardEvent { .kind = DashboardEventKind::Resize, .columns = 60, .rows = 30 },
                   DashboardEvent { .kind = DashboardEventKind::Tick } },
                 DashboardLimits {},
                 view,
                 sink);
    REQUIRE(sink.frames.size() == 1);
    CHECK(sink.frames[0].contains(endpoint));
    auto lines = std::size_t { 0 };
    for (auto const line: std::views::split(sink.frames[0], '\n'))
    {
        CHECK(TerminalCellWidth(std::string_view { line.begin(), line.end() }) == 60);
        ++lines;
    }
    CHECK(lines > 2);
}

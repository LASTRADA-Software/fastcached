// SPDX-License-Identifier: Apache-2.0
#include "TerminalCapabilities.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace FastCache;
using namespace FastCache::Cli;

namespace
{

/// One record and the rung it must be drawn on.
struct RungRow
{
    SixelAnswer sixel;
    TerminalTextEncoding encoding;
    RenderRung rung;
};

/// Every combination, so a rule that forgot one is a row that fails rather than a case nobody wrote.
constexpr auto RungRows = std::to_array<RungRow>({
    { .sixel = SixelAnswer::Advertised, .encoding = TerminalTextEncoding::Utf8, .rung = RenderRung::Sixel },
    { .sixel = SixelAnswer::Advertised, .encoding = TerminalTextEncoding::Other, .rung = RenderRung::Sixel },
    { .sixel = SixelAnswer::Advertised, .encoding = TerminalTextEncoding::Unknown, .rung = RenderRung::Sixel },
    { .sixel = SixelAnswer::NotAdvertised, .encoding = TerminalTextEncoding::Utf8, .rung = RenderRung::Unicode },
    { .sixel = SixelAnswer::NotAdvertised, .encoding = TerminalTextEncoding::Other, .rung = RenderRung::Ascii },
    { .sixel = SixelAnswer::NotAdvertised, .encoding = TerminalTextEncoding::Unknown, .rung = RenderRung::Ascii },
    { .sixel = SixelAnswer::NoReply, .encoding = TerminalTextEncoding::Utf8, .rung = RenderRung::Unicode },
    { .sixel = SixelAnswer::NoReply, .encoding = TerminalTextEncoding::Other, .rung = RenderRung::Ascii },
    { .sixel = SixelAnswer::NoReply, .encoding = TerminalTextEncoding::Unknown, .rung = RenderRung::Ascii },
    { .sixel = SixelAnswer::NotAsked, .encoding = TerminalTextEncoding::Utf8, .rung = RenderRung::Unicode },
    { .sixel = SixelAnswer::NotAsked, .encoding = TerminalTextEncoding::Other, .rung = RenderRung::Ascii },
    { .sixel = SixelAnswer::NotAsked, .encoding = TerminalTextEncoding::Unknown, .rung = RenderRung::Ascii },
});

} // namespace

TEST_CASE("with a cell size, the rung is Sixel only when advertised, then Unicode only for UTF-8, and ASCII otherwise",
          "[cli][dashboard][rung]")
{
    // A terminal that did not answer DA1 is drawn exactly as one that answered without Sixel, and
    // an unknown encoding exactly as a known non-UTF-8 one: both fall to the floor that keeps every
    // figure. The record keeps them apart so a report can say which; the rung does not need to.
    for (auto const& row: RungRows)
    {
        CAPTURE(static_cast<int>(row.sixel), static_cast<int>(row.encoding));
        CHECK(ChooseRenderRung(TerminalCapabilities {
                  .sixel = row.sixel, .encoding = row.encoding, .cellPixels = CellPixelSize { .width = 10, .height = 20 } })
              == row.rung);
    }
}

TEST_CASE("an advertised Sixel with no cell size is not the Sixel rung", "[cli][dashboard][rung]")
{
    // An image is sized in pixels to cover cells. WHAT DISTINGUISHES: the same advertised Sixel is the
    // Sixel rung with a reported cell size and the ENCODING's rung without one -- Unicode for UTF-8,
    // ASCII otherwise -- never Sixel drawn from a guessed size.
    auto const measured = CellPixelSize { .width = 10, .height = 20 };
    for (auto const encoding: { TerminalTextEncoding::Utf8, TerminalTextEncoding::Other, TerminalTextEncoding::Unknown })
    {
        CAPTURE(static_cast<int>(encoding));
        auto const withSize =
            TerminalCapabilities { .sixel = SixelAnswer::Advertised, .encoding = encoding, .cellPixels = measured };
        auto const withoutSize = TerminalCapabilities { .sixel = SixelAnswer::Advertised, .encoding = encoding };
        CHECK(ChooseRenderRung(withSize) == RenderRung::Sixel);
        CHECK(ChooseRenderRung(withoutSize)
              == ChooseRenderRung(TerminalCapabilities { .sixel = SixelAnswer::NotAdvertised, .encoding = encoding }));
        CHECK(ChooseRenderRung(withoutSize) != RenderRung::Sixel);
    }
    // And a cell size alone draws no Sixel: the terminal still has to say it can.
    CHECK(ChooseRenderRung(TerminalCapabilities {
              .sixel = SixelAnswer::NotAdvertised, .encoding = TerminalTextEncoding::Utf8, .cellPixels = measured })
          == RenderRung::Unicode);
}

TEST_CASE("the rung is decided from the record alone, with nothing to ask", "[cli][dashboard][rung]")
{
    // A default record is what a caller holds before anything was asked, and it must not be read
    // as a finding that the terminal is poor: it lands on the ASCII floor like any unknown does.
    CHECK(ChooseRenderRung(TerminalCapabilities {}) == RenderRung::Ascii);
}

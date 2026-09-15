// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/FigureText.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace FastCache;

TEST_CASE("a share is written for a program to four decimals, an exact tie going to the even digit", "[core][figures]")
{
    // WHAT DISTINGUISHES: two binary fractions that land exactly halfway between two four-decimal answers. Half away
    // from zero writes 1/32 as `0.0313`; truncation writes 3/32 as `0.0937`. Only rounding to the even digit passes
    // both, so neither tie alone pins the rule.
    CHECK(WriteMachineFigure(1.0 / 32.0, FigureFormat::Percent) == "0.0312");
    CHECK(WriteMachineFigure(3.0 / 32.0, FigureFormat::Percent) == "0.0938");

    // The ends are written in full, so a column of shares lines up and a reader never meets a bare `1`.
    CHECK(WriteMachineFigure(1.0, FigureFormat::Percent) == "1.0000");
    CHECK(WriteMachineFigure(0.0, FigureFormat::Percent) == "0.0000");
    CHECK(WriteMachineFigure(3.0 / 4.0, FigureFormat::Percent) == "0.7500");
}

TEST_CASE("every other figure is written for a program without its unit and without grouping", "[core][figures]")
{
    // A whole number stays whole and ungrouped: `awk` sums `1284991`, and cannot sum `1 284 991`.
    CHECK(WriteMachineFigure(1'284'991.0, FigureFormat::Count) == "1284991");
    CHECK(WriteMachineFigure(3.0 * 1024.0 * 1024.0 * 1024.0, FigureFormat::Bytes) == "3221225472");
    CHECK(WriteMachineFigure(250.0, FigureFormat::Rate) == "250.000");
    CHECK(WriteMachineFigure(1.84, FigureFormat::Seconds) == "1.840");
}

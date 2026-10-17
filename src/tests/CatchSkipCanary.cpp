// SPDX-License-Identifier: Apache-2.0
//
// The shapes the exit contract `SKIP_RETURN_CODE 4` rests on (#1128, #1152).
//
// ctest is told a Catch2 skip is a skip by `SKIP_RETURN_CODE 4`, and that is exact only
// because of a contract Catch2 has kept since 3.7.1: a run whose every case skipped exits
// 4, and ANY assertion failure exits one fixed value, 42, whatever the count. Before that
// the exit code WAS the failed-assertion count, so a case failing exactly four assertions
// exited 4 and was scored *skipped* -- #1128, measured on 3.6.0 inside `100% tests passed`.
// The top-level CMakeLists pins Catch2 EXACTLY, because a version-compatible find takes
// whatever 3.x a machine has installed, older ones included.
//
// This binary exists so that contract has a READER. `check-catch-skip-exit-contract` runs
// every shape below in its own process and asserts what each one exits, because the
// contract is a property of the Catch2 BUILD the tree links, and a rule resting on one
// becomes false the day that build changes. It is deliberately NOT registered with
// `catch_discover_tests`: several cases fail on purpose.
//
// ## Why the shapes are still these
//
// They were collected (#1128, #1146) to defeat any PATTERN keyed on Catch2's output, and
// they still record why the mechanism is the exit code and never a pattern: the output is
// shared with text a case's author controls, a summary line changes SHAPE as its columns
// widen, and a case that both skips and fails prints `SKIPPED:` in its body while its
// summary says `1 failed`. A pattern has to survive every one of those; the exit contract
// survives them by construction, and these shapes are what shows it does.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>

// The compile-time half of the contract, against the Catch2 this binary links: the 4 every
// `catch_discover_tests` registration carries as `SKIP_RETURN_CODE` (and
// `check-catch-skip-return-code` requires) IS Catch2's all-skipped code, and no failure can
// spell it. The run-time half, that a failing case really exits with the failure code, is
// `check-catch-skip-exit-contract`'s.
static_assert(Catch::AllTestsSkippedExitCode == 4,
              "Catch2's all-tests-skipped exit code is no longer 4, so every `SKIP_RETURN_CODE 4` in this tree "
              "now scores a skip as a FAILURE -- #499 returning");
static_assert(Catch::TestFailureExitCode != Catch::AllTestsSkippedExitCode,
              "a failing run exits with the skip code again, so `SKIP_RETURN_CODE 4` scores failures as "
              "SKIPPED -- #1128 returning");

// The control: a case that passes exits 0, so a canary that exits non-zero for every shape
// is a broken binary rather than a contract that holds.
TEST_CASE("catch skip canary: a case that passes", "[canarypass]")
{
    CHECK(true);
}

// The shape every environment-conditional skip in this tree has. It must exit 4, or real
// skips start scoring as failures -- #499 returning.
TEST_CASE("catch skip canary: a case that skips", "[canaryskip]")
{
    SKIP("this canary skips on purpose");
}

// The shape #1128 was about. On 3.6.0 this exited **4** and was scored SKIPPED; under the
// contract it exits the failure code. Exactly four, because four was the collision.
TEST_CASE("catch skip canary: a case that fails four assertions", "[canaryfail]")
{
    CHECK(false);
    CHECK(false);
    CHECK(false);
    CHECK(false);
}

// One failure and three hundred: the failure code must not depend on the count. Three
// hundred is past the 255 the old count was clamped at, the other end of the old range.
TEST_CASE("catch skip canary: a case that fails one assertion", "[canaryfailone]")
{
    CHECK(false);
}

TEST_CASE("catch skip canary: a case that fails three hundred assertions", "[canaryfailmany]")
{
    for (auto const i: std::views::iota(0, 300))
    {
        CHECK(i < 0);
    }
}

// A case that BOTH skips and fails, which is what killed the obvious pattern: it prints
// `SKIPPED:` in its body and `1 failed` in its summary. It must exit the failure code --
// a skip somewhere in a case does not excuse a failure elsewhere in it.
TEST_CASE("catch skip canary: a case that skips and fails", "[canarymixed]")
{
    SECTION("skips")
    {
        SKIP("this section skips");
    }

    SECTION("fails")
    {
        CHECK(false);
        CHECK(false);
        CHECK(false);
        CHECK(false);
    }
}

// The shape the first version of this canary could not produce (#1146): at ten or more
// assertions the summary's first column is padded and the line changes SHAPE, gaining a
// `| 0 passed` column -- Catch2's zero-suppression tests `value != "0"`, and the padded
// `" 0"` is not `"0"`. Fatal to a pattern keyed on the narrow line; irrelevant to an exit
// code, which is the point. It must exit 4.
TEST_CASE("catch skip canary: a case that skips after ten or more assertions", "[canarypadskip]")
{
    for (auto const i: std::views::iota(0, 12))
    {
        CHECK(i >= 0);
    }

    SKIP("this canary skips after enough assertions to widen the summary column");
}

// Padding is UNBOUNDED, not one extra space: at a hundred-plus assertions the column widens
// again. It must exit 4.
//
//     12 assertions    test cases:  1 |  0 passed | 1 skipped     two spaces
//    120 assertions    test cases:   1 |   0 passed | 1 skipped   three
TEST_CASE("catch skip canary: a case that skips after a hundred assertions", "[canarywidepad]")
{
    for (auto const i: std::views::iota(0, 120))
    {
        CHECK(i >= 0);
    }

    SKIP("this canary skips after enough assertions to widen the column twice");
}

// The real-world shape: a skip in a LATER section, after earlier sections have run
// assertions into the same totals. It must exit 4.
TEST_CASE("catch skip canary: a skip in a later section after earlier ones ran", "[canarysectionskip]")
{
    SECTION("first section runs assertions")
    {
        for (auto const i: std::views::iota(0, 15))
        {
            CHECK(i >= 0);
        }
    }

    SECTION("a later section skips")
    {
        SKIP("this section skips after earlier sections ran assertions");
    }
}

// A failing case whose skip message echoes the old pattern's tail. Text an author writes
// reaches the output, never the exit code: it must exit the failure code.
TEST_CASE("catch skip canary: a failing case whose text echoes the pattern", "[canarydecoy]")
{
    SECTION("skips with a hostile message")
    {
        SKIP("a message mentioning 1 skipped on purpose");
    }

    SECTION("fails")
    {
        CHECK(false);
    }
}

// The adversarial shape (#1146): a case that only FAILS while carrying a WHOLE summary line,
// on one line, in an `INFO`. It decided that no unanchored pattern was usable; under the
// exit contract it must simply exit the failure code.
TEST_CASE("catch skip canary: a failing case echoing a whole summary line", "[canaryadversary]")
{
    std::string const report = "test cases: 1 | 1 skipped";
    INFO("reporting: " << report);
    CHECK(report.empty());
}

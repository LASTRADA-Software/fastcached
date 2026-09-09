// SPDX-License-Identifier: Apache-2.0
//
// The shapes any replacement skip mechanism must survive (#1128, #1152).
//
// ctest is told a Catch2 skip is a skip by `SKIP_RETURN_CODE 4`, and that is a defect
// we could not close: Catch2's exit code IS its failed-assertion count, so a case
// failing exactly four assertions is scored *skipped*. Three channels were measured
// and all three are closed -- the exit status is fully occupied, the output is shared
// with text the subject controls and cannot be anchored through the Windows command
// line, and `FAIL_REGULAR_EXPRESSION` does not outrank `SKIP_RETURN_CODE`. The argument
// is at the top of the top-level CMakeLists; the fix is a change of MECHANISM, #1152.
//
// This binary exists so that argument has a READER. `check-catch-skip-exit-collision`
// runs it and asserts the PREMISES rather than the conclusion, because a record saying
// something cannot be done instructs the next session not to try -- and would become a
// false rule the day Catch2 changed. It is deliberately NOT registered with
// `catch_discover_tests`: several cases fail on purpose.
//
// ## The structural property the whole design rests on
//
// `Totals::delta` puts a case in EXACTLY ONE bucket, and `failed` outranks `skipped`.
// So a case that both skips and fails can never report `1 skipped` in its SUMMARY, on
// any input -- which is why the summary line is a sound discriminator and Catch2's
// `SKIPPED:` marker is not. The marker survives in the BODY of a failing case at every
// width, measured at zero assertions and at padded.
//
// A property that holds on any input beats any number of measured shapes. The shapes
// below exist for the cases where no such property is available.
//
// ## Why adding shapes cannot make a PATTERN safe
//
// Learned the expensive way. A decoy carrying the pattern's TAIL refutes a tail-only
// pattern and certifies a same-line pattern as safe; a decoy carrying a WHOLE summary
// line then refutes that one. Every shape here is one somebody thought of, and the
// output channel is shared with an author who will think of others. That is the
// argument for a launcher over any further pattern work.
#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>

// The shape every environment-conditional skip in this tree has. Its output must MATCH
// the shared pattern, or real skips start scoring as failures -- #499 returning.
TEST_CASE("catch skip canary: a case that skips", "[canaryskip]")
{
    SKIP("this canary skips on purpose");
}

// The shape #1128 is about, and the reason the pattern may not be keyed on Catch2's
// exit code: this exits **4**, which is indistinguishable from a skip by status alone.
// Its output must NOT match the shared pattern.
//
// Exactly four, because four is the collision. Fewer or more would exit with a value
// no skip mechanism ever claimed, so the case would pass for a reason that says nothing
// about the defect.
TEST_CASE("catch skip canary: a case that fails four assertions", "[canaryfail]")
{
    CHECK(false);
    CHECK(false);
    CHECK(false);
    CHECK(false);
}

// A case that BOTH skips and fails, which is what killed the obvious candidate.
//
// Keying on Catch2's `SKIPPED:` marker rather than on the summary would score this
// SKIPPED -- and it fails four assertions. That would be WIDER than #1128: it hides any
// number of failures in a case that also skips, where #1128 hides exactly four. Its
// output must NOT match the shared pattern, and Catch2 itself already gets this right
// by printing `1 failed` in the summary while printing `SKIPPED:` above it.
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

// The shape the first version of this canary could not produce, and the reason a
// runtime check pointed at one input proves less than it looks (#1146).
//
// `SummaryColumn::addRow` left-pads a column to its widest row, and the first column
// carries the case count and the assertion count together. Below ten assertions both
// are one character wide and nothing moves; at ten or more the `1` becomes `" 1"`, and
// the line changes SHAPE rather than only spacing -- Catch2's zero-suppression tests
// `value != "0"`, and the padded `" 0"` is not `"0"`, so a `| 0 passed` column appears
// that is absent from the narrow form.
//
// A bare `SKIP()` with no prior assertions is the ONE shape where neither happens,
// and it was the only shape this canary had. Any pattern keyed on the narrow line
// passes here and misses in production, where ctest then falls back to the exit code
// and scores a legitimate skip as a FAILURE -- #499 returning through the change that
// exists to prevent it.
TEST_CASE("catch skip canary: a case that skips after ten or more assertions", "[canarypadskip]")
{
    for (auto const i: std::views::iota(0, 12))
    {
        CHECK(i >= 0);
    }

    SKIP("this canary skips after enough assertions to widen the summary column");
}

// A skip message chosen to be hostile to the pattern rather than neutral.
//
// The tail of the summary line is the only pipe-free region carrying discrimination,
// so the pattern lives there -- and text an author writes is echoed into the SKIPPED:
// block above it. A case that FAILS while echoing the pattern's own text is the shape
// that would turn a failure into a skip, which is the widening direction this ticket
// refuses. It must not match.
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

// reload-batch's adversarial shape, which is STRONGER than the decoy above and is the
// one that decides whether an unanchored pattern is usable at all (#1146).
//
// The decoy above only carries the pattern's TAIL, so it refutes a tail-only pattern
// and nothing else. This one carries the WHOLE summary line, on ONE line, inside an
// `INFO` -- and `SKIP_REGULAR_EXPRESSION` is a substring search over the case's entire
// output, which includes names, `INFO` messages and stringified expressions. None of
// that is text this project's check controls.
//
// So a pattern that merely requires `test cases:` and the tail to share a LINE is not
// anchored: an author can put both on one line. Its real summary says `1 failed`, and
// it must NOT match, or a case that only FAILS is scored SKIPPED -- #1128's own species
// arriving through the mechanism meant to close it, and in the fail-OPEN direction.
TEST_CASE("catch skip canary: a failing case echoing a whole summary line", "[canaryadversary]")
{
    std::string const report = "test cases: 1 | 1 skipped";
    INFO("reporting: " << report);
    CHECK(report.empty());
}

// ## What any future mechanism must survive
//
// The structural property the whole design rests on, stated because a property that
// holds on ANY input beats any number of measured shapes: `Totals::delta` puts a case
// in EXACTLY ONE bucket, and `failed` outranks `skipped`. So a case that both skips and
// fails can never report `1 skipped` in its summary -- which is why the summary line is
// a sound discriminator and Catch2's `SKIPPED:` marker is not. The marker survives in
// the BODY of a failing case at every width, measured at zero and at padded.

// Padding is UNBOUNDED, not one extra space. At a hundred-plus assertions the column
// widens again, so any tolerance written for a fixed width dies here rather than at
// twelve.
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

// The real-world shape, and how the live site reaches two digits: a skip in a LATER
// section, after earlier sections have already run assertions into the same totals.
//
// A case-level skip aborts the case, so its totals stay small; a SECTION-level one lets
// the other sections run, which is why the two site censuses differ -- assertions
// BEFORE the skip against assertions in the whole case.
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

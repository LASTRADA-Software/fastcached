// SPDX-License-Identifier: Apache-2.0
//
// A program that MUST end on a failed assert, promptly, with no dialog (#1389).
//
// It links `Catch2::Catch2WithMain` ON PURPOSE. That is the population the defect lived
// in: a `main` this project did not write, which therefore never called
// `SuppressWindowsErrorPopups()`, and a Debug assert in it opened "Debug Assertion Failed"
// and waited for a click. So the only thing standing between this canary and that dialog
// is the translation unit `cmake/ErrorPopups.cmake` attaches -- remove it and
// `scripts/error-popup-gate.cmake` sees the process outlive its bound instead of exiting
// with the assertion text.
//
// Registered through that gate rather than as a Catch2 test: a case that aborts is scored
// by its exit status alone, and a non-zero status is what a dialog killed by a TIMEOUT
// produces too. The gate requires the assertion's own text as well.

#include <catch2/catch_test_macros.hpp>

#include <cassert>

TEST_CASE("error-popup canary: a Debug assert ends the process instead of opening a dialog", "[canary]")
{
    // The message is what the gate looks for, so a process ending for any other reason is
    // not read as this assert firing.
    assert(false && "error-popup-canary: this assert must end the process");
}

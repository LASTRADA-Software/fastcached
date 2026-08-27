// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NarrowText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>

using namespace FastCache;

TEST_CASE("Narrow text reaches this process as UTF-8", "[platform][narrowtext]")
{
    // The one assertion this file exists for, and on Windows it is a check that the
    // BUILD did something: an executable only reports 65001 here when the
    // `activeCodePage` manifest cmake/Utf8CodePage.cmake attaches actually reached
    // the linker. Without it this reports whatever the host's legacy code page is —
    // 1252 on a Western install — and every operator-typed non-ASCII argument
    // arrives as bytes the fleet's UTF-8 rules refuse (issue #155).
    //
    // Trivially true on POSIX, where nothing is transcoded at all. That asymmetry
    // is the defect's, not the test's.
    auto const codePage = ActiveCodePage();
    INFO(std::format("active code page: {}",
                     codePage.has_value() ? std::to_string(*codePage) : std::string { "none (nothing is transcoded)" }));
    CHECK(NarrowTextIsUtf8());
}

TEST_CASE("A code page is reported exactly where one is applied", "[platform][narrowtext]")
{
    // Not cosmetic: `NarrowTextIsUtf8()` answers true for BOTH "transcoded through
    // UTF-8" and "not transcoded", so which of the two this host is has to be asked
    // separately -- a diagnostic that named a code page on a platform that has none
    // would explain a failure with a fact it invented. WHICH code page it is, is the
    // case above; that `Utf8CodePage` is the number Windows means by it is a
    // static_assert in NarrowText.cpp, where <windows.h> is in scope.
    auto const codePage = ActiveCodePage();
#if defined(_WIN32)
    CHECK(codePage.has_value());
#else
    CHECK_FALSE(codePage.has_value());
#endif
}

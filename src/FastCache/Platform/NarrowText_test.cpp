// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NarrowText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace FastCache;
using namespace std::string_view_literals;

namespace
{
/// U+00FC, spelled once per encoding this file needs it in.
constexpr auto UmlautUtf8 = "gr\xC3\xBC"
                            "n"sv; ///< UTF-8.
constexpr auto UmlautCp1252 = "gr\xFC"
                              "n"sv; ///< CP-1252, and CP-850's `n` is not this.
constexpr auto UmlautCp850 = "gr\x81"
                             "n"sv; ///< CP-850, the OEM console default here.
} // namespace

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

TEST_CASE("Text that is already UTF-8 is taken as it stands", "[platform][narrowtext]")
{
    // Asked first and answered without the platform, which is why these hold
    // everywhere: a producer that emits UTF-8 -- clang and gcc always, cl under a
    // UTF-8 console -- never reaches a code page at all.
    CHECK(Utf8FromNarrowText("plain ascii"sv, std::nullopt) == "plain ascii");
    CHECK(Utf8FromNarrowText(UmlautUtf8, std::nullopt) == std::string { UmlautUtf8 });
    CHECK(Utf8FromNarrowText(""sv, std::nullopt) == "");

    // Named as a code page and still not a second candidate: UTF-8 was already
    // tried and refused, so re-trying it would be the same answer twice.
    CHECK_FALSE(Utf8FromNarrowText(UmlautCp1252, Utf8CodePage).has_value());
}

TEST_CASE("Text that is not UTF-8 and has no second candidate is refused", "[platform][narrowtext]")
{
    // The whole point of refusing rather than repairing: a path is matched against
    // a root byte for byte, so a `U+FFFD` substituted into one names a file that
    // does not exist, and the launcher would key it as toolchain content and never
    // revalidate it. Refused where it enters instead.
    CHECK_FALSE(Utf8FromNarrowText(UmlautCp1252, std::nullopt).has_value());
    CHECK_FALSE(Utf8FromNarrowText(UmlautCp850, std::nullopt).has_value());

    // Strict in the sense RFC 3629 is, because `Core/Utf8.hpp` is: a form that only
    // LOOKS like UTF-8 must fall through to the code page rather than be passed on
    // as text (#141).
    CHECK_FALSE(Utf8FromNarrowText("\xC0\x80"sv, std::nullopt).has_value());     // overlong NUL
    CHECK_FALSE(Utf8FromNarrowText("\xED\xA0\x80"sv, std::nullopt).has_value()); // lone surrogate
    CHECK_FALSE(Utf8FromNarrowText("\x80"sv, std::nullopt).has_value());         // stray continuation
}

TEST_CASE("Text that is not UTF-8 is decoded through the code page named", "[platform][narrowtext]")
{
#if defined(_WIN32)
    // The case this whole seam exists for: `cl.exe` writes the paths in
    // `/showIncludes` in the CONSOLE OUTPUT code page, so the same U+00FC arrives
    // as one byte whose value depends on which console the build is running under.
    CHECK(Utf8FromNarrowText(UmlautCp1252, 1252U) == std::string { UmlautUtf8 });
    CHECK(Utf8FromNarrowText(UmlautCp850, 850U) == std::string { UmlautUtf8 });

    // And the reason there is exactly one code page rather than a ladder of them:
    // a legacy single-byte page decodes very nearly every byte, so the WRONG page
    // is a wrong answer and not a refusal. `MB_ERR_INVALID_CHARS` does not save
    // this -- Windows maps CP-1252's five unassigned bytes to the matching C1
    // controls rather than rejecting them, so `81` reads as U+0081 and succeeds.
    // A path decoded that way names a file that does not exist, under a spelling
    // nothing will ever match; trying pages until one "works" would produce it
    // every time.
    auto const wrongPage = Utf8FromNarrowText(UmlautCp850, 1252U);
    REQUIRE(wrongPage.has_value());
    CHECK(*wrongPage != std::string { UmlautUtf8 });
#else
    // There is no transcoder here, so a code page a caller names is a request this
    // build cannot honour -- and saying so is not the same as saying the bytes are
    // fine. `HostNarrowTextPolicy()` never names one on this platform.
    CHECK_FALSE(Utf8FromNarrowText(UmlautCp1252, 1252U).has_value());
#endif
}

TEST_CASE("The host policy separates this process from a tool it runs", "[platform][narrowtext]")
{
    auto const policy = HostNarrowTextPolicy();
#if defined(_WIN32)
    // True because of the manifest, and it is NOT `NarrowTextIsUtf8()`: that
    // answers true where nothing is transcoded, which is the very host on which
    // narrow bytes need not be UTF-8 to name a file.
    CHECK(policy.pathsAreUtf8);

    // ALWAYS an answer here, whether or not this process has a console -- and that
    // is the point rather than a detail. Answering nothing would make every
    // non-ASCII path a tool reported unreadable, and the launcher would decline to
    // cache a translation unit it cached yesterday, in silence, on exactly the
    // console-less builds (MSBuild, an IDE, a detached agent) where the operator
    // has no console to fix.
    CHECK(policy.toolCodePage.has_value());
#else
    CHECK_FALSE(policy.pathsAreUtf8);
    // Nothing to read a code page from, and inventing one would make every legacy
    // filename on this host unusable.
    CHECK_FALSE(policy.toolCodePage.has_value());
#endif
}

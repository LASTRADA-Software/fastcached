// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;

namespace
{
/// A key present on every supported Windows and readable by an unprivileged
/// process, so the positive cases assert against the real registry rather than
/// against a fixture this module would have had to write.
constexpr std::string_view CurrentVersionKey = R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)";

/// A key nothing has ever installed. Named once so the two absence cases below
/// cannot drift into testing different things.
constexpr std::string_view AbsentKey = R"(SOFTWARE\fastcached-no-such-key-9f3a)";
} // namespace

TEST_CASE("A key that does not exist reads as absent rather than empty", "[registry]")
{
    // The distinction is the whole contract: an empty string is a value somebody
    // wrote, and a caller that treated "absent" as "" would build paths out of it.
    CHECK_FALSE(ReadRegistryString(RegistryHive::LocalMachine, AbsentKey, "Anything", RegistryView::Native).has_value());
    CHECK_FALSE(
        ReadRegistryString(RegistryHive::CurrentUser, AbsentKey, "Anything", RegistryView::ThirtyTwoBit).has_value());
}

#if defined(_WIN32)

TEST_CASE("A REG_SZ value comes back verbatim", "[registry]")
{
    auto const build =
        ReadRegistryString(RegistryHive::LocalMachine, CurrentVersionKey, "CurrentBuildNumber", RegistryView::Native);
    REQUIRE(build.has_value());
    CHECK_FALSE(FastCache::Testing::Unwrap(build).empty());

    // No embedded NUL survived the read. A value stored without a terminator and
    // sized from the reported byte count keeps one, and such a string opens no
    // path and compares equal to nothing -- which is why TrimAtNul exists.
    CHECK(FastCache::Testing::Unwrap(build).find('\0') == std::string::npos);
}

TEST_CASE("A value of the wrong type is absent rather than reinterpreted", "[registry]")
{
    // Proven to be a rejection rather than an absence: a REG_SZ under the SAME key
    // reads fine, so the key opened and the value below was seen and turned down
    // for its type. `InstallDate` is REG_DWORD on every Windows since Vista.
    REQUIRE(ReadRegistryString(RegistryHive::LocalMachine, CurrentVersionKey, "CurrentBuildNumber", RegistryView::Native)
                .has_value());

    CHECK_FALSE(
        ReadRegistryString(RegistryHive::LocalMachine, CurrentVersionKey, "InstallDate", RegistryView::Native).has_value());
}

TEST_CASE("The 32-bit view resolves to WOW6432Node", "[registry]")
{
    // `ProgramFilesDir` is the value the redirection exists FOR: the native copy
    // says `C:\Program Files` and the 32-bit one `C:\Program Files (x86)`. That is
    // what makes this case catch a dropped `KEY_WOW64_32KEY` -- with the flag
    // missing, the left-hand read falls through to the native copy and the two
    // stop matching.
    //
    // The IDENTITY is what is asserted, not the inequality. That the two copies
    // differ is true on every machine anybody will run this on and guaranteed on
    // none of them; that the 32-bit VIEW of a key and the NATIVE read of its
    // `WOW6432Node` twin name the same storage is the API's own contract.
    constexpr std::string_view windowsKey = R"(SOFTWARE\Microsoft\Windows\CurrentVersion)";
    constexpr std::string_view redirectedKey = R"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion)";

    auto const throughView =
        ReadRegistryString(RegistryHive::LocalMachine, windowsKey, "ProgramFilesDir", RegistryView::ThirtyTwoBit);
    auto const throughPath =
        ReadRegistryString(RegistryHive::LocalMachine, redirectedKey, "ProgramFilesDir", RegistryView::Native);

    // Required present so the comparison cannot pass by both sides being absent,
    // which is what an assertion nobody notices looks like.
    REQUIRE(throughView.has_value());
    REQUIRE(throughPath.has_value());
    CHECK(FastCache::Testing::Unwrap(throughView) == FastCache::Testing::Unwrap(throughPath));
}

#else

TEST_CASE("A host with no registry answers nothing, on every hive and view", "[registry]")
{
    // Compiled everywhere so a caller needs no `#if` of its own; the point of
    // this case is that the POSIX build really does link and really does answer.
    CHECK_FALSE(ReadRegistryString(RegistryHive::LocalMachine, CurrentVersionKey, "CurrentBuildNumber", RegistryView::Native)
                    .has_value());
    CHECK_FALSE(
        ReadRegistryString(RegistryHive::CurrentUser, CurrentVersionKey, "CurrentBuildNumber", RegistryView::ThirtyTwoBit)
            .has_value());
}

#endif

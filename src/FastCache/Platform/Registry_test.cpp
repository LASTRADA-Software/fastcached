// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace FastCache;

namespace
{
/// A key present on every supported Windows and readable by an unprivileged
/// process, so the positive cases assert against the real registry rather than
/// against a fixture this module would have had to write.
constexpr std::string_view CurrentVersionKey = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

/// A key nothing has ever installed. Named once so the two absence cases below
/// cannot drift into testing different things.
constexpr std::string_view AbsentKey = "SOFTWARE\\fastcached-no-such-key-9f3a";
} // namespace

TEST_CASE("A key that does not exist reads as absent rather than empty", "[registry]")
{
    // The distinction is the whole contract: an empty string is a value somebody
    // wrote, and a caller that treated "absent" as "" would build paths out of it.
    CHECK_FALSE(ReadRegistryString(RegistryHive::LocalMachine, AbsentKey, "Anything", RegistryView::Native).has_value());
    CHECK_FALSE(
        ReadRegistryString(RegistryHive::CurrentUser, AbsentKey, "Anything", RegistryView::ThirtyTwoBit).has_value());
    CHECK(ListRegistryValueNames(RegistryHive::LocalMachine, AbsentKey, RegistryView::Native).empty());
}

#if defined(_WIN32)

TEST_CASE("A REG_SZ value comes back verbatim", "[registry]")
{
    auto const build =
        ReadRegistryString(RegistryHive::LocalMachine, CurrentVersionKey, "CurrentBuildNumber", RegistryView::Native);
    REQUIRE(build.has_value());
    CHECK_FALSE(build->empty());

    // No embedded NUL survived the read. A value stored without a terminator and
    // sized from the reported byte count keeps one, and such a string opens no
    // path and compares equal to nothing -- which is why TrimAtNul exists.
    CHECK(build->find('\0') == std::string::npos);
}

TEST_CASE("A value of the wrong type is absent rather than reinterpreted", "[registry]")
{
    // Proven to be a rejection rather than an absence: the same key's value
    // listing names it, so the read below saw it and turned it down for its type.
    // `InstallDate` is REG_DWORD on every Windows since Vista.
    auto const names = ListRegistryValueNames(RegistryHive::LocalMachine, CurrentVersionKey, RegistryView::Native);
    REQUIRE(std::ranges::contains(names, std::string { "InstallDate" }));

    CHECK_FALSE(
        ReadRegistryString(RegistryHive::LocalMachine, CurrentVersionKey, "InstallDate", RegistryView::Native).has_value());
}

TEST_CASE("Value names are enumerated to the end of the key", "[registry]")
{
    auto const names = ListRegistryValueNames(RegistryHive::LocalMachine, CurrentVersionKey, RegistryView::Native);

    // More than a handful, and containing an entry whose name is longer than the
    // first one enumerated -- which is the case that used to stop the loop early,
    // because RegEnumValue writes the used length back over the buffer size.
    CHECK(names.size() > 4);
    CHECK(std::ranges::contains(names, std::string { "CurrentBuildNumber" }));
    CHECK(std::ranges::none_of(names, [](std::string const& name) { return name.find('\0') != std::string::npos; }));
}

TEST_CASE("The 32-bit view resolves to WOW6432Node", "[registry]")
{
    // Asserted as the redirection IDENTITY -- `SOFTWARE\X` read in the 32-bit view
    // is `SOFTWARE\WOW6432Node\X` read natively -- rather than by comparing the two
    // views of one path against each other. `Windows NT\CurrentVersion` is a
    // *redirected* key, so those are two independent keys: the WOW6432Node copy is
    // written at install and not kept in step by servicing, and an upgraded machine
    // legitimately has a stale `CurrentBuildNumber` there. A test asserting they
    // match would fail on such a host while this function was behaving correctly.
    //
    // Both sides here name the same underlying key, so the case can never be flaky
    // -- and it still catches a dropped `KEY_WOW64_32KEY`, because the redirected
    // copy holds a genuinely different set of values (33 against 25 on the machine
    // this was written on), so the two listings diverge as soon as the flag stops
    // reaching the open. The inequality is deliberately NOT asserted: it is true on
    // every machine anybody will run this on and guaranteed on none of them, which
    // is the property that made the previous version of this case wrong.
    auto const throughPath = ListRegistryValueNames(
        RegistryHive::LocalMachine, "SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion", RegistryView::Native);
    auto const redirected =
        ListRegistryValueNames(RegistryHive::LocalMachine, CurrentVersionKey, RegistryView::ThirtyTwoBit);

    // Required non-empty so the comparison cannot pass by both sides being empty,
    // which is what an assertion nobody notices looks like.
    REQUIRE_FALSE(throughPath.empty());
    CHECK(redirected == throughPath);
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
    CHECK(ListRegistryValueNames(RegistryHive::LocalMachine, CurrentVersionKey, RegistryView::Native).empty());
}

#endif

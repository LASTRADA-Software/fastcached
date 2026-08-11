// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using FastCache::ReadEnvironmentVariable;
using FastCache::Testing::ScopedEnv;

// The one place the library reads the environment, and the reason it is one
// place: three call sites each spelled their own #if / getenv_s / std::getenv
// dance, which is three chances to get the Windows length convention wrong.
// What the callers rely on is the *shape* of the answer, so that is what these
// pin down.

TEST_CASE("ReadEnvironmentVariable: a variable that is set comes back verbatim", "[platform][environment]")
{
    ScopedEnv const env { "FC_TEST_ENVIRONMENT", "/opt/fastcached/etc" };

    auto const value = ReadEnvironmentVariable("FC_TEST_ENVIRONMENT");
    REQUIRE(value.has_value());
    REQUIRE(*value == "/opt/fastcached/etc");
}

TEST_CASE("ReadEnvironmentVariable: an unset variable is nullopt, not an empty string", "[platform][environment]")
{
    // The distinction the config lookup is built on: an unset base variable
    // means "this location does not apply to this process" and skips its
    // candidate row, whereas an empty one would make the suffix relative to the
    // working directory — C:\Windows\System32 for a service.
    REQUIRE_FALSE(ReadEnvironmentVariable("FC_TEST_DEFINITELY_NOT_SET").has_value());
}

TEST_CASE("ReadEnvironmentVariable: a name is not required to be NUL-terminated", "[platform][environment]")
{
    ScopedEnv const env { "FC_TEST_ENVIRONMENT", "value" };

    // Callers pass string_views cut out of larger buffers — the candidate
    // table's baseVar fields are views into string literals — and both platform
    // APIs take a NUL-terminated name, so the copy has to happen inside.
    std::string const haystack { "FC_TEST_ENVIRONMENTX" };
    auto const value = ReadEnvironmentVariable(std::string_view { haystack }.substr(0, haystack.size() - 1));
    REQUIRE(value.has_value());
    REQUIRE(*value == "value");
}

#if !defined(_WIN32)
TEST_CASE("ReadEnvironmentVariable: a variable set to nothing is an empty string", "[platform][environment]")
{
    // POSIX only, and not an oversight: on Windows there is no way to produce a
    // present-but-empty variable — _putenv_s with "" deletes it — so the state
    // this asserts is unreachable there. getenv_s' convention still has to be
    // read correctly for it (a length of 1, not 0), which is what makes the
    // case worth pinning where it can be.
    ScopedEnv const env { "FC_TEST_ENVIRONMENT", "" };

    auto const value = ReadEnvironmentVariable("FC_TEST_ENVIRONMENT");
    REQUIRE(value.has_value());
    REQUIRE(value->empty());
}
#endif

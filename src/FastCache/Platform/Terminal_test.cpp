// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Terminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

namespace
{
/// Set the NO_COLOR environment variable for the duration of a test,
/// restoring (clearing) it on destruction. Env mutation is process-local
/// and each Catch test case runs in its own ctest process, so this stays
/// isolated.
struct ScopedNoColor
{
    ScopedNoColor()
    {
#if defined(_WIN32)
        ::_putenv_s("NO_COLOR", "1");
#else
        ::setenv("NO_COLOR", "1", /*overwrite=*/1);
#endif
    }

    ~ScopedNoColor()
    {
#if defined(_WIN32)
        ::_putenv_s("NO_COLOR", "");
#else
        ::unsetenv("NO_COLOR");
#endif
    }

    ScopedNoColor(ScopedNoColor const&) = delete;
    ScopedNoColor& operator=(ScopedNoColor const&) = delete;
    ScopedNoColor(ScopedNoColor&&) = delete;
    ScopedNoColor& operator=(ScopedNoColor&&) = delete;
};
} // namespace

TEST_CASE("Terminal: StdoutSupportsColor honors NO_COLOR", "[platform][terminal][color]")
{
    ScopedNoColor const guard;
    // With NO_COLOR set, color must be suppressed regardless of TTY state.
    REQUIRE_FALSE(FastCache::StdoutSupportsColor());
}

TEST_CASE("Terminal: StdoutSupportsColor is false for a non-terminal stdout", "[platform][terminal][color]")
{
    // The test runner's stdout is a pipe/file under ctest, never an interactive
    // terminal, so color detection must report false (and must not crash).
    REQUIRE_FALSE(FastCache::StdoutSupportsColor());
}

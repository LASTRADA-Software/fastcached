// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Terminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include <tests/ScopedPipeStdin.hpp>

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

TEST_CASE("Terminal: standard streams are not interactive when stdin is a pipe", "[platform][terminal]")
{
    FastCache::Testing::ScopedPipeStdin const guard;
    REQUIRE(guard.installed);
    REQUIRE_FALSE(FastCache::StandardStreamsAreInteractive());
}

TEST_CASE("Terminal: saved terminal modes are not captured without a terminal", "[platform][terminal]")
{
    // The capturing direction needs a real terminal, which no test runner has; this is the half
    // that can be asserted anywhere. It matters on its own: a capture that "succeeded" on a pipe
    // would later apply a zeroed termios to whatever stdin is.
    FastCache::Testing::ScopedPipeStdin const guard;
    REQUIRE(guard.installed);
    CHECK_FALSE(FastCache::SavedTerminalModes::Capture().has_value());
}

TEST_CASE("Terminal: the locale variables decide the encoding by libc's precedence", "[platform][terminal]")
{
    using FastCache::EncodingFromLocaleVariables;
    using FastCache::TerminalTextEncoding;
    using std::nullopt;

    // The first non-empty variable decides and the rest are not consulted. The first row is the
    // one a scan for any mention of UTF-8 gets wrong.
    CHECK(EncodingFromLocaleVariables("C", nullopt, "en_US.UTF-8") == TerminalTextEncoding::Other);
    CHECK(EncodingFromLocaleVariables("", "en_US.UTF-8", "C") == TerminalTextEncoding::Utf8);
    CHECK(EncodingFromLocaleVariables(nullopt, "POSIX", "en_US.UTF-8") == TerminalTextEncoding::Other);
    CHECK(EncodingFromLocaleVariables(nullopt, nullopt, "de_DE.utf8") == TerminalTextEncoding::Utf8);
    CHECK(EncodingFromLocaleVariables(nullopt, nullopt, "C.UTF-8") == TerminalTextEncoding::Utf8);
    CHECK(EncodingFromLocaleVariables(nullopt, nullopt, "sr_RS.UTF-8@latin") == TerminalTextEncoding::Utf8);
    CHECK(EncodingFromLocaleVariables(nullopt, nullopt, "en_US.ISO-8859-1") == TerminalTextEncoding::Other);
    CHECK(EncodingFromLocaleVariables(nullopt, nullopt, "en_US") == TerminalTextEncoding::Other);
}

TEST_CASE("Terminal: no locale to read is Unknown, never a finding", "[platform][terminal]")
{
    using FastCache::EncodingFromLocaleVariables;
    using FastCache::TerminalTextEncoding;

    CHECK(EncodingFromLocaleVariables(std::nullopt, std::nullopt, std::nullopt) == TerminalTextEncoding::Unknown);
    CHECK(EncodingFromLocaleVariables("", "", "") == TerminalTextEncoding::Unknown);
}

TEST_CASE("Terminal: the console output code page decides the encoding on Windows", "[platform][terminal]")
{
    using FastCache::EncodingFromConsoleOutputCodePage;
    using FastCache::TerminalTextEncoding;

    CHECK(EncodingFromConsoleOutputCodePage(65001U) == TerminalTextEncoding::Utf8);
    CHECK(EncodingFromConsoleOutputCodePage(437U) == TerminalTextEncoding::Other);
    CHECK(EncodingFromConsoleOutputCodePage(std::nullopt) == TerminalTextEncoding::Unknown);
}

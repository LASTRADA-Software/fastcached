// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Terminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

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

namespace
{
/// Replace standard input with the read end of a pipe for the duration of a test.
///
/// A pipe is never a terminal on any platform, so with it in place the answer is fixed
/// whatever the runner happened to connect stdin to. Asserting on the runner's own stdin
/// would pass or fail depending on whether ctest was started from a terminal.
struct ScopedPipeStdin
{
    ScopedPipeStdin()
    {
#if defined(_WIN32)
        saved = ::GetStdHandle(STD_INPUT_HANDLE);
        installed = ::CreatePipe(&readEnd, &writeEnd, nullptr, 0) != 0 && ::SetStdHandle(STD_INPUT_HANDLE, readEnd) != 0;
#else
        saved = ::dup(STDIN_FILENO);
        installed = saved >= 0 && ::pipe(ends.data()) == 0 && ::dup2(ends[0], STDIN_FILENO) >= 0;
#endif
    }

    ~ScopedPipeStdin()
    {
#if defined(_WIN32)
        ::SetStdHandle(STD_INPUT_HANDLE, saved);
        ::CloseHandle(readEnd);
        ::CloseHandle(writeEnd);
#else
        ::dup2(saved, STDIN_FILENO);
        ::close(saved);
        ::close(ends[0]);
        ::close(ends[1]);
#endif
    }

    ScopedPipeStdin(ScopedPipeStdin const&) = delete;
    ScopedPipeStdin& operator=(ScopedPipeStdin const&) = delete;
    ScopedPipeStdin(ScopedPipeStdin&&) = delete;
    ScopedPipeStdin& operator=(ScopedPipeStdin&&) = delete;

    /// Whether the pipe really replaced stdin. Asserted by the case, or a failed setup would
    /// leave the runner's own stdin in place and the answer would depend on how ctest started.
    bool installed { false };
#if defined(_WIN32)
    HANDLE saved { nullptr };
    HANDLE readEnd { nullptr };
    HANDLE writeEnd { nullptr };
#else
    int saved { -1 };
    std::array<int, 2> ends { -1, -1 };
#endif
};
} // namespace

TEST_CASE("Terminal: standard streams are not interactive when stdin is a pipe", "[platform][terminal]")
{
    ScopedPipeStdin const guard;
    REQUIRE(guard.installed);
    REQUIRE_FALSE(FastCache::StandardStreamsAreInteractive());
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

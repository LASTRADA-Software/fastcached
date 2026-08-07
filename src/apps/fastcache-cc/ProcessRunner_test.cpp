// SPDX-License-Identifier: Apache-2.0
//
// Tests for the real process runner. These spawn actual child processes, so
// they use a shell one-liner that exists on every host the launcher targets.

#include "IProcessRunner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache::Cc;

namespace
{

/// Build an argv that runs `script` through the platform shell.
///
/// The script text itself must already be in that shell's dialect — see the
/// helpers below. Selecting the interpreter here while writing POSIX-only
/// script text at the call sites silently mis-ran these tests under cmd.exe,
/// which does not treat `;` as a command separator and so folded both echoes
/// into a single stdout line.
///
/// @param script The shell command to run, in the host shell's dialect.
/// @return The full invocation.
[[nodiscard]] std::vector<std::string> ShellCommand(std::string const& script)
{
#if defined(_WIN32)
    return { "cmd.exe", "/c", script };
#else
    return { "/bin/sh", "-c", script };
#endif
}

/// A script that writes `outText` to stdout and `errText` to stderr.
/// @param outText Text for stdout.
/// @param errText Text for stderr.
/// @return The invocation, in the host shell's dialect.
[[nodiscard]] std::vector<std::string> EchoBothStreams(std::string const& outText, std::string const& errText)
{
#if defined(_WIN32)
    // cmd.exe separates commands with '&', and `echo x 1>&2` needs no quoting.
    return ShellCommand("echo " + outText + "& echo " + errText + " 1>&2");
#else
    return ShellCommand("echo " + outText + "; echo " + errText + " 1>&2");
#endif
}

/// A script that writes `lines` lines to BOTH streams, enough to overflow the
/// ~64 KiB pipe buffer on either one.
/// @param lines Number of lines to emit per stream.
/// @return The invocation, in the host shell's dialect.
[[nodiscard]] std::vector<std::string> FloodBothStreams(int lines)
{
    auto const n = std::to_string(lines);
#if defined(_WIN32)
    // `for /L %i in (1,1,N)` is cmd.exe's counted loop; '%' needs no escaping
    // because this runs via /c rather than from a batch file.
    return ShellCommand("for /L %i in (1,1," + n + ") do @(echo " + std::string(80, 'o') + "& echo " + std::string(80, 'e')
                        + " 1>&2)");
#else
    return ShellCommand("i=0; while [ $i -lt " + n + " ]; do echo " + std::string(80, 'o') + "; echo " + std::string(80, 'e')
                        + " 1>&2; i=$((i+1)); done");
#endif
}

} // namespace

TEST_CASE("RunCaptureSplit reports the child's exit code")
{
    auto const runner = MakeProcessRunner();

    auto const ok = runner->RunCaptureSplit(ShellCommand("exit 0"));
    CHECK(ok.exitCode == 0);

    auto const failed = runner->RunCaptureSplit(ShellCommand("exit 3"));
    CHECK(failed.exitCode == 3);
}

TEST_CASE("RunCaptureSplit keeps stdout and stderr apart")
{
    auto const runner = MakeProcessRunner();
    auto const run = runner->RunCaptureSplit(EchoBothStreams("to-out", "to-err"));

    CHECK(run.exitCode == 0);
    // The whole point of the split capture: a cache hit replays each stream on
    // its own channel, so they must not be merged here.
    CHECK(run.out.contains("to-out"));
    CHECK_FALSE(run.out.contains("to-err"));
    CHECK(run.err.contains("to-err"));
    CHECK_FALSE(run.err.contains("to-out"));
}

TEST_CASE("RunCaptureCombined merges both streams into out")
{
    auto const runner = MakeProcessRunner();
    auto const run = runner->RunCaptureCombined(EchoBothStreams("to-out", "to-err"));

    CHECK(run.exitCode == 0);
    CHECK(run.out.contains("to-out"));
    CHECK(run.out.contains("to-err"));
    CHECK(run.err.empty());
}

TEST_CASE("RunCaptureSplit does not deadlock when both streams exceed the pipe buffer")
{
    // A pipe buffer is ~64 KiB. Draining one stream to EOF before starting the
    // other deadlocks as soon as the undrained one fills — and preprocessed
    // compiler output routinely exceeds that. A truncated capture would make
    // the cache key nondeterministic and silently defeat all caching, so this
    // is a correctness test, not a performance one.
    constexpr int LineCount = 2000; // 81 bytes/line => ~160 KiB per stream

    auto const runner = MakeProcessRunner();
    auto const run = runner->RunCaptureSplit(FloodBothStreams(LineCount));

    CHECK(run.exitCode == 0);
    // Assert we captured the bulk of BOTH streams rather than a truncated
    // prefix of one of them.
    CHECK(run.out.size() > 64 * 1024);
    CHECK(run.err.size() > 64 * 1024);
}

TEST_CASE("RunCaptureSplit reports a spawn failure as exit code -1")
{
    auto const runner = MakeProcessRunner();
    auto const run = runner->RunCaptureSplit({ { "definitely-not-a-real-program-xyzzy" } });

    // -1 is the launcher's "could not spawn" signal, distinct from a compiler
    // that ran and failed; the caller turns it into a plain fallback.
    CHECK(run.exitCode == -1);
}

TEST_CASE("RunCaptureSplit on an empty argv fails instead of spawning anything")
{
    auto const runner = MakeProcessRunner();
    CHECK(runner->RunCaptureSplit({}).exitCode == -1);
}

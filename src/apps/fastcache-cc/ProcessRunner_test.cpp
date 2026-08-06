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
/// @param script The shell command to run.
/// @return The full invocation.
[[nodiscard]] std::vector<std::string> ShellCommand(std::string const& script)
{
#if defined(_WIN32)
    return { "cmd.exe", "/c", script };
#else
    return { "/bin/sh", "-c", script };
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
    auto const run = runner->RunCaptureSplit(ShellCommand("echo to-out; echo to-err 1>&2"));

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
    auto const run = runner->RunCaptureCombined(ShellCommand("echo to-out; echo to-err 1>&2"));

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
    constexpr int LineCount = 4000; // 41 bytes/line => ~160 KiB per stream
    auto const script = std::string { "i=0; while [ $i -lt " } + std::to_string(LineCount)
                        + " ]; do echo 0123456789012345678901234567890123456789; "
                          "echo 0123456789012345678901234567890123456789 1>&2; "
                          "i=$((i+1)); done";

    auto const runner = MakeProcessRunner();
    auto const run = runner->RunCaptureSplit(ShellCommand(script));

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

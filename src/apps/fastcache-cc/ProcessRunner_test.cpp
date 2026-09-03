// SPDX-License-Identifier: Apache-2.0
//
// Tests for the real process runner. These spawn actual child processes, so
// they use a shell one-liner that exists on every host the launcher targets.

#include "IProcessRunner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
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

namespace
{
/// What a child prints when it still has an inherited `PATH`.
constexpr std::string_view InheritedPathMarker = "HASPATH";

/// A script that echoes one named variable, then reports whether `PATH` SURVIVED.
///
/// `PATH` is the inherited thing every host has, so no test has to arrange one. But
/// it is reported by a fixed marker rather than echoed, and that is not tidiness:
/// `cmd.exe` expands variables before it applies its ~8191-character command-line
/// limit, so a script echoing a real `PATH` twice exits 1 on any machine whose
/// `PATH` is long. Measured here: at 1309 characters every case passed, at 5449 the
/// two-expansion case failed, at 8554 the one-expansion case failed too, and
/// `cmd /c "echo %PATH%& echo %PATH%"` on its own exits 1 at 5449 where a single
/// echo exits 0. A CI runner's `PATH` is in exactly that range, which is why this
/// passed locally and failed on both Windows legs.
///
/// The marker is also a STRONGER predicate than echoing the value was, not a weaker
/// one substituted to stop a failure. `cmd.exe` echoes an unset variable as the
/// literal `%PATH%`, so both a length check and a "contains a separator" check were
/// reading text that a replaced environment still produces; `if defined` is false
/// exactly when the variable is gone, which is the property under test. Its output
/// is bounded, so no machine's configuration can make it fail for an unrelated
/// reason.
///
/// The two shells spell all of this differently and neither spelling works under the
/// other, which is the trap `ShellCommand` records above.
/// @param named The variable to echo first.
/// @return The invocation, in the host shell's dialect.
[[nodiscard]] std::vector<std::string> EchoEnvironment(std::string const& named)
{
    auto const marker = std::string { InheritedPathMarker };
#if defined(_WIN32)
    return ShellCommand("echo %" + named + "%& if defined PATH (echo " + marker + ")");
#else
    // Raw literals: the expansion must be double-quoted so a value containing a
    // space stays one word, and spelling that with escapes is what
    // `modernize-raw-string-literal` refuses.
    return ShellCommand(R"(echo "$)" + named + R"("; if [ -n "$PATH" ]; then echo )" + marker + "; fi");
#endif
}
} // namespace

TEST_CASE("A spawn's added environment is layered on the inherited one, not substituted for it")
{
    // The property the overload exists for, and the one that is expensive to get
    // wrong in the quiet direction. A spawn that REPLACED the environment would pass
    // any test that only looked for the added variable -- and on Windows it would
    // lose `INCLUDE`, so every compile fails with `C1034` and a compiler that cannot
    // find `cstddef` reports a confident syntax error rather than a missing
    // environment. Both halves are therefore asserted: the addition ARRIVED, and the
    // inherited environment SURVIVED.
    auto const runner = MakeProcessRunner();
    std::array<EnvironmentAssignment, 1> const added { {
        { .name = "FASTCACHE_ENV_PROBE", .value = "layered" },
    } };

    auto const run = runner->RunCaptureSplit(EchoEnvironment("FASTCACHE_ENV_PROBE"), added);

    REQUIRE(run.exitCode == 0);
    CHECK(run.out.contains("layered"));

    // The inherited half. This assertion has now been wrong twice in OPPOSITE
    // directions, which is why `EchoEnvironment` carries the whole argument. It
    // began as "the second line is LONG", which a replaced environment passes,
    // because `cmd.exe` echoes an unset variable as the literal `%PATH%`. It became
    // "the second line contains a directory separator", which was strong enough and
    // required echoing a real `PATH` -- which exits 1 on a machine whose `PATH` is
    // long, so it failed on CI for a reason unrelated to the property. The marker is
    // bounded AND strictly stronger than either.
    CHECK(run.out.contains(InheritedPathMarker));
}

TEST_CASE("A spawn's added environment overrides an inherited variable of the same name")
{
    // Additive does not mean appended. A duplicate name is resolved by the
    // implementation on POSIX, so leaving both entries in place would make the
    // override silently ineffective on some libc; on Windows a block carrying one
    // name twice is simply malformed. `PATH` is used because it is guaranteed to be
    // inherited, which makes this a real collision rather than a constructed one.
    auto const runner = MakeProcessRunner();
    std::array<EnvironmentAssignment, 1> const overriding { {
        { .name = "PATH", .value = "fastcache-overrode-this" },
    } };

    auto const run = runner->RunCaptureSplit(EchoEnvironment("PATH"), overriding);

    REQUIRE(run.exitCode == 0);
    CHECK(run.out.contains("fastcache-overrode-this"));
}

TEST_CASE("A spawn with no additions inherits what it would have inherited anyway")
{
    // The no-additions path is the one every ordinary compile takes, and it must not
    // become a reconstruction of the environment that could differ from the real
    // one. Asserted through the OVERLOAD rather than the original, because that is
    // where a rebuilt block would be handed over.
    //
    // `FASTCACHE_ENV_PROBE` rather than `PATH` as the echoed name: this case is
    // about what was INHERITED, the marker answers that on its own, and asking for
    // "PATH" made the script expand a real `PATH` twice in one `cmd.exe` line. That
    // is what failed on both Windows legs while both sibling cases passed -- the
    // FIXTURE, not the runner. See `EchoEnvironment` for the measurements.
    auto const runner = MakeProcessRunner();
    auto const run = runner->RunCaptureSplit(EchoEnvironment("FASTCACHE_ENV_PROBE"), {});

    REQUIRE(run.exitCode == 0);

    // The same marker the layering case uses, and for the same reason: a length
    // check passes here under a runner that dropped the inherited block. A
    // `CHECK_FALSE` for the overriding value would be worse than weak -- nothing in
    // this spawn ever sets it, so it is vacuous.
    CHECK(run.out.contains(InheritedPathMarker));
}

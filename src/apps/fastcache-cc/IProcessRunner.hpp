// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <span>
#include <string>

namespace FastCache::Cc
{

/// The exit code a `CompileRun` carries when the process could not be spawned.
///
/// Named because it is a value callers BRANCH on rather than report -- a toolchain
/// probe that never ran must not be mistaken for one that ran and found nothing
/// (issue #225), and a compiler that cannot be executed must not be registered as a
/// worker's toolchain. Both of those tests used to spell `-1` by hand, in files that
/// share no header but this one.
///
/// No real process can produce it: a POSIX exit status is 8 bits and a Windows one is
/// unsigned, so the negative range is free for this to occupy.
inline constexpr int NotSpawned = -1;

/// The result of running a child process to completion with its output
/// captured. `exitCode` is `NotSpawned` when the process could not be started at
/// all, which callers must distinguish from a compiler that ran and failed.
struct CompileRun
{
    int exitCode { NotSpawned };
    std::string out; ///< Captured stdout (empty for combined captures).
    std::string err; ///< Captured stderr (empty for combined captures).
};

/// One environment variable a spawn ADDS to the environment it inherits.
///
/// A named pair rather than a `"NAME=VALUE"` string, because the joining differs by
/// platform -- Windows wants one `\0`-separated block, POSIX an array -- and a
/// caller that pre-joined would be writing one of those two by hand.
struct EnvironmentAssignment
{
    std::string name;  ///< The variable to set.
    std::string value; ///< What to set it to.
};

/// Spawns child processes and captures their output.
///
/// This is the launcher's process seam. Spawning a compiler is ambient I/O
/// with a wildly different implementation per platform (CreateProcess + pipes
/// on Windows, fork/exec + pipes on POSIX), so it is reached through this
/// interface: the launcher's caching logic stays platform-free, and tests
/// drive it with a scripted fake instead of a real toolchain.
///
/// **Implementations must tolerate concurrent calls.** Argv in, `CompileRun` out,
/// with nothing kept between calls -- which is what both platform implementations
/// already are, holding no members at all. Requiring it here is what lets a caller
/// with several things to identify at once (the compile node, fingerprinting every
/// toolchain on a machine at startup) share the ONE runner it was handed, instead of
/// quietly manufacturing its own and stepping around the seam its tests depend on.
class IProcessRunner
{
  public:
    IProcessRunner() = default;
    virtual ~IProcessRunner() = default;
    IProcessRunner(IProcessRunner const&) = delete;
    IProcessRunner& operator=(IProcessRunner const&) = delete;
    IProcessRunner(IProcessRunner&&) = delete;
    IProcessRunner& operator=(IProcessRunner&&) = delete;

    /// Run `argv` with stdout and stderr merged into one stream. Used where the
    /// separation does not matter — preprocessing and the compiler-id probe.
    /// @param argv Full invocation; argv[0] is the executable.
    /// @return Exit code and the combined output in `out` (`err` stays empty).
    [[nodiscard]] virtual CompileRun RunCaptureCombined(std::span<std::string const> argv) = 0;

    /// The same, with variables ADDED to the environment the child inherits.
    ///
    /// The combined-capture twin of the split overload below, and everything that
    /// one's note says about additivity, about never using this on a spawn whose
    /// output a developer reads, and about honouring it being best effort applies
    /// here unchanged. Read it there; it is not repeated so the two cannot drift.
    ///
    /// This one exists for the compiler's BANNER. `cl` localizes it, and #195 made
    /// that banner the compiler's identity -- so one MSVC toolset under two Visual
    /// Studio UI languages became two identities that share no cache entry and never
    /// match each other in the fleet (issue #200). Asking for the banner in English
    /// collapses them without parsing the banner, which is the part that could not be
    /// done safely: no rule over "the version-looking token and the last one"
    /// survives a locale nobody here has read.
    ///
    /// @param argv        Full invocation; argv[0] is the executable.
    /// @param environment Variables to add to the inherited environment.
    /// @return Exit code and the combined output in `out` (`err` stays empty).
    [[nodiscard]] virtual CompileRun RunCaptureCombined(std::span<std::string const> argv,
                                                        std::span<EnvironmentAssignment const> environment)
    {
        // Forwards for the same reason the split overload's default does: a runner
        // with no process to spawn has no environment to set, and no caller may
        // depend on the variable having taken effect in either case.
        (void) environment;
        return RunCaptureCombined(argv);
    }

    /// Run `argv` capturing stdout and stderr SEPARATELY, so a later cache hit
    /// can replay each on its own channel.
    ///
    /// Implementations must drain both pipes concurrently: draining one fully
    /// before the other deadlocks as soon as the undrained stream fills its
    /// pipe buffer (~64 KiB), which preprocessed compiler output routinely
    /// exceeds. A truncated capture would make the cache key nondeterministic
    /// and silently defeat all caching.
    ///
    /// @param argv Full invocation; argv[0] is the executable.
    /// @return Exit code and the two captured streams.
    [[nodiscard]] virtual CompileRun RunCaptureSplit(std::span<std::string const> argv) = 0;

    /// The same, with variables ADDED to the environment the child inherits.
    ///
    /// Additive, never a replacement, and that is the load-bearing word. A spawn
    /// that replaced the environment would lose `INCLUDE`, which on Windows is the
    /// difference between a compile and `C1034` -- and a compiler that cannot find
    /// `cstddef` reports a confident syntax error rather than a missing environment.
    /// An assignment naming a variable the parent already has overrides that one;
    /// everything else is inherited untouched.
    ///
    /// It exists for `VSLANG`, which makes `cl` speak English on a spawn whose output
    /// only the launcher reads (issue #692). It must never be used on a spawn whose
    /// output reaches the developer: forcing English there would trade a silent
    /// performance loss for silently anglicizing every diagnostic they read.
    ///
    /// **Honouring this is best effort by nature, not by omission.** `cl` ignores
    /// `VSLANG` when the requested language pack is not installed, so no caller may
    /// conclude from a successful spawn that the child obeyed. Every caller must be
    /// able to tell from the OUTPUT that it did not -- which is also what makes the
    /// default implementation below honest rather than a silent no-op.
    ///
    /// An overload rather than a parameter on the one above, deliberately: scripted
    /// fakes and the compile node's own caller share this interface and none of them
    /// has an environment to add to. Defaulted for the same reason. A runner that
    /// really spawns MUST override it.
    ///
    /// @param argv        Full invocation; argv[0] is the executable.
    /// @param environment Variables to add to the inherited environment.
    /// @return Exit code and the two captured streams.
    [[nodiscard]] virtual CompileRun RunCaptureSplit(std::span<std::string const> argv,
                                                     std::span<EnvironmentAssignment const> environment)
    {
        // Forwarding rather than ignoring: a runner with no process to spawn has no
        // environment to set. A caller cannot depend on the variable having taken
        // effect in either case, which is the contract stated above.
        (void) environment;
        return RunCaptureSplit(argv);
    }
};

/// Create the process runner for the host platform.
/// @return A runner spawning real child processes.
[[nodiscard]] std::unique_ptr<IProcessRunner> MakeProcessRunner();

} // namespace FastCache::Cc

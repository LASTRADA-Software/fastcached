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
};

/// Create the process runner for the host platform.
/// @return A runner spawning real child processes.
[[nodiscard]] std::unique_ptr<IProcessRunner> MakeProcessRunner();

} // namespace FastCache::Cc

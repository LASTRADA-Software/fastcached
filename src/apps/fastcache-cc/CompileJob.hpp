// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "IProcessRunner.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// One translation unit a worker has been asked to compile.
struct CompileJob
{
    std::string fingerprint;       ///< The toolchain the client compiled against.
    std::vector<std::string> args; ///< Already filtered by the client's `RemoteCompileArgs`.
    std::string preprocessed;      ///< The translation unit, preprocessed.
    /// The base name the client asked its scratch file to be given. Sanitized
    /// before it becomes a path -- see `SafeSourceName` -- and never trusted: this
    /// is a string that arrived over a socket.
    std::string sourceName;
};

/// Why a job was refused before any compiler ran.
enum class JobRefusal : std::uint8_t
{
    /// No compiler on this worker matches the fingerprint the client named. The
    /// scheduler should not have sent it here, but the worker checks anyway —
    /// a client that reached the port directly did not go through scheduling at all.
    UnknownFingerprint,
    /// An argument this worker will not pass to a compiler.
    RejectedArgument,
    /// The scratch directory could not be prepared, or the source could not be
    /// written into it.
    ScratchUnavailable,
    /// The compiler could not be spawned at all. Distinct from a compiler that ran
    /// and rejected the code: only the latter is the client's answer.
    SpawnFailed,
};

/// What a completed job produced.
struct CompileOutcome
{
    int exitCode { 0 };            ///< The compiler's own exit code.
    std::vector<std::byte> object; ///< The object, empty when the compile failed.
    std::string stdoutText;
    std::string stderrText;
};

/// Runs compile jobs on this worker.
///
/// ## What this refuses to take from a client, and why
///
/// **The compiler.** A job names a *fingerprint*, never a program. The worker maps
/// that fingerprint to a path from its own configuration, and a fingerprint it does
/// not have is refused. This is the single most important property here: a job that
/// could name its own compiler would let anyone who can reach the port run an
/// arbitrary program, which is not a hardening detail but the difference between a
/// build accelerator and a remote shell.
///
/// **Any path.** The client's arguments have already been through
/// `RemoteCompileArgs`, which refuses a command line carrying anything that could
/// name a file. This checks again, on the receiving side, because the two checks
/// protect against different things: the client's protects an honest client from
/// dispatching something that would not work, and this one protects the worker from
/// a client that is not honest. A worker that trusted the client's filtering would
/// be secured by code running on the attacker's machine.
///
/// **Where anything is written.** The object path, the source path and the working
/// directory are all the worker's, inside a scratch directory it creates and
/// removes. Nothing the client sends decides where a byte lands.
///
/// The source *name* is used for exactly one thing: its extension, so the compiler
/// picks the right language. Even that is sanitized rather than trusted.
class CompileJobRunner
{
  public:
    /// @param runner Process spawning seam; must outlive the runner.
    /// @param scratchRoot Directory to create per-job scratch directories under.
    /// @param toolchains Fingerprint → compiler path. A job whose fingerprint is not
    ///        a key here is refused; there is deliberately no default entry.
    CompileJobRunner(IProcessRunner& runner,
                     std::filesystem::path scratchRoot,
                     std::map<std::string, std::string> toolchains);

    /// Run one job to completion.
    /// @param job The job.
    /// @return What the compiler produced, or why the job was refused.
    [[nodiscard]] std::expected<CompileOutcome, JobRefusal> Run(CompileJob const& job);

    /// The fingerprints this worker can serve, for its registration.
    /// @return Every configured fingerprint, sorted.
    [[nodiscard]] std::vector<std::string> Fingerprints() const;

  private:
    IProcessRunner& _runner;
    std::filesystem::path _scratchRoot;
    std::map<std::string, std::string> _toolchains;
    std::uint64_t _nextJob { 1 };
};

/// Whether `arg` is one this worker will pass to a compiler.
///
/// The receiving half of `RemoteCompileArgs`' rule, and deliberately a separate
/// implementation of the same idea rather than a shared call: this one has no
/// driver to ask, because a worker is told a fingerprint rather than a command, so
/// it cannot know which characters introduce an option, so it accepts both families'
/// and then refuses anything carrying a path separator or opening a response file.
///
/// That is marginally more permissive than the client's per-family rule — `/x` reads
/// as an option here where a GNU client would call it a path — and it does not
/// matter, because the separator check is what carries the weight and a bare `/x`
/// still has to survive it. What it must NOT do is test the raw argument: that
/// refuses `/O2` and therefore every MSVC job.
/// @param arg One argument from a job.
/// @return True when the worker will pass it on.
[[nodiscard]] bool IsAcceptableJobArgument(std::string_view arg);

/// The file name a job's scratch source may be given, sanitized.
///
/// The client asks for its own translation unit's base name, and it is worth having
/// rather than inventing one: a compiler records the name of the file it was handed
/// -- clang-cl and gcc in the `.file` symbol, MSVC in its compiland record -- so a
/// worker naming every input `tu.cpp` produces an object that differs from a
/// locally compiled one in that name and nothing else.
///
/// What it must never do is decide where anything GOES. The name arrives over a
/// socket and becomes a path under the scratch directory, so it is reduced to one
/// component and then to an allow-listed shape:
///
/// - the final component only, split on both separators and on a colon, so neither
///   a parent-directory escape nor a drive-relative `C:x` survives;
/// - a stem of `[A-Za-z0-9._+-]` with no leading dot, capped in length, which is
///   what makes `..` unspellable rather than merely unlikely;
/// - an extension from the same fixed table as before, defaulting to `.cpp`;
/// - never a Windows reserved device name (CON, NUL, COM1, ...), which on a Windows
///   worker names a device rather than a file and would send the translation unit
///   to the console instead of to disk.
///
/// Anything failing any of those yields `tu` plus a safe extension. A name never
/// fails a job: it is a cosmetic input, and refusing over one would cost a compile
/// to gain nothing.
///
/// The LANGUAGE no longer rides on this. Every driver family is now told the
/// language explicitly by the client (`-x c++-cpp-output`, `/TP`), which is what
/// closed a dispatched `.c` translation unit being compiled as C++ because the
/// worker had named its file `tu.cpp`.
///
/// @param sourceName The base name the client asked for.
/// @return A file name safe to create inside the scratch directory.
[[nodiscard]] std::string SafeSourceName(std::string_view sourceName);

} // namespace FastCache::Cc

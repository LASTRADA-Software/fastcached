// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "IProcessRunner.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
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
    /// An argument this worker will not pass to a compiler: one that is not on the
    /// per-driver-family allowlist of accepted flag shapes. Which argument it was
    /// travels in `JobError::detail`, so a fallback names the flag rather than
    /// leaving an operator to bisect a command line.
    RejectedArgument,
    /// The scratch directory could not be prepared, or the source could not be
    /// written into it.
    ScratchUnavailable,
    /// The compiler could not be spawned at all. Distinct from a compiler that ran
    /// and rejected the code: only the latter is the client's answer.
    SpawnFailed,
    Last, ///< Not a refusal, and has no row: `RefusalTable`'s length.
};

/// Why a job was refused, and — where there is one — which argument caused it.
///
/// The reason is still the enum `RefusalTable` is indexed by; `detail` is the
/// human-readable specifics that belong on the wire but not in a table. It carries
/// the offending flag for `RejectedArgument`, so a client's local fallback can say
/// *which* argument the fleet would not take rather than only that one existed —
/// over-refusal is otherwise invisible until someone correlates a hit-rate dip with
/// a build's flags by hand. Empty for every refusal that has nothing to add.
///
/// Compares equal to a bare `JobRefusal`, so a test that only cares about the reason
/// reads `result.error() == JobRefusal::RejectedArgument` unchanged.
struct JobError
{
    JobRefusal reason { JobRefusal::UnknownFingerprint }; ///< The reason, as `RefusalTable` keys it.
    /// Specifics for the wire; empty when there are none.
    ///
    /// **Anything derived from what a client sent must be built by
    /// `RejectedArgumentNaming`, never assigned here directly.** This string is
    /// encoded into the reply message and lands in the client's fallback log, so
    /// forwarding a peer's bytes verbatim would put unbounded, arbitrary content --
    /// control characters, terminal escapes, megabytes of it -- onto the wire and
    /// into a log. Detail set at the other refusal sites is this file's own literal
    /// text and needs no such treatment.
    std::string detail;

    /// A `RejectedArgument` refusal naming the offending argument.
    ///
    /// The one producer of a detail carrying client bytes, so the cap and the
    /// character rule live here rather than at each future call site that would have
    /// to remember them. @p argument is truncated and reduced to printable ASCII: it
    /// is enough to identify a flag, it cannot be a payload, and it is UTF-8 by
    /// construction — which the fleet requires of text a peer sent, and which a
    /// verbatim copy of an arbitrary byte string would not be.
    /// @param argument The argument the allowlist refused, as it arrived.
    /// @return The refusal.
    [[nodiscard]] static JobError RejectedArgumentNaming(std::string_view argument);

    /// @param error The error.
    /// @param reason The reason to compare against.
    /// @return True when the error names that reason, whatever its detail.
    [[nodiscard]] friend bool operator==(JobError const& error, JobRefusal reason) noexcept
    {
        return error.reason == reason;
    }
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
    ///
    /// **Callable from several threads at once**, which is what a worker serving
    /// `slots` compiles does through one of these. Every path a job writes hangs off
    /// the directory the job counter names, so no two jobs share one.
    ///
    /// The toolchain map is the one thing here that is NOT fixed at construction: a
    /// node re-surveys its machine when a compiler is patched underneath it and
    /// replaces the map (#238). So the compiler path is copied out under the lock at
    /// lookup and the copy is what the rest of this uses. Holding the map's iterator
    /// instead -- as this did -- left it dereferenced twice long downstream, after
    /// the scratch directory was made and the whole preprocessed source written, on
    /// the two lines that decide which program executes.
    ///
    /// The process runner it is given must accept concurrent calls too.
    /// @param job The job.
    /// @return What the compiler produced, or why the job was refused (with the
    ///         offending argument named, for a rejected one).
    [[nodiscard]] std::expected<CompileOutcome, JobError> Run(CompileJob const& job);

    /// The fingerprints this worker can serve, for its registration.
    /// @return Every configured fingerprint, sorted.
    [[nodiscard]] std::vector<std::string> Fingerprints() const;

    /// Serve a different set of toolchains from now on.
    ///
    /// The seam a node needs when the machine changes under it. A compiler patched
    /// in place -- a distro upgrade, a Windows SDK update -- keeps the node
    /// advertising the pre-upgrade fingerprint while spawning the post-upgrade
    /// compiler, so clients receive objects built by a compiler they did not key
    /// against and store them in the shared cache under the old key (#238).
    ///
    /// Replaces rather than merges, and that is the whole point: the fingerprint
    /// this worker can no longer honour has to STOP being served, which a merge
    /// would leave in place forever. A job naming it afterwards is refused
    /// `UnknownFingerprint` -- an answer this worker already gives, with a wire code
    /// and a counter of its own -- and its client compiles locally.
    ///
    /// **Safe against concurrent `Run` and `Fingerprints`.** Jobs already admitted
    /// keep the compiler they looked up; only the next lookup sees the new map.
    /// @param toolchains Fingerprint to compiler path, as the constructor takes it.
    void ReplaceToolchains(std::map<std::string, std::string> toolchains);

    /// Where scratch files are written.
    ///
    /// Exposed so a caller can report the space on *that* filesystem rather than
    /// on whichever one happens to hold the working directory — a worker's scratch
    /// directory is routinely a different mount, and a disk metric for the wrong
    /// one is worse than none.
    /// @return The scratch root.
    [[nodiscard]] std::filesystem::path const& ScratchRoot() const noexcept
    {
        return _scratchRoot;
    }

  private:
    IProcessRunner& _runner;
    std::filesystem::path _scratchRoot;

    /// Guards `_toolchains`. Mutable because `Fingerprints` is logically const and
    /// must still take it -- the alternative is a const method reading a map another
    /// thread is replacing.
    mutable std::shared_mutex _toolchainsMutex;
    std::map<std::string, std::string> _toolchains;
    /// Atomic because a worker runs `slots` compiles at once, on `slots` threads,
    /// through ONE of these.
    ///
    /// A plain `++` here let two jobs read the same number and derive the same
    /// scratch directory -- and with it the same source path and the same hard-coded
    /// `tu.o`. One then read the other's object and returned it to its client, which
    /// cached it under its own key: silent wrong-object delivery, which is the worst
    /// thing a compile cache can do. The gentler interleaving is one job's
    /// `ScratchGuard` deleting the directory under the other, reported as
    /// `ScratchUnavailable` and blamed on the disk.
    std::atomic<std::uint64_t> _nextJob { 1 };
};

/// Whether `arg` is one this worker will pass to @p driver.
///
/// **An allowlist, not a denylist**, and that inversion is the whole point. The
/// argument this worker splices into the argv is chosen by the client, the compile
/// port carries no credential, and loopback is admitted unconditionally — so a local
/// process reaches this check, and everything past it runs as the node's service
/// account. The question is therefore not "could this argument name a file?" (the old
/// filter's question, and the wrong one): a driver option whose *purpose* is to run
/// another program or load code into the driver carries no path separator —
/// `-wrapper prog,args`, `-fplugin=name`, `-Xclang -load` — so a shape-based denylist
/// admitted every one of them. The flag space belongs to GCC, Clang and Microsoft and
/// grows every release; a denylist we audit against upstream forever fails open the
/// day we miss one, and we learn we missed one from an incident.
///
/// So the accepted set is small, closed and ours: `AllowedArgs` names the flag shapes
/// a distributed compile legitimately carries — the code-generation, language and
/// diagnostic options — and everything else is refused. A refused argument costs one
/// local compile; an admitted program-invoking one is code execution. The failure
/// modes are not comparable, so the default is *refuse*.
///
/// The `-f` space is **enumerated rather than prefixed**, which is the load-bearing
/// half: a blanket `-f` prefix with a carve-out for `-fplugin=` reads as an allowlist
/// and behaves as a denylist, and both `-fmodule-mapper=|program args` (GCC spawns a
/// subprocess) and `-fpass-plugin=` (Clang's pass-manager loader) escape such a
/// carve-out. See `AllowedArgs` for which prefixes remain and why each is bounded.
///
/// The @p driver is this worker's OWN configured compiler, never anything the client
/// sent — the same rule that decides which program runs at all. It is a `DriverSpec`
/// rather than a bare family so the language spellings and the target-pin prefix come
/// from the driver's own table (`preprocessedInput`, `TargetPinPrefixFor`) instead of
/// being restated here, where they would drift and silently refuse every dispatched
/// job. An unclassifiable driver accepts no argument, and `CompileJobRunner::Run`
/// refuses such a job outright before this is ever asked.
///
/// This is the receiving half of `RemoteCompileArgs`' rule and deliberately stricter:
/// the client forwards anything without a path separator, the worker forwards only
/// what it recognises. A build using a flag the table does not yet cover falls back
/// to a local compile — visibly, via `WorkerJobsRefusedRejectedArgument` — rather than
/// exposing the port.
/// @param arg One argument from a job.
/// @param driver The descriptor for this worker's configured compiler.
/// @return True when the worker will pass it on.
[[nodiscard]] bool IsAcceptableJobArgument(std::string_view arg, DriverSpec const& driver);

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

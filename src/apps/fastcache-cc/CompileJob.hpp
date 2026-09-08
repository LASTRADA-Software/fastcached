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
#include <span>
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
    /// The directory the CLIENT's compile ran in, and what its own
    /// `-fdebug-prefix-map` rules spell that as. Both empty when the client maps
    /// nothing, and the worker must then map nothing either; a half-filled pair is
    /// refused.
    ///
    /// Neither is a path this worker opens — they become the two halves of the rules
    /// `WorkerPrefixMapRules` builds, and reach only the debug records of the object.
    std::string compileDir;
    std::string compileDirReplacement;
    /// The CLIENT's own spelling of its source path, and what its `-fdebug-prefix-map`
    /// rules make of it. Both empty when no rule governs the source, which is the
    /// ordinary case; a half-filled pair is refused.
    ///
    /// This is `sourceName`'s companion, not a duplicate of it. `sourceName` is the
    /// MAPPED half and is all clang needs, because clang takes `DW_AT_name` from the
    /// input file path -- which this worker owns and can therefore map with a rule it
    /// builds itself. gcc takes it from the `#line` marker inside the preprocessed
    /// text, which names the CLIENT's path: a string on no command line this worker
    /// sees and matching no rule derived from its scratch root
    /// ([#883](https://github.com/LASTRADA-Software/fastcached/issues/883)). A rewrite
    /// rule needs two operands, so the raw half has to travel too.
    ///
    /// Neither is a path this worker opens.
    std::string sourceRoot;
    std::string sourceRootReplacement;
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
    /// This worker has not finished identifying its toolchains, so it has no map to
    /// look the fingerprint up in yet.
    ///
    /// Reached only before the first survey completes. Kept apart from
    /// `UnknownFingerprint` because the two are the same sentence to a client and
    /// opposite instructions to an operator: one says the fleet is matching the
    /// wrong machines, the other says this machine is still coming up and will serve
    /// the identical request shortly (#365).
    ToolchainSurveyInFlight,
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
    /// What this worker actually compiled, tying the reply to its request (#280).
    ///
    /// Filled by the RUNNER from the values it used, never recomputed by the wire
    /// layer from the decoded request: at the wire layer both of two crossed requests
    /// are still pristine, so a digest taken there matches whatever it is compared
    /// against and catches nothing. See `CompileCorrelation`, which also records the
    /// half of the problem this cannot see.
    std::string correlation;
};

/// Runs one compile job, and the seam `WorkerProtocol` reaches a compiler through.
///
/// **Why this exists rather than `WorkerProtocol` naming `CompileJobRunner`.** This
/// project's rule is that anything touching I/O, the filesystem or process spawning is
/// reached through an injected interface, never a concrete type. `CompileJobRunner`
/// does all three, and was a concrete dependency of `WorkerProtocol` from the day it
/// was written. What hid it is that the inner `IProcessRunner` seam made the
/// arrangement *look* injected while sitting one layer too deep to substitute the
/// thing that matters.
///
/// That depth is demonstrable rather than a matter of taste. A fake `IProcessRunner`
/// receives `argv` AFTER the runner has recorded what it is about to compile, so it can
/// make the OUTPUT wrong but cannot make the execution diverge from the record. The
/// only crossed-job fixture the tree could build was therefore
/// [#279](https://github.com/LASTRADA-Software/fastcached/issues/279)'s half -- right
/// input, foreign object -- and never
/// [#280](https://github.com/LASTRADA-Software/fastcached/issues/280)'s, where a runner
/// reports on work other than the work it was asked for. See `CompileOutcome`'s
/// `correlation`.
///
/// Deliberately ONE method, because that is all `WorkerProtocol` calls. An interface
/// mirroring `CompileJobRunner`'s whole surface would be a second name for the class
/// rather than a seam, and every future fake would carry methods no test uses.
class ICompileJobRunner
{
  public:
    ICompileJobRunner() = default;
    virtual ~ICompileJobRunner() = default;
    ICompileJobRunner(ICompileJobRunner const&) = delete;
    ICompileJobRunner& operator=(ICompileJobRunner const&) = delete;
    ICompileJobRunner(ICompileJobRunner&&) = delete;
    ICompileJobRunner& operator=(ICompileJobRunner&&) = delete;

    /// Run one job to completion.
    /// @param job What to compile.
    /// @return The outcome, or why the job was refused before any compiler ran.
    [[nodiscard]] virtual std::expected<CompileOutcome, JobError> Run(CompileJob const& job) = 0;
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
/// Whether a runner's toolchain map is an ANSWER or an absence.
///
/// An empty map means two opposite things and a `std::map` cannot tell them apart:
/// this machine was surveyed and serves nothing, or it has not been surveyed yet. A
/// node now comes up and serves its cache tier while it walks its include trees --
/// over 300 s on a cold Windows runner
/// ([#354](https://github.com/LASTRADA-Software/fastcached/issues/354)) -- so the
/// second state is one a running worker is genuinely in, and a client that reaches
/// the compile port during it must be told "not yet" rather than "wrong toolchain".
/// Those send an operator to opposite conclusions
/// ([#365](https://github.com/LASTRADA-Software/fastcached/issues/365)).
///
/// **No default constructor**, for the reason `PreAuth` and `PayloadCap` have none:
/// a defaulted value would answer the question by omission, and the answer it would
/// give -- "surveyed" -- is exactly the conflation this exists to end. The compiler
/// asks; there is nothing to forget.
class ToolchainSurvey
{
  public:
    ToolchainSurvey() = delete;

    /// The map is not an answer yet. Every job is refused
    /// `JobRefusal::ToolchainSurveyInFlight` until `ReplaceToolchains` supplies one.
    /// @return The pre-survey state.
    [[nodiscard]] static ToolchainSurvey InFlight() noexcept
    {
        return ToolchainSurvey { false };
    }

    /// The map is what this machine serves, empty or not.
    /// @return The surveyed state.
    [[nodiscard]] static ToolchainSurvey Completed() noexcept
    {
        return ToolchainSurvey { true };
    }

    /// @return True once a survey has answered.
    [[nodiscard]] bool HasCompleted() const noexcept
    {
        return _completed;
    }

  private:
    explicit ToolchainSurvey(bool completed) noexcept:
        _completed { completed }
    {
    }

    bool _completed;
};

class CompileJobRunner final: public ICompileJobRunner
{
  public:
    /// @param runner Process spawning seam; must outlive the runner.
    /// @param scratchRoot Directory to create per-job scratch directories under.
    /// @param toolchains Fingerprint → compiler path. A job whose fingerprint is not
    ///        a key here is refused; there is deliberately no default entry.
    /// @param survey Whether @p toolchains is an answer or an absence. Undefaulted
    ///        on purpose; see `ToolchainSurvey`.
    CompileJobRunner(IProcessRunner& runner,
                     std::filesystem::path scratchRoot,
                     std::map<std::string, std::string> toolchains,
                     ToolchainSurvey survey);

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
    [[nodiscard]] std::expected<CompileOutcome, JobError> Run(CompileJob const& job) override;

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
    /// A survey has answered; serve this set from now on.
    ///
    /// Completes the survey as well as replacing the map, because the answer
    /// ARRIVING is what completes it -- the first call is the node's initial survey
    /// and every later one is a re-survey (#238), and neither can leave the runner
    /// claiming it has not been asked.
    /// @param toolchains Fingerprint → compiler path.
    void ReplaceToolchains(std::map<std::string, std::string> toolchains);

    /// Accept these extra argument spellings from now on, in addition to the
    /// built-in table.
    ///
    /// The seam `--allow-compile-arg` reaches, at startup and at every accepted
    /// reload. It exists because a built-in allowlist cannot be complete forever: a
    /// site using a legitimate flag the table does not yet name would otherwise wait
    /// for a release, and the failure is silent -- the build stays green and the
    /// fleet quietly stops distributing (#293).
    ///
    /// **Extends, never replaces.** These are consulted only after the built-in table
    /// has failed to recognise an argument; a `Deny` row has already refused by then,
    /// so no configuration can re-admit what the table refuses. That is enforced by
    /// `IsAcceptableJobArgument`'s order rather than by this comment.
    ///
    /// Replaces the previous SET, like `ReplaceToolchains`: an entry an operator
    /// removed from the file has to stop being honoured, which a merge would leave in
    /// place forever.
    ///
    /// **Safe against concurrent `Run`.** A job already admitted keeps the decision it
    /// was admitted under; only the next argument check sees the new set.
    /// @param spellings Whole, exact argument spellings.
    void ReplaceExtraAllowedArgs(std::vector<std::string> spellings);

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

    /// Guards `_extraAllowedArgs`. Its own lock rather than `_toolchainsMutex`: the
    /// two are replaced by different events -- a machine re-survey and an operator
    /// edit -- and sharing one would make a re-survey wait behind a config reload for
    /// no reason other than that both happen to be snapshots.
    mutable std::shared_mutex _extraAllowedArgsMutex;

    /// The operator's additions to the built-in allowlist, whole and exact.
    ///
    /// Empty is the shipped state and the ordinary one; a non-empty set is a
    /// deliberate widening of what a client may make this worker's compiler do.
    std::vector<std::string> _extraAllowedArgs;

    /// Whether `_toolchains` has been answered for. Under `_toolchainsMutex` with
    /// the map it qualifies, because the two are one fact and a reader that saw a
    /// stale survey beside a fresh map would refuse a job this worker can do.
    ToolchainSurvey _survey;

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
/// @param operatorAllowed Extra spellings this site has added to the built-in table
///        (`--allow-compile-arg`). **Consulted last, and only when no built-in row
///        matched at all** -- a `Deny` row has already returned by then, so a config
///        cannot re-admit anything the table refuses. Entries are matched WHOLE and
///        exactly: no prefix, no wildcard, because an `-f` prefix re-admits
///        `-fplugin=` and an `-X` prefix re-admits `-Xclang -load`, which is the
///        defect the built-in table is enumerated to avoid.
/// @return True when the worker will pass it on.
[[nodiscard]] bool IsAcceptableJobArgument(std::string_view arg,
                                           DriverSpec const& driver,
                                           std::span<std::string const> operatorAllowed = {});

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

/// The `-fdebug-prefix-map` rules a worker must add so its object records the
/// compilation directory the CLIENT's own mapping records.
///
/// ## Why the worker builds the rules rather than receiving them
///
/// `DW_AT_comp_dir` is the directory the compiler ran in. It is on no command line, so
/// no cache key can distinguish two producers by it, and a dispatched object recorded a
/// directory a locally mapped one does not, under the same key
/// ([#506](https://github.com/LASTRADA-Software/fastcached/issues/506)). One half of
/// the rule the worker needs is a path on THIS machine, which the client has never
/// seen, so the client sends its own directory and its own replacement and this builds
/// the rules.
///
/// ## TWO rules, because WHICH directory the object records is the driver's answer
///
/// Measured on gcc 14.2.0 and clang 20.1.2, one translation unit each way, reading
/// `DW_AT_comp_dir` off the worker's object:
///
/// | preprocess line | the worker's object records |
/// | --- | --- |
/// | `g++ -E` | the WORKER's directory |
/// | `g++ -E -g` | the CLIENT's directory |
/// | `clang++ -E`, `clang++ -E -g` | the WORKER's directory |
///
/// gcc's `-fworking-directory` is implicit under `-g` and emits a line marker naming
/// the preprocessing directory, which the worker's compile adopts; clang emits none. So
/// both candidates are mapped, to the same replacement, and the answer is the same
/// whichever the driver used. **Mapping only this worker's own directory fixes clang
/// and leaves gcc recording the client's UNMAPPED path** — the object comparison every
/// other case in the fleet's end-to-end fixture makes cannot see that, and reading
/// `comp_dir` can, which is why #506's acceptance clause insists on reading it.
///
/// ## What it refuses, and why each refusal is not a silent skip
///
///   - **No directory is not a refusal.** It is the client saying it maps nothing, and
///     a worker that mapped anyway would hand a build that asked for nothing an object
///     naming a directory neither machine has. An empty REPLACEMENT is not that case
///     either: `-fdebug-prefix-map=<builddir>=` maps a root to nothing and is a standard
///     reproducible-build spelling, so the DIRECTORY alone says whether a mapping is in
///     force. Only the reverse — a replacement with no directory — is refused, because
///     it would map everything.
///   - **Any of the three carrying anything but the shape below.** They are peer text
///     that ends up inside an artefact and, before that, on a command line. The set
///     allowed is deliberately narrower than `SafeSourceName`'s in one direction and
///     wider in another: never the row's own separator, no whitespace, quote or control
///     character (this is spliced into a command line and, on Windows, into a
///     `CreateProcessA` string), and bounded — but bytes at or above `0x80` ARE allowed,
///     because unlike a source name none of them ever becomes a path and a build
///     directory with a non-ASCII component is an ordinary thing to have. This worker's
///     OWN directory is checked too; it is the one value the client did not send, so it
///     is refused as the worker's fault rather than the client's.
///   - **The worker's own rule is DROPPED, not refused, when it would also match the
///     client's directory.** A prefix-map rule appends the unmatched tail, so a worker
///     directory of `/` rewrites `/home/ci/build` to `.home/ci/build` and every system
///     header to `.usr/include/...`. That is the production value — the shipped
///     `fastcache-compile-node.service` sets no `WorkingDirectory=` and
///     `PosixDaemonHost` calls `chdir("/")` — and measured on gcc 14.2.0 it produced a
///     `DW_AT_comp_dir` of `.tmp/…/client`, a WRONG object under a correct key and
///     strictly worse than the unmapped directory this closes. The client's rule still
///     lands, so the gcc case is fully mapped; what remains is clang on such a node,
///     which is the pre-#506 state rather than a new defect. Refusing instead would
///     cost every dispatched compile on every node installed from the shipped unit.
///   - **A compile directory containing `=`**, this worker's own included. gcc splits
///     `<from>=<to>` at the last separator and clang at the first — measured: a working
///     directory of `/tmp/l506b/eq=sign` mapped to `.` gives `.` under gcc and
///     `sign=.=sign` under clang. A wrong compilation directory is the defect this
///     closes, so the job is refused instead.
///   - **A driver with no such flag.** Read off `PathValueFlags()`'s prefix-map row
///     rather than tested by name, so the spelling, the separator and which families
///     accept it stay in the one table. `cl` has no path-map switch and clang-cl's
///     CodeView records are not remapped by one, which is why the row is GNU-only and
///     why an MSVC worker refuses rather than pretending.
///
/// @param workerDirectory The directory this worker's compiler will run in.
/// @param clientDirectory The directory the CLIENT's compile ran in; empty when the
///        client maps nothing.
/// @param replacement What the client asked that directory to read as; empty when the
///        client maps nothing.
/// @param family This worker's OWN driver family, never anything the client sent.
/// @return The arguments to append -- NONE when the client mapped nothing, which is
///         success and not a refusal -- or the `JobError` this job is refused with.
///         The two refusals are told apart at the source rather than by the caller: a
///         value the client sent is `RejectedArgument` and names the offending half,
///         a property of this machine is `SpawnFailed`. A caller reconstructing that
///         from which field is empty names the wrong half whenever the REPLACEMENT was
///         the offender.
[[nodiscard]] std::expected<std::vector<std::string>, JobError> WorkerPrefixMapRules(std::string_view workerDirectory,
                                                                                     std::string_view clientDirectory,
                                                                                     std::string_view replacement,
                                                                                     DriverFamily family);

/// The rule that makes a dispatched object record the CLIENT's source spelling.
///
/// A compiler with debug info on records the name of the file it was handed. gcc takes
/// it from the `#line` marker the preprocessed text carries, so a dispatched object
/// already reads what a locally built one does; **clang takes it from the input file
/// path**, so it recorded this worker's scratch —
/// [#660](https://github.com/LASTRADA-Software/fastcached/issues/660).
///
/// That is two defects and the second is the one that matters. The debug path names a
/// directory that exists on no developer's machine; and the scratch directory carries a
/// per-job counter, so two dispatches of the SAME translation unit to the SAME worker
/// produce byte-differing objects stored under one cache key. A compile cache exists to
/// deny exactly that.
///
/// **The whole path is mapped, not the directory**, which is the narrowest rule that
/// works: it matches exactly one path — the source file — so it cannot reach
/// `DW_AT_comp_dir` or anything else in the object, and it survives `SafeSourceName`
/// having renamed the scratch file, since the recorded name is the client's spelling on
/// both sides of that.
///
/// **Every way of not being able to build it is NO RULE, never a refusal.** That is the
/// difference from `WorkerPrefixMapRules` below, which is honouring a mapping the client
/// asked for and must refuse rather than silently skip. Nothing asked for this one: a
/// source file whose name carries a space, or an `=`, or a driver family with no
/// path-mapping switch at all, are ordinary things, and refusing them would stop
/// distributing those translation units to improve a debug record. Skipping an `=` is
/// also the NARROW choice rather than the lazy one — gcc cuts `<from>=<to>` at the last
/// separator and clang at the first, so such a rule records a name neither machine has,
/// which is worse than recording the worker's.
///
/// @param scratchSourcePath The path this compile is actually handed — the SANITIZED
///        one, since that is what the driver will match against.
/// @param clientSourceName The client's own spelling of its source, exactly as it sent
///        it. Never opened, never joined to a path: it is only ever the right-hand side
///        of a rule, and `SafeSourceName` is what decides the file this worker creates.
/// @param family This worker's OWN driver family, never anything the client sent.
/// @return The argument to append, or nothing when no unambiguous rule exists.
[[nodiscard]] std::optional<std::string> WorkerSourceNameRule(std::string_view scratchSourcePath,
                                                              std::string_view clientSourceName,
                                                              DriverFamily family);

/// The rule that makes a **gcc** dispatched object record the client's source spelling.
///
/// `WorkerSourceNameRule` above closes clang and cannot close gcc, and the reason is
/// where each driver reads `DW_AT_name` from. clang reads the INPUT FILE PATH, which is
/// this worker's scratch file, so a rule this worker builds from a path it chose itself
/// repairs it. gcc reads the `#line` marker in the preprocessed text, which names the
/// path on the CLIENT's machine -- a string this worker cannot derive from anything it
/// has ([#883](https://github.com/LASTRADA-Software/fastcached/issues/883)). So the
/// client sends both halves and this spells the rule.
///
/// The pair is sent only when a client-side rule actually CHANGED the spelling, so
/// `<x>=<x>` never crosses the wire and the ordinary case -- a relative source argument,
/// which matches no rule -- costs nothing.
///
/// **Emitted after `WorkerPrefixMapRules`' rules and before `WorkerSourceNameRule`'s.**
/// Both drivers honour the LAST matching rule, and all three can in principle match one
/// path: the replacement here is already the answer of every client rule applied in
/// order, so it must outrank the compilation-directory rule, while #660's rule stays
/// last for the reason its own comment gives.
///
/// **A half-filled pair is refused, unlike a directory's.** `WorkerPrefixMapRules`
/// accepts an empty replacement because `-fdebug-prefix-map=<builddir>=` is a real
/// reproducible-build spelling for a DIRECTORY; a source file mapped to nothing is not
/// a spelling of anything, so both directions of half a pair are malformed.
///
/// **A value that cannot be spelled inside a rule is NO RULE, never a refusal** -- the
/// same answer `WorkerSourceNameRule` gives and for the same reason: a source called
/// `my file.cpp` is ordinary, and refusing it would stop distributing that translation
/// unit to improve a debug record. Neither operand is this worker's own property, so
/// there is no startup warning to pair with it as #810 pairs with the scratch root.
///
/// @param clientSourcePath The client's raw spelling -- what its preprocessor wrote
///        into the `#line` marker, and so what gcc is about to record. Never opened.
/// @param replacement What a local compile on that machine would have recorded instead.
/// @param family This worker's OWN driver family, never anything the client sent.
/// @return The argument to append; nothing when the client mapped nothing or no
///         unambiguous rule exists; or the `JobError` a half-filled pair is refused
///         with.
[[nodiscard]] std::expected<std::optional<std::string>, JobError> WorkerSourcePathRule(std::string_view clientSourcePath,
                                                                                       std::string_view replacement,
                                                                                       DriverFamily family);

/// What an operator must be told about a scratch root no mapping rule can name.
///
/// `WorkerSourceNameRule` skips when its left-hand side cannot be spelled inside a
/// rule, and skipping is right for the CLIENT's half — a source file called
/// `my file.cpp` is ordinary. The worker's half is a different question with the same
/// answer at the call site: `scratchSourcePath` lies under a root this process chose
/// ONCE, so a root carrying a space or an `=` makes **every** rule this worker would
/// build unspellable, and every dispatched object it produces silently goes back to
/// recording `<scratch>/job-N/<name>` — nondeterministic between two dispatches of one
/// translation unit, stored under one cache key
/// ([#810](https://github.com/LASTRADA-Software/fastcached/issues/810)).
///
/// **A startup property is reported at startup, never decided per request.** That is
/// this repository's own rule about the worker's lease check, for the same reason: a
/// degradation decided per request leaves every counter reading zero and nothing said.
/// It is a WARNING and not a refusal — such a machine compiles perfectly well and only
/// its dispatched objects' debug names degrade, to exactly what they were before #660,
/// so taking it out of the fleet would cost compiles to buy a debug record.
///
/// Asked of **every prefix-map row the table can hold**, rather than of the families
/// this node actually serves. The served set exists only once the toolchain survey's
/// first round lands on the heartbeat thread (#365), which is minutes after the
/// operator stopped watching; a row the node serves nothing for costs one line naming
/// a flag, where waiting costs the audience the warning exists for.
///
/// @param scratchRoot The root this worker has claimed, as `CompileJobRunner` holds it.
/// @return One sentence per rule spelling this root cannot be written into; empty when
///         every one of them can, which is every ordinary deployment.
[[nodiscard]] std::vector<std::string> ScratchRootMappingWarnings(std::string_view scratchRoot);

} // namespace FastCache::Cc

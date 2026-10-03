// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace FastCache
{

/// How a daemon's process ended, in the terms a supervisor restarts on.
///
/// **A retry cannot change a refusal, and can change a failure** -- which is the whole
/// distinction, and the reason the two leave the process as different codes. A configuration
/// that is refused at one start is refused at every start, each writing the same reason into a
/// log until somebody looks; a port still held by a process that was just exiting is fixed by
/// the next one. So a supervisor that can tell the codes apart restarts the second and never the
/// first: every shipped systemd unit names `Refused`'s code in `RestartPreventExitStatus=`
/// (`ServiceControl_test` reads them back). The two supervisors that cannot are bounded instead,
/// and `ServiceRestartAttempts` says how.
///
/// Private: never transmitted or persisted as an enumerator. What leaves the process is the row's
/// `code`, and that is the contract a unit file holds.
enum class ProcessExit : std::uint8_t
{
    Served,  ///< It ran until it was asked to stop.
    Failed,  ///< It gave up for a reason a retry may change: a port held, a store busy, a file mid-rotation.
    Refused, ///< It refused to start on its configuration, or on a defect in its own build; the same start refuses again.
    Usage,   ///< A one-shot command refused what an operator asked of it; nothing supervises it.
    Last,
};

/// One way a daemon's process ends.
struct ProcessExitRow
{
    ProcessExit exit { ProcessExit::Served }; ///< Which one.
    int code { 0 };                           ///< What `main` returns, and what a supervisor reads.
    bool restarted { false };                 ///< Whether a supervisor that can tell it apart restarts it.
    bool endsAStart { true };                 ///< Whether a START ends this way -- what a supervisor ever sees.
    std::string_view meaning;                 ///< What an operator should read into it.
};

/// Every way a daemon's process ends, by what it means to a supervisor.
///
/// `Refused` is 78, `EX_CONFIG` in `sysexits.h` -- "something was found in an unconfigured or
/// misconfigured state" -- which systemd reports as `status=78/CONFIG`. It is also how a start ends
/// on a defect in this build's own wiring (`NodeRefusalCause::BuildDefect`), which BORROWS the code:
/// not a configuration, but just as certain to be met again, so the restart exemption is what it
/// needs too, and the reason in the log says which. Nothing else in either daemon returns it,
/// which is what makes it safe to exempt: a code a failure could also produce would stop the
/// restart of that failure too. `Failed` is `EXIT_FAILURE`.
///
/// `Usage` is 2, and it is no start's: a ONE-SHOT command -- `--print-surfaces`, `--install-service`,
/// `--migrate-storage`, a cluster verb -- is run by an operator at a terminal and never by a
/// supervisor, so the restart semantics of the other codes do not apply to it. It answers this one
/// when it DECLINES, having changed nothing -- the refusals decided before it runs included: a
/// configuration file that did not load, a command line that did not parse, an install the serving
/// rules refuse -- and `Failed` when it began changing something and did not finish
/// (`CommandEnding`). Both binaries reach it through `CommandExitCode`; no step of a start gives up
/// with it (`ProcessExit_test`).
inline constexpr auto ProcessExitRows = EnumTable<ProcessExit, ProcessExitRow> { {
    { .exit = ProcessExit::Served,
      .code = 0,
      .restarted = false,
      .meaning = "served until it was asked to stop, or a command did what it was asked" },
    { .exit = ProcessExit::Failed, .code = 1, .restarted = true, .meaning = "gave up for a reason a retry may change" },
    { .exit = ProcessExit::Refused,
      .code = 78,
      .restarted = false,
      .meaning = "refused to start on its configuration, or on a defect in its own build; the next start refuses it "
                 "again" },
    { .exit = ProcessExit::Usage,
      .code = 2,
      .restarted = false,
      .endsAStart = false,
      .meaning = "a one-shot command declined, having changed nothing; an operator reads it, not a supervisor" },
} };
static_assert(RowsInEnumeratorOrder(ProcessExitRows, [](ProcessExitRow const& row) { return row.exit; }),
              "every ProcessExit needs a row, at its own index");

/// What `main` returns for @p exit.
/// @param exit How the process ended.
/// @return The process exit code.
[[nodiscard]] constexpr int ExitCodeOf(ProcessExit exit) noexcept
{
    return ProcessExitRows[static_cast<std::size_t>(exit)].code;
}

// Every other site in both daemons that gives up returns `EXIT_FAILURE`, so the table must agree
// with it for "Failed" to be one code.
static_assert(ExitCodeOf(ProcessExit::Served) == EXIT_SUCCESS && ExitCodeOf(ProcessExit::Failed) == EXIT_FAILURE,
              "a clean stop and a failure are the C library's two codes");

/// A step of a daemon's start at which it can give up.
///
/// **Which exit is decided by the ARM that gave up, never by the step it is in**, and that is the
/// one rule of the table below. Only a VERDICT is `Refused`: on the command line, or on bytes that
/// were actually READ -- garbage, a foreign layout, an empty file, a contradiction -- which the
/// next start reads and judges the same way. Every I/O arm is `Failed`, whatever its errno and
/// without interpreting it: an open, a read, a directory created, a file written, a random draw.
/// It may succeed at the next start (a permission restored, a mount back), and the supervisor's
/// own start limit bounds the retries if it does not. So a step that both reads and judges has a
/// row per arm (`IdentityKey` and `IdentityKeyIo`), and an arm whose failures are of both kinds
/// is `Failed`: a refusal read as a failure is retried a bounded number of times, where a failure
/// read as a refusal is a service left stopped by something the next start would have fixed.
///
/// The compile node's serving body, where one tier's refusals are of several kinds, gives up by
/// the same rule through a table of its own causes (`NodeRefusalCauses`), carried by each refusal.
///
/// Private: never transmitted or persisted.
enum class StartStage : std::uint8_t
{
    CommandLine,         ///< argv does not parse.
    ConfigurationFile,   ///< What was read of the configuration does not load or assemble.
    ConfigurationFileIo, ///< The named configuration file is absent, or there and could not be read.
    NoOutcome,           ///< The command line selects nothing to do.
    StartupRules,        ///< A startup rule refuses the configuration (`StartupPolicyRejection`, `ServingRulesRejection`).
    IdentityKey,         ///< The key file was read, and is not a key this build can use or is reachable by others.
    IdentityKeyIo,       ///< The key file could not be read or written, or a new key's seed not drawn.
    NodeIdentity,        ///< The node's identity file was read, and holds no identity this build can use.
    NodeIdentityIo,      ///< The identity could not be read, recorded, or drawn.
    Formation,           ///< The node's formation record cannot be read, kept or applied.
    TlsMaterial,         ///< TLS is asked for with no certificate or key named.
    TlsUnavailable,      ///< TLS is asked for in a build without it.
    KeyspaceEvents,      ///< `--notify-keyspace-events` does not parse.
    TlsLoad,             ///< The named certificate or key does not load.
    Storage,             ///< The store does not open: held by another process, of another format, damaged.
    Last,
};

/// One step of a start, and the exit it gives up with.
struct StartStageRow
{
    StartStage stage { StartStage::CommandLine }; ///< Which step.
    ProcessExit exit { ProcessExit::Failed };     ///< How the process ends when the step gives up.
    std::string_view reads;                       ///< What the step judges, which is what decided `exit`.
};

/// Every step of a start at which a daemon can give up, and how it ends when it does.
inline constexpr auto StartStageRows = EnumTable<StartStage, StartStageRow> { {
    { .stage = StartStage::CommandLine, .exit = ProcessExit::Refused, .reads = "the command line" },
    { .stage = StartStage::ConfigurationFile,
      .exit = ProcessExit::Refused,
      .reads = "what the named file holds, the command line and the environment" },
    { .stage = StartStage::ConfigurationFileIo,
      .exit = ProcessExit::Failed,
      .reads = "whether the named file opens: a share or a mount not back yet, a permission" },
    { .stage = StartStage::NoOutcome, .exit = ProcessExit::Refused, .reads = "the command line" },
    { .stage = StartStage::StartupRules, .exit = ProcessExit::Refused, .reads = "the assembled configuration" },
    { .stage = StartStage::IdentityKey,
      .exit = ProcessExit::Refused,
      .reads = "the bytes of the node's own key file, and who may reach it and its state directory" },
    { .stage = StartStage::IdentityKeyIo,
      .exit = ProcessExit::Failed,
      .reads = "an open, a read or a write of the key file, and the operating system's generator" },
    { .stage = StartStage::NodeIdentity,
      .exit = ProcessExit::Refused,
      .reads = "the bytes of the node's own identity file" },
    { .stage = StartStage::NodeIdentityIo,
      .exit = ProcessExit::Failed,
      .reads = "reading or recording the identity file, creating its directory, and the generator" },
    { .stage = StartStage::Formation,
      .exit = ProcessExit::Failed,
      .reads = "reading, keeping and applying the formation record: a read or a write as well as its bytes" },
    { .stage = StartStage::TlsMaterial, .exit = ProcessExit::Refused, .reads = "the assembled configuration" },
    { .stage = StartStage::TlsUnavailable, .exit = ProcessExit::Refused, .reads = "the configuration and the build" },
    { .stage = StartStage::KeyspaceEvents, .exit = ProcessExit::Refused, .reads = "the assembled configuration" },
    { .stage = StartStage::TlsLoad,
      .exit = ProcessExit::Failed,
      .reads = "certificate and key files an operator replaces in place" },
    { .stage = StartStage::Storage, .exit = ProcessExit::Failed, .reads = "a store another process may hold" },
} };
static_assert(RowsInEnumeratorOrder(StartStageRows, [](StartStageRow const& row) { return row.stage; }),
              "every StartStage needs a row, at its own index");

/// The configuration errors that come from an I/O arm rather than from bytes that were read.
///
/// **Absence is one of them**, and deliberately: a start at boot can race the share or the mount a
/// named file lives on, so a file that is not there is retried within the supervisor's start
/// limit, exactly as one that is there and could not be read -- while a path that is simply wrong
/// still stops once the limit is spent. Only a verdict on what was read is the refusal.
inline constexpr auto ConfigurationIoCodes = std::to_array<ConfigErrorCode>({
    ConfigErrorCode::FileNotFound,
    ConfigErrorCode::FileUnreadable,
    ConfigErrorCode::WriteFailed,
});

/// Which arm of loading the configuration a configuration error came from.
/// @param code What the configuration could not be loaded with.
/// @return `ConfigurationFileIo` for a row of `ConfigurationIoCodes`, `ConfigurationFile` for a
///         verdict on what was read.
[[nodiscard]] constexpr StartStage ConfigurationFileStage(ConfigErrorCode code) noexcept
{
    return std::ranges::find(ConfigurationIoCodes, code) != ConfigurationIoCodes.end() ? StartStage::ConfigurationFileIo
                                                                                       : StartStage::ConfigurationFile;
}

/// How the process ends when @p stage gives up.
/// @param stage The step that gave up.
/// @return The exit its row names.
[[nodiscard]] constexpr ProcessExit ExitOf(StartStage stage) noexcept
{
    return StartStageRows[static_cast<std::size_t>(stage)].exit;
}

/// What `main` returns when @p stage gives up.
/// @param stage The step that gave up.
/// @return The process exit code.
[[nodiscard]] constexpr int ExitCodeFor(StartStage stage) noexcept
{
    return ExitCodeOf(ExitOf(stage));
}

/// How a ONE-SHOT command ended -- an install, an uninstall, a conversion, a seeded file, a
/// worksheet -- which an operator reads rather than a supervisor.
///
/// **What separates `Declined` from `Failed` is what a script calling it should do next.** 2 is
/// a DECISION, and running the command again unchanged gets the same answer: a verdict on its
/// arguments or on bytes it read, a refusal a peer REPLIED with, a store another process holds, a
/// precondition only the operator can change -- elevation, an account that does not exist, a name
/// the registration will not use. 1 is TRANSIENT, and a retry may get past it: an I/O arm that
/// failed (an open, a read, a write, a directory, a draw, a file that is not there), a transport
/// that delivered no reply, a system call that failed, a change stopped part-way that a re-run
/// finishes. That is the start's own rule (`StartStageRows`: a verdict is refused, an I/O arm
/// fails) asked of a command, which is why a step with a stage ends by its stage (`EndingOf`).
/// The result a command returns carries its ending, and `CommandExitCode` is the one mapping to a
/// code: no site spells a number.
///
/// **Its enumerators are in order of severity**, and that order is read: a command that acts on
/// several things -- a conversion over several shards -- ends as the WORST of its parts, `std::max`
/// over them (`ProcessExit_test` pins the order).
///
/// Private: never transmitted or persisted.
enum class CommandEnding : std::uint8_t
{
    Completed, ///< It did what it was asked.
    Declined,  ///< A decision: the same command gets the same answer.
    Failed,    ///< Transient, or stopped part-way: a retry may get past it.
    Last,
};

/// One way a one-shot command ends, and the exit that says so.
struct CommandEndingRow
{
    CommandEnding ending { CommandEnding::Last }; ///< Which one.
    ProcessExit exit { ProcessExit::Failed };     ///< What the process leaves as.
    std::string_view meaning;                     ///< What an operator should read into it.
};

/// Every way a one-shot command ends.
inline constexpr auto CommandEndingRows = EnumTable<CommandEnding, CommandEndingRow> { {
    { .ending = CommandEnding::Completed, .exit = ProcessExit::Served, .meaning = "did what it was asked" },
    { .ending = CommandEnding::Declined,
      .exit = ProcessExit::Usage,
      .meaning = "a decision: fix the request, since running it again unchanged gets the same answer" },
    { .ending = CommandEnding::Failed,
      .exit = ProcessExit::Failed,
      .meaning = "transient, or stopped part-way: a retry may get past it, and a re-run finishes a partial change" },
} };
static_assert(RowsInEnumeratorOrder(CommandEndingRows, [](CommandEndingRow const& row) { return row.ending; }),
              "every CommandEnding needs a row, at its own index");
static_assert(CommandEndingRows[static_cast<std::size_t>(CommandEnding::Declined)].exit == ProcessExit::Usage,
              "a declined command is what ProcessExit::Usage names");

/// What `main` returns when a one-shot command ends as @p ending.
/// @param ending How the command ended.
/// @return The process exit code.
[[nodiscard]] constexpr int CommandExitCode(CommandEnding ending) noexcept
{
    return ExitCodeOf(CommandEndingRows[static_cast<std::size_t>(ending)].exit);
}

/// A one-shot command that did not complete: how it ended, and why.
struct UnfinishedCommand
{
    CommandEnding ending { CommandEnding::Declined }; ///< A decision, or transient.
    std::string reason;                               ///< What to tell the operator.
};

/// How a one-shot command that did not complete ended, from the one fact that decides it.
/// @param transient Whether a retry may get past what stopped it.
/// @return `Failed` when it may, `Declined` when the stop is a decision.
[[nodiscard]] constexpr CommandEnding UnfinishedEnding(bool transient) noexcept
{
    return transient ? CommandEnding::Failed : CommandEnding::Declined;
}

/// How a one-shot command that stopped at @p stage ended: the start's own rule, asked of a command
/// -- an I/O arm is transient, a verdict is a decision.
/// @param stage The step that stopped it.
/// @return `Failed` for a stage a start fails at, `Declined` for one it is refused at.
[[nodiscard]] constexpr CommandEnding EndingOf(StartStage stage) noexcept
{
    return UnfinishedEnding(ExitOf(stage) == ProcessExit::Failed);
}

/// How a one-shot command that stopped on configuration error @p code ended, by the arm it came
/// from (`ConfigurationFileStage`): a file absent, unreadable or not written is transient, and a
/// verdict on what was read is a decision -- the start's split, asked of a command.
/// @param code Why it stopped.
/// @return `Failed` for a row of `ConfigurationIoCodes`, `Declined` for every other code.
[[nodiscard]] constexpr CommandEnding EndingOf(ConfigErrorCode code) noexcept
{
    return EndingOf(ConfigurationFileStage(code));
}

/// Who reads what a command line's outcome exits with, which decides what a refusal decided
/// BEFORE the outcome runs -- a command line that did not parse, a configuration file that did
/// not load, a rule that refused -- answers.
///
/// Private: never transmitted or persisted.
enum class ExitReader : std::uint8_t
{
    Supervisor, ///< A start: the step's own code, restarted or not by what it means.
    Operator,   ///< A one-shot command: the ending a command stopped at that step has.
    Prober,     ///< A health probe, whose reader knows only 0 and 1.
    Last,
};

/// One reader of an exit, and what a refusal before the outcome runs answers it.
struct ExitReaderRow
{
    ExitReader reader { ExitReader::Last };           ///< Which one.
    int (*exitCode)(StartStage) noexcept { nullptr }; ///< The code a refusal at a step is, for this reader.
    std::string_view why;                             ///< Why that answer, for that reader.
};

/// Every reader of an exit.
///
/// A start answers its `StartStage`'s code because a supervisor restarts on it. A one-shot command
/// answers the ending a command stopped at that step has (`EndingOf`): an I/O arm is transient and
/// a verdict a decision, as at a start, in the codes an operator reads rather than a supervisor.
/// **The derivation is per READER, not one shared answer, and the two part where the readers' next
/// move differs.** A store another process holds is 1 for a start (`StartStage::Storage`): a
/// supervisor's restart may find the lock released as the old process exits. It is 2 for
/// `--migrate-storage` and `--migrate-cache` (`CowTreeStorage::MigrationFailure::Transient`): a
/// verdict on a lock a live daemon holds until an operator stops it, which running the conversion
/// again unchanged meets again. A health probe answers `Failed`, which is
/// 1: Docker's HEALTHCHECK documents 0 and 1 and reserves 2, and a probe that could not even be
/// configured is an unhealthy answer rather than a start's 78.
inline constexpr auto ExitReaderRows = EnumTable<ExitReader, ExitReaderRow> { {
    { .reader = ExitReader::Supervisor,
      .exitCode = [](StartStage stage) noexcept { return ExitCodeFor(stage); },
      .why = "a supervisor restarts a failure and never a refusal, so the step's own code" },
    { .reader = ExitReader::Operator,
      .exitCode = [](StartStage stage) noexcept { return CommandExitCode(EndingOf(stage)); },
      .why = "an operator reads a decision as 2 and what a retry may get past as 1, by the step's arm" },
    { .reader = ExitReader::Prober,
      .exitCode = [](StartStage) noexcept { return CommandExitCode(CommandEnding::Failed); },
      .why = "a container runtime reads 0 or 1 only, and Docker reserves 2" },
} };
static_assert(RowsInEnumeratorOrder(ExitReaderRows, [](ExitReaderRow const& row) { return row.reader; }),
              "every ExitReader needs a row, at its own index");

/// What a refusal decided before an outcome runs exits with.
/// @param reader Who reads the outcome's exit.
/// @param stage The step that refused.
/// @return The step's own code for a supervisor, its ending's for an operator, 1 for a probe.
[[nodiscard]] constexpr int RefusalExitCode(ExitReader reader, StartStage stage) noexcept
{
    return ExitReaderRows[static_cast<std::size_t>(reader)].exitCode(stage);
}

} // namespace FastCache

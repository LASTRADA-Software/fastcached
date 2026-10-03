// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Node
{

/// Why a node's start gave up after the command line was judged -- in a tier, a surface, or the
/// state directory -- as the one fact a supervisor needs from it: would the next start give up
/// the same way.
///
/// **Decided by the ARM that refused, one row each** (`NodeRefusalCauses`), by `StartStageRows`'
/// rule: a verdict -- on the configuration, or on bytes that were actually READ -- reaches the same
/// answer at the next start, and is `Refused` (78, never restarted); an I/O arm, whatever its
/// errno, or a port, a store another process may hold, or material replaced in place may not,
/// and is `Failed` (restarted). A cause whose failures are of both kinds is `Failed`: a refusal
/// read as a failure is restarted a bounded number of times, where a failure read as a refusal is
/// a node left stopped by something the next start would have fixed.
///
/// Every `*OrExplain` refusal names one, which is what lets `main` end a deterministic refusal as
/// `Refused` without knowing which tier said it. `StartupRefusalCensus_test` holds every refusal
/// site to the cause it spells, and `NodeRefusal_test` every cause to its exit.
///
/// Private: never transmitted or persisted.
enum class NodeRefusalCause : std::uint8_t
{
    EarlierRule,         ///< A rule the startup table, `main` or an option parser answers first.
    HandedOverListeners, ///< The listeners a supervisor handed over, against `--advertise`.
    KeptRoster,          ///< The roster this node's state directory keeps, read and unusable, or its absence.
    CredentialFile,      ///< A credential file a flag names that was read, and holds nothing.
    LeaseValidation,     ///< How this worker would check a lease, against how it is reached.
    ConsensusState,      ///< A Raft state this build cannot read.
    KeptRosterIo,        ///< The kept roster is there, and could not be read.
    CredentialIo,        ///< A credential file that could not be opened, or whose read failed part way.
    Listener,            ///< A port that does not bind, or a listener that does not start.
    CacheStore,          ///< The `--cache-dir` and the store in it.
    ConsensusStore,      ///< Opening or recovering the Raft store.
    TlsMaterial,         ///< Loading or making the admin surface's TLS material.
    ScratchRoot,         ///< Claiming a scratch root another process may hold.
    ToolchainSurvey,     ///< A survey of this machine that found nothing to compile with.
    BuildDefect,         ///< How this build wired the node together: no configuration or host reaches it.
    Last,
};

/// One cause, and how the process ends when a start gives up with it.
struct NodeRefusalCauseRow
{
    NodeRefusalCause cause { NodeRefusalCause::Listener }; ///< Which.
    ProcessExit exit { ProcessExit::Failed };              ///< How the process ends.
    std::string_view reads;                                ///< What the refusal judges, which decided `exit`.
};

/// Every cause a node's start can give up with, and its exit.
inline constexpr auto NodeRefusalCauses = EnumTable<NodeRefusalCause, NodeRefusalCauseRow> { {
    { .cause = NodeRefusalCause::EarlierRule, .exit = ProcessExit::Refused, .reads = "the configuration" },
    { .cause = NodeRefusalCause::HandedOverListeners,
      .exit = ProcessExit::Refused,
      .reads = "the socket unit that handed the listeners over, and --advertise" },
    { .cause = NodeRefusalCause::KeptRoster,
      .exit = ProcessExit::Refused,
      .reads = "the bytes of the roster this node keeps, and whether it keeps one" },
    { .cause = NodeRefusalCause::CredentialFile,
      .exit = ProcessExit::Refused,
      .reads = "the bytes of a file the operator named" },
    { .cause = NodeRefusalCause::LeaseValidation,
      .exit = ProcessExit::Refused,
      .reads = "the configuration, and whether the listener was handed over" },
    { .cause = NodeRefusalCause::ConsensusState,
      .exit = ProcessExit::Refused,
      .reads = "what this node's own Raft store holds, against this build" },
    { .cause = NodeRefusalCause::KeptRosterIo,
      .exit = ProcessExit::Failed,
      .reads = "a read of the kept roster: a permission, a mount, whatever the errno" },
    { .cause = NodeRefusalCause::CredentialIo,
      .exit = ProcessExit::Failed,
      .reads = "an open or a read of a credential file: a permission, a mount, a provisioner not done yet" },
    { .cause = NodeRefusalCause::Listener,
      .exit = ProcessExit::Failed,
      .reads = "a port another process may hold, or a listener start that also judges its address" },
    { .cause = NodeRefusalCause::CacheStore,
      .exit = ProcessExit::Failed,
      .reads = "creating the directory and opening a store another process may hold" },
    { .cause = NodeRefusalCause::ConsensusStore,
      .exit = ProcessExit::Failed,
      .reads = "opening a store another process may hold, and reading it back" },
    { .cause = NodeRefusalCause::TlsMaterial,
      .exit = ProcessExit::Failed,
      .reads = "certificate and key files an operator replaces in place, or making them" },
    { .cause = NodeRefusalCause::ScratchRoot, .exit = ProcessExit::Failed, .reads = "a lock another process may hold" },
    { .cause = NodeRefusalCause::ToolchainSurvey,
      .exit = ProcessExit::Failed,
      .reads = "the compilers installed on this machine, which an install changes" },
    // Borrows 78, which `ProcessExit` documents as configuration's: no configuration reaches this,
    // but the next start meets it exactly as surely, and a restart is what 78 exists to prevent.
    { .cause = NodeRefusalCause::BuildDefect,
      .exit = ProcessExit::Refused,
      .reads = "how this build wired its components together, which the next start wires the same" },
} };
static_assert(RowsInEnumeratorOrder(NodeRefusalCauses, [](NodeRefusalCauseRow const& row) { return row.cause; }),
              "every NodeRefusalCause needs a row, at its own index");

/// How the process ends when a start gives up with @p cause.
/// @param cause Why it gave up.
/// @return The exit its row names.
[[nodiscard]] constexpr ProcessExit ExitOf(NodeRefusalCause cause) noexcept
{
    return NodeRefusalCauses[static_cast<std::size_t>(cause)].exit;
}

/// What `main` returns when a start gives up with @p cause.
/// @param cause Why it gave up.
/// @return The process exit code.
[[nodiscard]] constexpr int ExitCodeFor(NodeRefusalCause cause) noexcept
{
    return ExitCodeOf(ExitOf(cause));
}

/// How a one-shot command that stopped on @p cause ended: the start's rule asked of a command, so
/// a cause a start fails at is transient and one it is refused at is a decision.
/// @param cause Why it stopped.
/// @return `Failed` or `Declined`.
[[nodiscard]] constexpr CommandEnding EndingOf(NodeRefusalCause cause) noexcept
{
    return UnfinishedEnding(ExitOf(cause) == ProcessExit::Failed);
}

/// A node's refusal to start: what to tell the operator, and what it says to a supervisor.
struct NodeRefusal
{
    NodeRefusalCause cause { NodeRefusalCause::Listener }; ///< Decides the exit.
    std::string reason;                                    ///< What the operator reads.
};

/// A refusal of @p cause.
/// @param cause Why the start gave up.
/// @param reason What the operator reads.
/// @return The refusal.
[[nodiscard]] inline NodeRefusal Refusal(NodeRefusalCause cause, std::string reason)
{
    return NodeRefusal { .cause = cause, .reason = std::move(reason) };
}

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Node
{

/// Where a one-shot network verb's answer came from -- a cluster command, an enrollment command,
/// a machine asking to be let in, a cordon -- which is what decides how it ended.
///
/// **Classified by the SOURCE, never by the words.** A script acts on the exit: 1 is transient and
/// worth a retry, 2 is a decision and is not. A reply that never arrived -- a dial that failed, an
/// exchange that broke or timed out, a seal that did not verify -- says nothing about the request,
/// so it is transient. A reply that DID arrive and refused -- `NotLeader` with nobody left to ask,
/// `UnknownLease`, `NotAMember`, a body this build cannot read -- is the peer's decision, and asking
/// again unchanged gets it again. A refusal reached HERE before anything was dialled -- on what the
/// operator typed, or on what this machine already holds -- is the same decision made locally. And
/// a reply that arrived and decided NOTHING is transient like a reply that never came: a refusal
/// the WIRE calls retriable (`CompileCacheWire::ErrorDescriptor::retry` -- a change still
/// committing, a busy endpoint), a `NotLeader` naming nobody (an election), a redirect chain that
/// did not settle, a joiner's request still waiting for a person when the wait ran out. The code
/// is not the peer's words: it is a structured fact, and its permanence is the wire's to state.
///
/// Private: never transmitted or persisted.
enum class AnswerSource : std::uint8_t
{
    Local,     ///< Refused here, before anything was dialled: on what was typed, or on what this machine holds.
    Transport, ///< No reply arrived: the dial failed, or the exchange broke, timed out or failed its seal.
    Reply,     ///< The peer replied, and the reply refused -- or could not be read.
    Pending,   ///< The peer replied, and decided nothing: retriable, an election, waiting, or closed.
    Last,
};

/// One source, and how a verb whose answer came from it ended.
struct AnswerSourceRow
{
    AnswerSource source { AnswerSource::Last };     ///< Which one.
    CommandEnding ending { CommandEnding::Failed }; ///< How the verb ended.
    std::string_view why;                           ///< Why, for a script deciding whether to retry.
};

/// Every source of an answer, and the ending it means.
inline constexpr auto AnswerSourceRows = EnumTable<AnswerSource, AnswerSourceRow> { {
    { .source = AnswerSource::Local,
      .ending = CommandEnding::Declined,
      .why = "the same command on the same machine is refused the same way" },
    { .source = AnswerSource::Transport,
      .ending = CommandEnding::Failed,
      .why = "no answer was delivered, so a retry may reach the peer" },
    { .source = AnswerSource::Reply,
      .ending = CommandEnding::Declined,
      .why = "the peer decided, and asking again unchanged gets the same answer" },
    { .source = AnswerSource::Pending,
      .ending = CommandEnding::Failed,
      .why = "the peer decided nothing, so asking again after it does may succeed" },
} };
static_assert(RowsInEnumeratorOrder(AnswerSourceRows, [](AnswerSourceRow const& row) { return row.source; }),
              "every AnswerSource needs a row, at its own index");

/// Where an exchange's outcome came from: the one classifier over the client's own error type.
///
/// `Transport` is an exchange that never completed -- bytes that could not be framed as a reply
/// included. Every other kind is a reply that arrived, and one of those decided nothing: a refusal
/// whose code the wire calls retriable, or a `NotLeader` that names nobody to ask
/// (`Cc::RedirectTarget`, the one reading of that message), which is an election.
///
/// **A peer on another wire VERSION is a reply, and a decision**: the reply header carries no
/// version, so the server's range decides, and it answers a request outside it with
/// `UnsupportedVersion` -- a refusal that arrived, and one the same build gives again.
/// @param outcome What the exchange came to.
/// @return `Transport`, `Pending` or `Reply`.
[[nodiscard]] inline AnswerSource SourceOf(Cc::CacheOutcome const& outcome)
{
    if (outcome.kind == Cc::CacheOutcomeKind::Transport)
        return AnswerSource::Transport;
    if (outcome.kind != Cc::CacheOutcomeKind::Rejected)
        return AnswerSource::Reply;
    if (CompileCacheWire::IsRetriable(outcome.code))
        return AnswerSource::Pending;
    if (outcome.code == CompileCacheWire::ErrorCode::NotLeader && !Cc::RedirectTarget(outcome).has_value())
        return AnswerSource::Pending;
    return AnswerSource::Reply;
}

/// A one-shot network verb that did not complete, by where its answer came from.
/// @param source Where the answer came from.
/// @param reason What to tell the operator.
/// @return The unfinished command, its ending read off `AnswerSourceRows`.
[[nodiscard]] inline UnfinishedCommand Unanswered(AnswerSource source, std::string reason)
{
    return UnfinishedCommand { .ending = AnswerSourceRows[static_cast<std::size_t>(source)].ending,
                               .reason = std::move(reason) };
}

/// A one-shot network verb that did not complete, by the exchange's own outcome.
/// @param outcome What the exchange came to.
/// @param reason What to tell the operator.
/// @return The unfinished command, classified by `SourceOf`.
[[nodiscard]] inline UnfinishedCommand Unanswered(Cc::CacheOutcome const& outcome, std::string reason)
{
    return Unanswered(SourceOf(outcome), std::move(reason));
}

} // namespace FastCache::Node

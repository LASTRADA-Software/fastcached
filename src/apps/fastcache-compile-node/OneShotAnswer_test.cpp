// SPDX-License-Identifier: Apache-2.0
#include "OneShotAnswer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

using namespace FastCache;
using namespace FastCache::Node;
namespace Wire = FastCache::CompileCacheWire;

TEST_CASE("A one-shot network verb ends by where its answer came from", "[node][exit]")
{
    // Written out here rather than read back from `AnswerSourceRows`, so a source whose ending
    // changes -- or one added without a decision -- fails by name. A script retries a 1 and acts on
    // a 2: no answer delivered is transient, and so is a reply that decided nothing yet; an answer
    // made here or a refusal the peer replied with is not.
    struct SourceEnding
    {
        AnswerSource source;
        CommandEnding ending;
    };
    constexpr auto verdicts = std::to_array<SourceEnding>({
        { .source = AnswerSource::Local, .ending = CommandEnding::Declined },
        { .source = AnswerSource::Transport, .ending = CommandEnding::Failed },
        { .source = AnswerSource::Reply, .ending = CommandEnding::Declined },
        { .source = AnswerSource::Pending, .ending = CommandEnding::Failed },
    });
    for (auto const source: Enumerators<AnswerSource>())
        CHECK(std::ranges::count(verdicts, source, &SourceEnding::source) == 1);
    for (auto const& verdict: verdicts)
    {
        INFO(static_cast<int>(verdict.source));
        CHECK(Unanswered(verdict.source, "why").ending == verdict.ending);
        CHECK(CommandExitCode(Unanswered(verdict.source, "why").ending)
              == (verdict.ending == CommandEnding::Failed ? 1 : 2));
    }
}

TEST_CASE("An exchange that never completed is transport, and a reply that decided nothing is pending", "[node][exit]")
{
    // The one classifier over the client's own error type. A miss or a hit is not a refusal, but it
    // ARRIVED -- a verb that cannot use it has the peer's answer, not a broken connection. A refusal
    // is the peer's decision unless the wire calls its code retriable, or it is a `NotLeader` that
    // names nobody: an election, which settles by itself.
    struct OutcomeSource
    {
        std::string_view why;
        Cc::CacheOutcomeKind kind;
        Wire::ErrorCode code;
        std::string_view message;
        AnswerSource source;
    };
    constexpr auto verdicts = std::to_array<OutcomeSource>({
        { .why = "a hit arrived",
          .kind = Cc::CacheOutcomeKind::Hit,
          .code = Wire::ErrorCode::MalformedFrame,
          .message = "",
          .source = AnswerSource::Reply },
        { .why = "a miss arrived",
          .kind = Cc::CacheOutcomeKind::Miss,
          .code = Wire::ErrorCode::MalformedFrame,
          .message = "",
          .source = AnswerSource::Reply },
        { .why = "nothing framed arrived",
          .kind = Cc::CacheOutcomeKind::Transport,
          .code = Wire::ErrorCode::MalformedFrame,
          .message = "",
          .source = AnswerSource::Transport },
        { .why = "a refusal the peer decided",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::NotAMember,
          .message = "not one of ours",
          .source = AnswerSource::Reply },
        { .why = "a peer on another wire version",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::UnsupportedVersion,
          .message = "supported 13..13",
          .source = AnswerSource::Reply },
        { .why = "a change still committing",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::ClusterChangeInFlight,
          .message = "one at a time",
          .source = AnswerSource::Pending },
        { .why = "an endpoint at its bound",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::EndpointBusy,
          .message = "",
          .source = AnswerSource::Pending },
        { .why = "a fleet full of its own work",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::NoCapacity,
          .message = "",
          .source = AnswerSource::Pending },
        // A fleet that is booting has no worker for anything yet: a node registers nothing until its
        // survey is real, and a restarted scheduler's registry starts empty.
        { .why = "no worker registered for this toolchain yet",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::NoWorker,
          .message = "",
          .source = AnswerSource::Pending },
        { .why = "an election: a NotLeader naming nobody",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::NotLeader,
          .message = "this node does not lead the cluster",
          .source = AnswerSource::Pending },
        { .why = "a NotLeader naming the leader",
          .kind = Cc::CacheOutcomeKind::Rejected,
          .code = Wire::ErrorCode::NotLeader,
          .message = "10.0.0.9:7000",
          .source = AnswerSource::Reply },
    });
    for (auto const& verdict: verdicts)
    {
        INFO(verdict.why);
        auto outcome = Cc::CacheOutcome {};
        outcome.kind = verdict.kind;
        outcome.code = verdict.code;
        outcome.message = std::string { verdict.message };
        CHECK(SourceOf(outcome) == verdict.source);
        CHECK(Unanswered(outcome, "why").ending == Unanswered(verdict.source, "why").ending);
    }
}

TEST_CASE("A refusal is pending exactly when the wire calls its code retriable", "[node][exit]")
{
    // Every code the table knows, so a code whose permanence changes changes its exit with it and a
    // new code arrives already decided. `NotLeader` is the one refusal read further, by its message.
    for (auto const& row: Wire::ErrorTable)
    {
        if (row.code == Wire::ErrorCode::NotLeader)
            continue;
        INFO(row.name);
        auto outcome = Cc::CacheOutcome {};
        outcome.kind = Cc::CacheOutcomeKind::Rejected;
        outcome.code = row.code;
        outcome.message = "words the classifier does not read";
        CHECK(SourceOf(outcome) == (row.retry.MayHelp() ? AnswerSource::Pending : AnswerSource::Reply));
    }
}

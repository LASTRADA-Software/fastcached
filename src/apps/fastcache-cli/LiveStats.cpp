// SPDX-License-Identifier: Apache-2.0
#include "LiveStats.hpp"

#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    /// The subject keys, for a refusal that lists them: `cache, node, fleet`.
    /// @return The keys, in table order.
    [[nodiscard]] std::string SubjectKeys()
    {
        auto keys = std::string {};
        for (auto const& row: LiveSubjectTable)
        {
            if (!keys.empty())
                keys += ", ";
            keys += row.key;
        }
        return keys;
    }

    /// The refusal for an interval outside @p spec's wire bounds, or nothing.
    ///
    /// Names the bound, the subject it belongs to, and who pays for a snapshot: an operator told only
    /// *too short* cannot tell whether the limit is a mistake or a cost, and `fleet`'s is a cost
    /// landing on a machine they are not looking at. Refused rather than clamped, because the
    /// operator TYPED the interval, and one silently replaced is a dashboard stating a cadence
    /// nobody asked for.
    /// @param spec The subject whose bounds apply.
    /// @param interval What the operator asked for, or nullopt when they asked for nothing.
    /// @return The refusal, or nullopt when there is nothing to refuse.
    [[nodiscard]] std::optional<Answer> OutsideBounds(LiveSubjectSpec const& spec,
                                                      std::optional<std::chrono::milliseconds> interval)
    {
        if (!interval.has_value())
            return std::nullopt;
        if (*interval < FloorOf(spec))
            return Concluded(Outcome::Usage,
                             std::format("--interval={} is below the `{}` floor of {}: {}",
                                         FormatDuration(*interval),
                                         spec.key,
                                         FormatDuration(FloorOf(spec)),
                                         spec.costsWhom));
        if (*interval > CompileCacheWire::MaxLiveCadence)
            return Concluded(Outcome::Usage,
                             std::format("--interval={} is above the longest cadence a live-stats stream keeps, {}",
                                         FormatDuration(*interval),
                                         FormatDuration(CompileCacheWire::MaxLiveCadence)));
        return std::nullopt;
    }

    /// What a refusal says when RESP opened and `0xFC` identified nothing.
    ///
    /// The observation, not a remedy: the address evidently reaches SOMETHING, so *check
    /// that --addr reaches a fastcached* would send an operator to verify an address that
    /// works. What is known is that the thing there speaks RESP and would not say what it
    /// is -- a Redis-compatible server that is not this project, or a fastcached older
    /// than the question.
    constexpr std::string_view RespButUnidentified =
        "so this is not a fastcached, or one too old to identify itself over 0xFC";

    /// The subject an endpoint of @p kind is given when none is named.
    ///
    /// The row whose `inferredAt` is @p kind: unique by `EveryKindInfersAtMostOneSubject`, and
    /// served there by `EveryInferredSubjectIsServed`.
    /// @param kind What the endpoint turned out to be.
    /// @return The row, or nullptr when no subject is inferred at that kind.
    [[nodiscard]] LiveSubjectSpec const* InferredSubject(RemoteKind kind) noexcept
    {
        return FindOrNull(LiveSubjectTable, std::optional { kind }, &LiveSubjectSpec::inferredAt);
    }
} // namespace

LiveSubjectSpec const* FindLiveSubject(std::string_view key) noexcept
{
    return FindOrNull(LiveSubjectTable, key, &LiveSubjectSpec::key);
}

std::expected<LivePlan, Answer> AdmitLiveStats(VerbContext const& context)
{
    // What can be refused WITHOUT asking the endpoint is refused first, so a typo costs no
    // identification: a word naming no subject, and a named subject's floor. Not yet no
    // CONNECTION -- `main` opens the wire's connections before any handler runs.
    LiveSubjectSpec const* named = nullptr;
    if (!context.operands.empty())
    {
        auto const& operand = context.operands.front();
        named = FindLiveSubject(operand);
        if (named == nullptr)
            return std::unexpected(Concluded(
                Outcome::Usage,
                std::format("`{}` names nothing live-stats watches; expected one of: {}", operand, SubjectKeys())));
        if (auto refusal = OutsideBounds(*named, context.options.interval); refusal.has_value())
            return std::unexpected(*std::move(refusal));
    }

    // Not a failure to reach anything -- nothing was WIRED to ask, which is the
    // three-state rule the `fleet` verb keeps for its own admin seam.
    if (context.identity == nullptr)
        return std::unexpected(Concluded(Outcome::Usage, "no endpoint identity is available to this invocation"));

    auto identity = context.identity->IdentifyEndpoint();

    // **Nobody could ask, and that is a refusal -- never `cache`.** Inferring a cache here
    // would report *INFO did not answer* against a port that may speak no RESP. And a
    // NAMED subject is refused too, because an assertion about the endpoint that cannot
    // be checked is not one this view may act on. The remedy is therefore about the
    // address, not about naming a subject: telling the operator to name one would send
    // them straight back into this same refusal -- unless RESP opened, and then the
    // address is not in question either, so the refusal says what was observed instead.
    // Refused all the same (§9.7): RESP alone identifies nothing, and a plain Redis `INFO`
    // would not fill a cache panel anyway.
    if (!identity.kind.has_value())
    {
        if (context.resp != nullptr)
            return std::unexpected(Concluded(Outcome::Unreachable,
                                             std::format("cannot tell what this endpoint is: a RESP connection opened, "
                                                         "but {}; {}",
                                                         identity.detail,
                                                         RespButUnidentified)));
        return std::unexpected(Concluded(Outcome::Unreachable,
                                         std::format("cannot tell what this endpoint is ({}); check that --addr "
                                                     "reaches a fastcached or a fastcache-compile-node",
                                                     identity.detail)));
    }
    auto const kind = *identity.kind;

    // What an endpoint of this kind makes of a subject it cannot serve is the kind's own
    // row, so this admission and `RunNodeFallback` give one endpoint one outcome. A kind
    // that establishes nothing answered in a protocol this client does not speak: no
    // subject, named or not, can be watched there, and `Protocol` is the outcome that says
    // the peer may not be a fastcache at all.
    auto const established = EstablishedBy(kind);
    if (!established.has_value())
    {
        if (context.resp != nullptr)
            return std::unexpected(Concluded(
                Outcome::Protocol,
                std::format("cannot watch this endpoint: a RESP connection opened, but 0xFC did not answer ({}); {}",
                            identity.detail,
                            RespButUnidentified)));
        return std::unexpected(Concluded(Outcome::Protocol, std::format("cannot watch this endpoint: {}", identity.detail)));
    }

    // A node whose own description this client could not read is refused whatever was named:
    // it answered in a shape this build's wire version does not have, so every sample would
    // fail the same way, and the session would stream nothing but gaps. Said by version, since
    // that is what an operator changes -- the node's cannot be read, so this client's is named.
    if (identity.unreadable)
        return std::unexpected(
            Concluded(Outcome::Protocol,
                      std::format("cannot watch this node: {}. This client speaks 0xFC wire {}, and a node on another wire "
                                  "version answers in a shape it cannot read -- upgrade the node and this client together",
                                  identity.detail,
                                  static_cast<unsigned>(CompileCacheWire::CurrentVersion))));

    auto const* const subject = named != nullptr ? named : InferredSubject(kind);
    if (subject == nullptr)
        return std::unexpected(
            Concluded(*established,
                      std::format("nothing live-stats watches is inferred for this endpoint ({}); name one of: {}",
                                  identity.detail,
                                  SubjectKeys())));

    // **Asserted, not requested** (#134 §1.2): the refusal names what the endpoint turned
    // out to be, because a bare *refused* leaves the operator guessing which of the two --
    // their address or their subject -- is wrong. An inferred subject matches by
    // construction (`EveryInferredSubjectIsServed`), so only a named one can be refused here.
    if (!subject->servedBy.Contains(kind))
        return std::unexpected(Concluded(
            *established, std::format("`live-stats {}` needs {}, and {}", subject->key, subject->needs, identity.detail)));

    // A named subject's floor was checked before asking. An INFERRED one's is known only
    // now, which is why this is the one floor refusal that costs a round trip.
    if (auto refusal = OutsideBounds(*subject, context.options.interval); refusal.has_value())
        return std::unexpected(*std::move(refusal));

    return LivePlan { .subject = subject->subject,
                      .server = kind,
                      .interval = context.options.interval.value_or(subject->defaultInterval),
                      .samples = context.options.samples.value_or(0),
                      .endpoint = std::move(identity.detail) };
}

Answer LiveStatsVerb(VerbContext const& context)
{
    auto plan = AdmitLiveStats(context);
    if (!plan.has_value())
        return std::move(plan).error();

    auto const& spec = LiveSubjectTable[static_cast<std::size_t>(plan->subject)];
    return Answered(RecordValue({
        Field { .name = "subject", .value = TextCell(std::string { spec.key }) },
        Field { .name = "interval_ms", .value = NumberCell(static_cast<std::int64_t>(plan->interval.count())) },
        // *No bound* is an answer, not an absence: rendered absent it would claim the
        // bound was never reported, when it was decided and is none.
        Field { .name = "samples",
                .value =
                    plan->samples == 0 ? TextCell("unbounded") : NumberCell(static_cast<std::uint64_t>(plan->samples)) },
        Field { .name = "endpoint", .value = TextCell(plan->endpoint) },
    }));
}

} // namespace FastCache::Cli

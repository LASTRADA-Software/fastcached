// SPDX-License-Identifier: Apache-2.0
#include "LiveStats.hpp"

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

    /// The refusal for an interval below @p spec's floor, or nothing.
    ///
    /// Names the floor, the subject it belongs to, and who pays for a sample: an
    /// operator told only *too short* cannot tell whether the limit is a mistake or a
    /// cost, and `fleet`'s is a cost landing on a machine they are not looking at.
    /// @param spec The subject whose floor applies.
    /// @param interval What the operator asked for, or nullopt when they asked for nothing.
    /// @return The refusal, or nullopt when there is nothing to refuse.
    [[nodiscard]] std::optional<Answer> BelowFloor(LiveSubjectSpec const& spec,
                                                   std::optional<std::chrono::milliseconds> interval)
    {
        if (!interval.has_value() || *interval >= spec.minInterval)
            return std::nullopt;
        return Concluded(Outcome::Usage,
                         std::format("--interval={} is below the `{}` floor of {}ms: {}",
                                     interval->count(),
                                     spec.key,
                                     spec.minInterval.count(),
                                     spec.costsWhom));
    }

    /// The inferrable subject an endpoint of @p kind is given.
    ///
    /// Derived from `servedBy`, and unique by `EveryKindInfersAtMostOneSubject`.
    /// @param kind What the endpoint turned out to be.
    /// @return The row, or nullptr when no inferrable subject is served by that kind.
    [[nodiscard]] LiveSubjectSpec const* InferredSubject(RemoteKind kind) noexcept
    {
        for (auto const& row: LiveSubjectTable)
            if (row.inferrable && row.servedBy == kind)
                return &row;
        return nullptr;
    }
} // namespace

LiveSubjectSpec const* FindLiveSubject(std::string_view key) noexcept
{
    for (auto const& row: LiveSubjectTable)
        if (row.key == key)
            return &row;
    return nullptr;
}

std::expected<LivePlan, Answer> AdmitLiveStats(VerbContext const& context)
{
    // What can be refused WITHOUT asking the endpoint is refused first, so a typo costs no
    // round trip: a word naming no subject, and a named subject's floor.
    LiveSubjectSpec const* named = nullptr;
    if (!context.operands.empty())
    {
        auto const& operand = context.operands.front();
        named = FindLiveSubject(operand);
        if (named == nullptr)
            return std::unexpected(Concluded(
                Outcome::Usage,
                std::format("`{}` names nothing live-stats watches; expected one of: {}", operand, SubjectKeys())));
        if (auto refusal = BelowFloor(*named, context.options.interval); refusal.has_value())
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
    // them straight back into this same refusal.
    if (!identity.kind.has_value())
        return std::unexpected(Concluded(Outcome::Unreachable,
                                         std::format("cannot tell what this endpoint is ({}); check that --addr "
                                                     "reaches a fastcached or a fastcache-compile-node",
                                                     identity.detail)));
    auto const kind = *identity.kind;

    // The port answered in a protocol this client does not speak. No subject, named or
    // not, can be watched there, and `Protocol` is the outcome that says the peer may not
    // be a fastcache at all.
    if (kind == RemoteKind::NotFastcacheWire)
        return std::unexpected(Concluded(Outcome::Protocol, std::format("cannot watch this endpoint: {}", identity.detail)));

    auto const* subject = named;
    if (subject == nullptr)
    {
        subject = InferredSubject(kind);
        if (subject == nullptr)
            return std::unexpected(
                Concluded(Outcome::Refused,
                          std::format("nothing live-stats watches is inferred for this endpoint ({}); name one of: {}",
                                      identity.detail,
                                      SubjectKeys())));
        // The floor of an INFERRED subject is known only now, which is why this check is
        // the one refusal that costs a round trip.
        if (auto refusal = BelowFloor(*subject, context.options.interval); refusal.has_value())
            return std::unexpected(*std::move(refusal));
    }
    else if (subject->servedBy != kind)
    {
        // **Asserted, not requested** (#134 §1.2): the refusal names what the endpoint
        // turned out to be, because a bare *refused* leaves the operator guessing which of
        // the two -- their address or their subject -- is wrong.
        return std::unexpected(
            Concluded(Outcome::Refused,
                      std::format("`live-stats {}` needs {}, and {}", subject->key, subject->needs, identity.detail)));
    }

    return LivePlan { .subject = subject->subject,
                      .interval = context.options.interval.value_or(subject->defaultInterval),
                      .samples = context.options.samples.value_or(0),
                      .identity = std::move(identity) };
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
        Field { .name = "endpoint", .value = TextCell(plan->identity.detail) },
    }));
}

} // namespace FastCache::Cli

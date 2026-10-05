// SPDX-License-Identifier: Apache-2.0
#include "FleetProbe.hpp"

#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/PeerText.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Transport/TotalDeadlineSocket.hpp>

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <CacheProtocol.hpp>
#include <core/async/SyncRun.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    // The question's nonce is drawn by the one nonce primitive, so its width is the wire field's
    // by construction rather than by two constants agreeing.
    static_assert(NonceBytes == Wire::NodeChallengeBytes, "a fleet-summary question carries exactly one nonce");
} // namespace

DialledFleetProbe::DialledFleetProbe(IEndpointDialer& dialer,
                                     ISecureRandom& random,
                                     core::platform::IClock const& clock) noexcept:
    _dialer { dialer },
    _clock { clock },
    _issuer { random }
{
}

std::expected<DialledFleetProbe::Answered, std::string> DialledFleetProbe::Exchange(std::string_view endpoint)
{
    // Taken first, so the connect is inside the deadline too.
    auto const deadline = _clock.now() + ExchangeDeadline;
    // Drawn per question and never kept: the nonce is what makes a recorded answer worthless, so
    // it lives exactly as long as the one exchange it asks. A failed draw is THIS host's fault,
    // and is said so -- never a question with a weak nonce, which a recorded answer could meet.
    auto nonce = _issuer.IssueNonce();
    if (!nonce.has_value())
        return std::unexpected { std::format(
            "cannot ask {} which fleet it is in: this node cannot draw a nonce ({})", endpoint, nonce.error().ToString()) };

    auto dialled = _dialer.Dial(endpoint, core::net::DialOptions { .connectTimeout = ConnectTimeout });
    if (dialled == nullptr)
        return std::unexpected { std::format("cannot reach {} to ask which fleet it is in", endpoint) };
    auto client = TotalDeadlineSocket { std::move(dialled), _clock, deadline };

    // No credential: the verb is answered before authentication, to strangers, by design.
    auto notice = Cc::CredentialNotice { [](std::string_view /*text*/) {} };
    auto const outcome = core::async::syncRun(Cc::ExchangeFramed(
        &client,
        &notice,
        Wire::EncodeFleetSummaryRequest(std::span<std::byte const, Wire::NodeChallengeBytes> { nonce->wire })));

    switch (outcome.kind)
    {
        case Cc::CacheOutcomeKind::Hit:
            break;
        case Cc::CacheOutcomeKind::Rejected:
            return std::unexpected { std::format(
                "{} would not say which fleet it is in: {}", endpoint, BoundedPeerText(outcome.message, MaxRefusalText)) };
        case Cc::CacheOutcomeKind::Transport:
            // Said apart, because *too slow* and *gone* are opposite diagnoses: only the deadline
            // knows it is what ended the exchange.
            if (client.Expired())
                return std::unexpected { std::format(
                    "{} did not answer which fleet it is in within {} ms", endpoint, ExchangeDeadline.count()) };
            return std::unexpected { std::format(
                "asking {} which fleet it is in {}", endpoint, Cc::DescribeTransportFailure(outcome.transportFailure)) };
        case Cc::CacheOutcomeKind::Miss:
            return std::unexpected { std::format("{} answered which fleet it is in with nothing", endpoint) };
    }

    auto reply = Wire::DecodeFleetSummaryReply(outcome.value);
    if (!reply.has_value())
        return std::unexpected { std::format("{} answered which fleet it is in with a reply this build cannot read",
                                             endpoint) };
    return Answered { .held = std::move(nonce->held), .reply = *std::move(reply) };
}

namespace
{
    /// What a probe says of an answer whose signature does not verify over its question.
    /// @param endpoint Whom it asked.
    /// @return The sentence.
    [[nodiscard]] std::string NotSignedOverThisQuestion(std::string_view endpoint)
    {
        return std::format(
            "{} answered which fleet it is in with a summary not signed over this question by the key it carries", endpoint);
    }
} // namespace

std::expected<Cluster::ProvenFleet, std::string> DialledFleetProbe::Ask(Cluster::SeedCandidate const& seed)
{
    auto answered = Exchange(seed.endpoint);
    if (!answered.has_value())
        return std::unexpected { std::move(answered).error() };

    // Only what the signature proves, over THIS question's nonce: an answer signed over another
    // nonce is somebody replaying one, and names nobody here.
    auto proven = Cluster::ProvenFleet::FromSeedAnswer(std::move(answered->held), answered->reply, seed.source);
    if (!proven.has_value())
        return std::unexpected { NotSignedOverThisQuestion(seed.endpoint) };
    return *std::move(proven);
}

std::expected<Cluster::ProvenFleetSummary, std::string> DialledFleetProbe::AskSummary(std::string_view endpoint)
{
    auto answered = Exchange(endpoint);
    if (!answered.has_value())
        return std::unexpected { std::move(answered).error() };
    auto proven = Cluster::ProvenFleetSummary::VerifyAnswer(std::move(answered->held), answered->reply);
    if (!proven.has_value())
        return std::unexpected { NotSignedOverThisQuestion(endpoint) };
    return *std::move(proven);
}

} // namespace FastCache::Node

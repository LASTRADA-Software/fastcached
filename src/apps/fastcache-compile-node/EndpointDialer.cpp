// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialer.hpp"

#include <format>
#include <ranges>
#include <utility>

#include <EndpointDial.hpp>
#include <core/net/BlockingConnector.hpp>

namespace FastCache::Node
{

BlockingEndpointDialer::BlockingEndpointDialer(std::chrono::milliseconds ioTimeout) noexcept:
    _ioTimeout { ioTimeout }
{
}

std::unique_ptr<core::net::ISocket> BlockingEndpointDialer::Dial(std::string_view endpoint, core::net::DialOptions options)
{
    // **The concrete type never leaves this function**, which is what preserves
    // `Cc::DialEndpointBlocking`'s guard: it takes a `BlockingConnector&` because its
    // soundness rests on the connector resolving inline and never leaving its task
    // suspended. Injecting one level up keeps that rule intact while making the
    // REPLIES scriptable.
    core::net::BlockingConnector connector { core::net::defaultAddressResolver(),
                                             core::net::BlockingConnectorOptions { .ioTimeout = _ioTimeout } };
    return Cc::DialEndpointBlocking(connector, endpoint, options);
}

IEndpointDialer& DefaultOneShotDialer() noexcept
{
    static BlockingEndpointDialer instance { OneShotIoTimeout };
    return instance;
}

std::optional<ReachedEndpoint> DialFirstReachable(IEndpointDialer& dialer,
                                                  std::span<std::string const> endpoints,
                                                  core::net::DialOptions options)
{
    for (auto const& endpoint: endpoints)
        if (auto socket = dialer.Dial(endpoint, options); socket != nullptr)
            return ReachedEndpoint { .socket = std::move(socket), .endpoint = endpoint };
    return std::nullopt;
}

std::expected<LeaderAnswer, UnfinishedCommand> AskTheLeader(IEndpointDialer& dialer,
                                                            std::span<std::string const> schedulers,
                                                            core::net::DialOptions options,
                                                            std::string_view subject,
                                                            LeaderAsk const& ask)
{
    std::optional<std::string> leader;
    // `MaxLeaderRedirects + 1` because the bound is inclusive and `iota` is half-open: three
    // redirects means four asks.
    for (auto const hop: std::views::iota(0, MaxLeaderRedirects + 1))
    {
        auto const targets = leader.has_value() ? std::span<std::string const> { &*leader, 1 } : schedulers;
        auto reached = DialFirstReachable(dialer, targets, options);
        if (!reached.has_value())
            return std::unexpected { Unanswered(AnswerSource::Transport,
                                                std::format("cannot reach {} at {}", subject, JoinEndpoints(targets))) };

        auto outcome = ask(*reached->socket, reached->endpoint);
        if (outcome.kind == Cc::CacheOutcomeKind::Transport)
            return std::unexpected { Unanswered(outcome,
                                                std::format("{} at {} did not answer", subject, reached->endpoint)) };

        // Followed only after a refusal, so nothing was applied where this landed.
        if (auto named = Cc::RedirectTarget(outcome); named.has_value())
        {
            if (hop < MaxLeaderRedirects)
            {
                leader = std::move(*named);
                continue;
            }
            // A chain that did not settle: stale leaders naming each other until leadership does,
            // which decided nothing about this request (`Pending`).
            return std::unexpected { Unanswered(
                AnswerSource::Pending,
                std::format("gave up after {} leader redirect(s); the last, from {}, named {}",
                            MaxLeaderRedirects,
                            reached->endpoint,
                            *named)) };
        }
        return LeaderAnswer { .outcome = std::move(outcome), .endpoint = std::move(reached->endpoint) };
    }
    // Unreachable: the last hop returns above whatever it was answered. Answered rather than
    // asserted, as the redirect exhaustion it would be.
    return std::unexpected { Unanswered(AnswerSource::Pending,
                                        std::format("gave up after {} leader redirect(s)", MaxLeaderRedirects)) };
}

std::string JoinEndpoints(std::span<std::string const> endpoints)
{
    std::string joined;
    for (auto const& endpoint: endpoints)
    {
        if (!joined.empty())
            joined += ", ";
        joined += endpoint;
    }
    return joined;
}

} // namespace FastCache::Node

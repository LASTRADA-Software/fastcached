// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "CordonCli.hpp"
#include "EndpointDial.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <array>
#include <chrono>
#include <format>

#include <core/Ranges.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// How long to wait for this machine's own node to answer.
    ///
    /// A cordon is answered under a lock, not after a compile, so a node that has not
    /// answered in this long is not busy -- it is not there.
    constexpr std::chrono::milliseconds DialTimeout { 5'000 };

    /// One row per bind host that names no address to dial, and the loopback that
    /// reaches a listener bound there.
    struct WildcardRow
    {
        std::string_view bound;    ///< What the surface was bound to.
        std::string_view loopback; ///< What to dial instead.
    };

    /// Every wildcard spelling a `--listen-node` resolves to.
    constexpr std::array WildcardRows {
        WildcardRow { .bound = "", .loopback = "127.0.0.1" },
        WildcardRow { .bound = "0.0.0.0", .loopback = "127.0.0.1" },
        WildcardRow { .bound = "::", .loopback = "::1" },
        WildcardRow { .bound = "[::]", .loopback = "::1" },
    };

    /// What an operator reads for each state: the state, and what it means for them.
    struct CordonStateRow
    {
        Wire::WireCordonState state; ///< The state this row describes.
        std::string_view sentence;   ///< What it means for somebody about to stop this node.
    };

    /// Every state a worker reports.
    constexpr std::array CordonStateRows {
        CordonStateRow { .state = Wire::WireCordonState::Serving, .sentence = "serving: this worker takes compiles" },
        CordonStateRow { .state = Wire::WireCordonState::Draining,
                         .sentence = "draining: cordoned, finishing what it is running; stopping this node now "
                                     "abandons those compiles" },
        CordonStateRow { .state = Wire::WireCordonState::Drained,
                         .sentence = "drained: cordoned, nothing running; stopping this node now abandons nothing" },
    };
} // namespace

std::string SelfDialEndpoint(SurfaceEndpoint const& bound)
{
    auto const* const wildcard =
        core::findIfOrNull(WildcardRows, [&bound](WildcardRow const& row) { return row.bound == bound.host; });
    return FormatHostPort(wildcard != nullptr ? wildcard->loopback : std::string_view { bound.host }, bound.port);
}

std::string RenderCordonReply(Wire::CordonFields const& fields)
{
    auto const* const row =
        core::findIfOrNull(CordonStateRows, [&fields](CordonStateRow const& r) { return r.state == fields.state; });
    // `DecodeCordonFields` refuses a state this build does not name, so every decoded
    // reply has a row; the fallback is for a caller that built one by hand.
    auto const sentence = row != nullptr ? row->sentence : std::string_view { "in a state this build does not name" };
    if (fields.state == Wire::WireCordonState::Draining)
        return std::format("worker {} ({} compile(s) still running)\n", sentence, fields.inFlight);
    return std::format("worker {}\n", sentence);
}

std::expected<std::string, UnfinishedCommand> PutCordonRequest(core::net::ISocket& client,
                                                               Wire::CordonAction action,
                                                               std::string_view endpoint)
{
    // Through the launcher's own exchange, for `PutClusterRequest`'s reason: a second copy of the
    // framing would differ. Silent and never consulted: a notice reports a credential the peer
    // ignored, and this exchange presents none.
    auto notice = Cc::CredentialNotice::Silent();
    auto const outcome = core::async::syncRun(Cc::ExchangeFramed(&client, &notice, Wire::EncodeCordonRequest(action)));

    if (outcome.kind == Cc::CacheOutcomeKind::Transport)
        return std::unexpected { Unanswered(outcome, std::format("the node at {} did not answer", endpoint)) };
    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
        return std::unexpected { Unanswered(outcome, Cc::DescribeOutcome(outcome)) };

    auto const fields = Wire::DecodeCordonFields(outcome.value);
    if (!fields.has_value())
        return std::unexpected { Unanswered(outcome,
                                            std::format("{} answered with a body this client cannot read", endpoint)) };
    return RenderCordonReply(*fields);
}

std::expected<std::string, UnfinishedCommand> RunCordonAdmin(NodeConfig const& cfg, CordonCommand command)
{
    auto const bound = SoleEndpointOf(NodeSurface::Node, cfg);
    if (!bound.has_value())
        return std::unexpected { Unanswered(AnswerSource::Local, bound.error()) };
    auto const endpoint = SelfDialEndpoint(*bound);

    // A one-shot CLI on the process main thread: no reactor exists here, so this
    // legitimately blocks -- `RunClusterAdmin`'s idiom, for the same situation.
    core::net::BlockingConnector connector { core::net::defaultAddressResolver(),
                                             core::net::BlockingConnectorOptions { .ioTimeout = DialTimeout } };
    auto client = Cc::DialEndpointBlocking(connector, endpoint, core::net::DialOptions { .connectTimeout = DialTimeout });
    if (client == nullptr)
        return std::unexpected { Unanswered(
            AnswerSource::Transport,
            std::format("cannot reach this machine's node at {}; a cordon is asked of the node running here", endpoint)) };

    auto const action = command == CordonCommand::Cordon ? Wire::CordonAction::Cordon : Wire::CordonAction::Lift;
    return PutCordonRequest(*client, action, endpoint);
}

} // namespace FastCache::Node

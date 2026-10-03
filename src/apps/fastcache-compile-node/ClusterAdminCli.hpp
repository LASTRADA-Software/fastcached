// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "NodeConfig.hpp"
#include "OneShotAnswer.hpp"

#include <FastCache/Cluster/ClusterState.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <TicketCredentials.hpp>
#include <core/net/ISocket.hpp>

namespace FastCache::Node
{

/// Frame the request this action makes.
/// @param request What the operator asked for.
/// @return The framed request, or empty for `ClusterAction::None`.
[[nodiscard]] std::vector<std::byte> EncodeClusterRequest(ClusterRequest const& request);

/// Render a cluster's state for a terminal.
///
/// Plain aligned text rather than a machine format, and deliberately: the audience
/// is a person deciding whether the fleet looks right. Anything that wants to parse
/// it should speak the wire, which is what this binary is doing on their behalf.
/// @param state What the cluster has agreed.
/// @return The rendered report, ending in a newline.
[[nodiscard]] std::string RenderClusterState(Cluster::ClusterState const& state);

/// Turn a reply into what the operator should see.
///
/// Separated from the exchange so it can be tested against bytes rather than a
/// socket -- which matters most for the failure arms, since the interesting replies
/// here are the refusals and provoking a real one needs a cluster.
/// @param action What was asked.
/// @param reply The reply payload, on success.
/// @return What to print, or what went wrong.
[[nodiscard]] std::expected<std::string, std::string> InterpretClusterReply(ClusterAction action,
                                                                            std::span<std::byte const> reply);

/// What one cluster-administration exchange came back with.
struct ClusterExchange
{
    Cc::CacheOutcome outcome;                  ///< The scheduler's answer.
    std::optional<Cc::MintFailure> missing {}; ///< Why no ticket was presented, when one was due.
};

/// Put one cluster-administration request to an already-connected scheduler.
///
/// Split out of `RunClusterAdmin` so that the step which PRESENTS A CREDENTIAL can
/// be driven against a scripted socket without composing a dial around it.
///
/// The credential is asked for HERE, at the exchange, and for the endpoint @p client is connected
/// to: a machine ticket names one audience, so a verb that dialled the second `--scheduler` because
/// the first did not connect presents a ticket for the second (#404's rule, per endpoint).
/// @param client A connected scheduler; not owned.
/// @param notice Where "your credential went unchecked" is reported.
/// @param request What to ask.
/// @param credentials What each endpoint is shown, asked at the moment of the exchange.
/// @param scheduler Where @p client is connected: the audience a ticket names.
/// @return The answer, and what the exchange presented.
[[nodiscard]] ClusterExchange PutClusterRequest(core::net::ISocket& client,
                                                Cc::CredentialNotice& notice,
                                                ClusterRequest const& request,
                                                Cc::ICredentialFor& credentials,
                                                std::string_view scheduler);

/// Turn the answer a cluster verb ended on into what the operator should see.
/// @param action What was asked.
/// @param answer The last exchange: never a redirect `RunClusterAdmin` followed.
/// @param scheduler Who gave it.
/// @return What to print, or what went wrong and where the answer came from (`AnswerSource`).
[[nodiscard]] std::expected<std::string, UnfinishedCommand> InterpretClusterAnswer(ClusterAction action,
                                                                                   ClusterExchange const& answer,
                                                                                   std::string_view scheduler);

/// Carry out one cluster-administration request against `cfg.schedulers`.
///
/// The one impure step: connect, exchange, interpret. Everything it decides lives in
/// the functions above, which is what lets `main.cpp` -- in no test target -- hold
/// nothing but the call.
///
/// Asks the FIRST configured scheduler that connects (`DialFirstReachable`) and follows a
/// `NotLeader` to the leader it names, bounded (`AskTheLeader`) -- a second PC admitting a machine
/// reaches the leader wherever it is. Following is safe for a mutating verb because a `NotLeader`
/// is a refusal: nothing was proposed where it landed. A connection that fails mid-exchange is
/// reported and never retried elsewhere, because `--cluster-admit` may already have been proposed
/// where it landed (#1310).
/// @param cfg Where the scheduler is.
/// @param request What to ask.
/// @param credentials What each endpoint is shown: `OperatorCredentials` in production.
/// @param dialer How each endpoint is reached; production never varies it.
/// @return What to print, or what went wrong and where the answer came from (`AnswerSource`).
[[nodiscard]] std::expected<std::string, UnfinishedCommand> RunClusterAdmin(
    NodeConfig const& cfg,
    ClusterRequest const& request,
    Cc::ICredentialFor& credentials,
    IEndpointDialer& dialer = DefaultOneShotDialer());

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// Put one cluster-administration request to an already-connected scheduler.
///
/// Split out of `RunClusterAdmin` so that the step which PRESENTS A CREDENTIAL can
/// be driven against a scripted socket. The dial cannot: `RunClusterAdmin` takes a
/// `BlockingConnector` by type on purpose -- that is what keeps "this legitimately
/// blocks" checkable rather than a comment -- so a test reaching through it would
/// need a listening port to say anything about the bytes that went out.
///
/// The credential arrives as the seam rather than as a value for the reason every
/// other site takes it that way (#404). This verb runs once and exits, so nothing can
/// rotate underneath it today -- but a site that reads the live secret cannot become
/// the one stale site later, and "the one that did not need it" is exactly how three
/// sites came to be two-thirds correct.
/// @param client A connected scheduler; not owned.
/// @param notice Where "your credential went unchecked" is reported.
/// @param request What to ask.
/// @param credential What to present, asked at the moment of the exchange.
/// @param scheduler Where @p client is connected, for the diagnostics.
/// @return What to print, or what went wrong.
[[nodiscard]] std::expected<std::string, std::string> PutClusterRequest(ISocket& client,
                                                                        Cc::CredentialNotice& notice,
                                                                        ClusterRequest const& request,
                                                                        ICredentialSource const& credential,
                                                                        std::string_view scheduler);

/// Carry out one cluster-administration request against `cfg.scheduler`.
///
/// The one impure step: connect, exchange, interpret. Everything it decides lives in
/// the functions above, which is what lets `main.cpp` -- in no test target -- hold
/// nothing but the call.
/// @param cfg Where the scheduler is.
/// @param request What to ask.
/// @param credential What to present. A `FixedCredential` in production, because
///        these verbs run before any reloader exists and return without serving.
/// @return What to print, or what went wrong.
[[nodiscard]] std::expected<std::string, std::string> RunClusterAdmin(NodeConfig const& cfg,
                                                                      ClusterRequest const& request,
                                                                      ICredentialSource const& credential);

} // namespace FastCache::Node

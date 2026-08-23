// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Cluster/ClusterState.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
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

/// Carry out one cluster-administration request against `cfg.scheduler`.
///
/// The one impure step: connect, exchange, interpret. Everything it decides lives in
/// the functions above, which is what lets `main.cpp` -- in no test target -- hold
/// nothing but the call.
/// @param cfg Where the scheduler is, and the credential to present.
/// @param request What to ask.
/// @return What to print, or what went wrong.
[[nodiscard]] std::expected<std::string, std::string> RunClusterAdmin(NodeConfig const& cfg,
                                                                      ClusterRequest const& request);

} // namespace FastCache::Node

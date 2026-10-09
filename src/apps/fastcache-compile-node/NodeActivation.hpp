// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeRefusal.hpp"

#include <FastCache/Transport/NativeListen.hpp>

#include <expected>
#include <optional>

/// @file NodeActivation.hpp
/// What a socket a supervisor handed over makes of the configuration: the node surface's bind is
/// the socket's, so every endpoint derived from that bind -- the advertised one first -- describes
/// the socket clients actually reach.

namespace FastCache::Node
{

/// The refusal a start gives when the socket a supervisor handed over will not say which address
/// and port it listens on: then nothing this node could advertise would describe it.
/// @return The refusal, `NodeRefusalCause::HandedOverListeners`.
[[nodiscard]] NodeRefusal UnreadableActivatedBindRefusal();

/// Make the configuration describe the socket a supervisor handed over: the Node surface's bind
/// becomes the descriptor's bound host and port, so the advertised endpoint is derived as for any
/// bind -- a wildcard socket in `auto` mode advertises the address this machine routes from on the
/// socket's port, a socket bound to one address advertises that address, and an `--advertise` that
/// pins a value still wins.
///
/// Only the value is adopted, never its provenance: `nodeListenExplicit` keeps saying whether the
/// operator typed `--listen-node`, because the rules that read that bit judge what the operator
/// promised, and a unit's `ListenStream=` is not a promise this command line made.
/// @param cfg The configuration to adopt the bind into; unchanged on a refusal.
/// @param bound Where the handed-over socket is bound, as the socket reports it.
/// @return Nothing, or the refusal when that bound address is not one the Node surface can hold
///         (no host, or port 0).
[[nodiscard]] std::expected<void, NodeRefusal> AdoptActivatedBind(NodeConfig& cfg, BoundEndpoint const& bound);

/// `AdoptActivatedBind` over what the descriptor answered when asked where it is bound.
/// @param cfg The configuration to adopt the bind into; unchanged on a refusal.
/// @param bound Where the handed-over socket is bound, or nothing when it would not say.
/// @return Nothing, or the refusal when the descriptor cannot be asked what it is bound to
///         (`UnreadableActivatedBindRefusal`) or its answer cannot be held.
[[nodiscard]] std::expected<void, NodeRefusal> AdoptActivatedBind(NodeConfig& cfg,
                                                                  std::optional<BoundEndpoint> const& bound);

} // namespace FastCache::Node

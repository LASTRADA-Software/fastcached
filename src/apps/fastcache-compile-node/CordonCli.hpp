// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <expected>
#include <string>
#include <string_view>

#include <CacheProtocol.hpp>
#include <core/net/ISocket.hpp>

namespace FastCache::Node
{

/// Where a `--cordon` run on this machine dials its own node.
///
/// **The node's own `--listen-node`, never `--scheduler` or `--advertise`.** A cordon is
/// answered by this machine only -- the worker refuses one from anywhere else -- so the
/// address to ask is the one this node listens on, reached from here. A wildcard bind
/// names no address to dial, and loopback of the same family is the one that reaches it
/// and is certainly this machine; any other bind host is dialled as written, since the
/// node listens there and nowhere else.
/// @param bound What `SoleEndpointOf(NodeSurface::Node, cfg)` resolved.
/// @return `host:port` to dial.
[[nodiscard]] std::string SelfDialEndpoint(SurfaceEndpoint const& bound);

/// Render a cordon reply for a terminal.
/// @param fields What the worker answered.
/// @return The report, ending in a newline.
[[nodiscard]] std::string RenderCordonReply(CompileCacheWire::CordonFields const& fields);

/// Put one cordon request to an already-connected node.
///
/// Split from `RunCordonAdmin` for `PutClusterRequest`'s reason: the dial takes a
/// `core::net::BlockingConnector` by type, so the exchange is what a scripted socket can drive.
/// @param client A connected node; not owned.
/// @param notice Where "your credential went unchecked" is reported.
/// @param action Cordon, or lift it.
/// @param credential What to present, asked at the moment of the exchange.
/// @param endpoint Where @p client is connected, for the diagnostics.
/// @return What to print, or what went wrong.
[[nodiscard]] std::expected<std::string, std::string> PutCordonRequest(core::net::ISocket& client,
                                                                       Cc::CredentialNotice& notice,
                                                                       CompileCacheWire::CordonAction action,
                                                                       ICredentialSource const& credential,
                                                                       std::string_view endpoint);

/// Carry out one `--cordon` or `--uncordon` against this machine's own node.
/// @param cfg Where the node listens.
/// @param command What the operator asked for; never `CordonCommand::None`.
/// @param credential What to present.
/// @return What to print, or what went wrong.
[[nodiscard]] std::expected<std::string, std::string> RunCordonAdmin(NodeConfig const& cfg,
                                                                     CordonCommand command,
                                                                     ICredentialSource const& credential);

} // namespace FastCache::Node

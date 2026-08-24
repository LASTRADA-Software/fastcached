// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <memory>
#include <string_view>

namespace FastCache::Cc
{

/// Dial an endpoint written as one string, e.g. `"127.0.0.1:6674"`.
///
/// The join between `Core/HostPort` and `Net/TcpClient`, and it lives up here
/// rather than in either of them on purpose. `Net` is meant to be liftable out of
/// this codebase on its own, so it must not reach into `Core` for a grammar its
/// caller can apply first; and `Core/HostPort` knows nothing about sockets. One
/// place doing the join is what keeps six call sites across the launcher and the
/// node from each writing their own -- and what keeps the split going through the
/// one parser, since `rfind(':')` picks the wrong colon in `[::1]:7000`.
///
/// The same timeout bounds each blocking send and recv as bounds the dial. They
/// are separate parameters on `FastCache::ConnectTcp` because they answer
/// different questions, but every caller up here wants the same answer to both:
/// "give up rather than hold up the build".
///
/// @param hostPort Endpoint text; a hostname, an IPv4 literal, or `[v6]:port`.
/// @param connector How to dial. Injected rather than constructed here, because
///        which connector this is decides whether the caller's thread blocks: a
///        component on a reactor passes `PlatformConnector`, one on a thread that
///        may block passes `BlockingConnector` and drives the result with
///        `SyncRun`.
/// @param connectTimeout Ceiling on the dial, name resolution included.
///
/// There is no post-connect timeout here any more, and its absence is the point.
/// It used to be ONE value passed twice -- as the dial bound AND as the socket's
/// `SO_RCVTIMEO` -- which the header defended because "every caller up here wants
/// the same answer to both". Two things ended that. The dial grew a bounded name
/// lookup, so the two genuinely bound different things: the node's five-second
/// I/O ceiling was chosen per operation and was never meant as a
/// resolve-plus-connect budget. And a reactor socket has no `SO_RCVTIMEO` to set
/// -- its reads suspend rather than block, so the option is inert.
///
/// A caller that needs the transfer bounded arms a `DeadlineTimer` that closes
/// the socket, which is strictly more than the option offered: `SO_RCVTIMEO`
/// bounds a single call, so a peer dribbling one byte at a time could still take
/// forever. `RemoteUpstream` does exactly that.
/// @return The connected socket, or nullptr when the endpoint could not be
///         parsed or reached.
[[nodiscard]] Task<std::unique_ptr<ISocket>> DialEndpoint(IConnector* connector,
                                                          std::string_view hostPort,
                                                          std::chrono::milliseconds connectTimeout);

/// Dial and hand back a socket synchronously, for a thread that may block.
///
/// `BlockingConnector&` and NOT `IConnector&`, deliberately: this function's
/// soundness rests entirely on the connector resolving inline and never leaving
/// its task suspended, which is what `SyncRun` requires. A comment saying so is a
/// rule somebody breaks; the type is the rule, and the failure it prevents is a
/// `std::logic_error` thrown from inside a heartbeat thread.
///
/// @param connector A blocking connector, already carrying whatever socket-level
///        timeouts the caller wants.
/// @param hostPort `host:port`; a bare port is refused.
/// @param connectTimeout Ceiling on the dial, name resolution included.
/// @return The connected socket, or nullptr.
[[nodiscard]] std::unique_ptr<ISocket> DialEndpointBlocking(BlockingConnector& connector,
                                                            std::string_view hostPort,
                                                            std::chrono::milliseconds connectTimeout);

} // namespace FastCache::Cc

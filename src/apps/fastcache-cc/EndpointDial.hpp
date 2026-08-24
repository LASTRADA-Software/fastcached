// SPDX-License-Identifier: Apache-2.0
#pragma once

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
/// @param ioTimeout Dial deadline and per-call send/recv deadline; non-positive
///        leaves both unbounded.
/// @return The connected socket, or nullptr when the text is malformed or the
///         peer could not be reached. Callers treat both as "no cache" and fall
///         back, which is why they are not distinguished here.
[[nodiscard]] std::unique_ptr<ISocket> DialEndpoint(std::string_view hostPort, std::chrono::milliseconds ioTimeout);

} // namespace FastCache::Cc

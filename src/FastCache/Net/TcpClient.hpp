// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache
{

/// The client half of a TCP conversation: dial, then send and receive whole
/// messages.
///
/// This exists because the tree had grown **three** answers to it, and the one
/// nobody maintained is the one that broke. `fastcache-cc` carried a synchronous
/// `Cc::ITcpClient`; `compile-cache-testclient` carried a hand-written class that
/// was IPv4-literal-only, unbounded, unprotected against SIGPIPE, and did not
/// compile on POSIX at all (issue #84); and `Net/BlockingConnector` carried the
/// real one. Three implementations of one job drift, and the drift is invisible
/// until somebody builds the forgotten one.
///
/// ## Why the loops are coroutines
///
/// `ISocket::Read`/`Write` are awaitables, so a partial-transfer loop over them
/// is a coroutine or it is nothing. Writing them here rather than at each caller
/// is the whole point: "keep going until the buffer is done, and say whether it
/// finished" is one rule, and it had been written out separately in the launcher
/// and again in `RaftPeerTransport`.
///
/// A **synchronous** caller drives these with `SyncRun`, which is sound over a
/// blocking socket precisely because such a socket resolves every awaitable
/// inline and so never leaves the task suspended -- the thing `SyncRun` refuses
/// to read from. `RaftPeerTransport` already does this and documents it. Do NOT
/// drive them with `SyncRun` over a reactor socket; there the awaitable really
/// does suspend, and the coroutine has to be awaited by another coroutine.
///
/// ## What is deliberately NOT here
///
/// Splitting a `"host:port"` string. That grammar is `Core/HostPort`, and `Net`
/// is meant to be liftable out of this codebase on its own, so it does not reach
/// into `Core` for something a caller can do before calling. It also keeps the
/// one parser one parser: `rfind(':')` finds the wrong colon in `[::1]:7000`.

/// Dial `host:port` and hand back a socket that is bounded in both directions.
///
/// A convenience over `IConnector` for the common case, not a replacement for
/// it: it constructs the platform connector itself, so a caller that needs to
/// inject a different one should use `IConnector::Connect` directly.
///
/// @param host Hostname or literal address, IPv4 or IPv6 (unbracketed).
/// @param port TCP port in host byte order.
/// @param connectTimeout How long to wait for the dial; non-positive leaves the
///        platform default, which can run to minutes.
/// @param ioTimeout Per-call deadline for each later blocking send/recv;
///        non-positive leaves it unbounded.
/// @return The connected socket, or why the attempt did not succeed.
[[nodiscard]] std::expected<std::unique_ptr<ISocket>, NetError> ConnectTcp(std::string_view host,
                                                                           std::uint16_t port,
                                                                           std::chrono::milliseconds connectTimeout,
                                                                           std::chrono::milliseconds ioTimeout);

/// Write every byte of `bytes`, looping over partial writes.
///
/// A pointer rather than a reference because a coroutine parameter must not be a
/// reference: the frame outlives the call expression, so a bound reference can
/// dangle once the coroutine suspends.
///
/// @param socket The connected socket; must not be null.
/// @param bytes The payload to write. It must outlive the returned task, which
///        is the `IoAwaitable` contract and not an extra rule here.
/// @return True when every byte was written; false on any socket error.
[[nodiscard]] Task<bool> SendAll(ISocket* socket, std::span<std::byte const> bytes);

/// Read exactly `count` bytes, looping over partial reads.
///
/// @param socket The connected socket; must not be null.
/// @param count Number of bytes required. Zero yields an empty vector without
///        touching the socket, so a zero-length payload is not a peer that
///        closed.
/// @return The bytes, or nullopt if the peer closed or errored first.
[[nodiscard]] Task<std::optional<std::vector<std::byte>>> RecvExactly(ISocket* socket, std::size_t count);

} // namespace FastCache

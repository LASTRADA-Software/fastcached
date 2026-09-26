// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>

#include <core/async/AsTask.hpp>
#include <core/async/SyncRun.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>

namespace FastCache::Testing
{

/// Half-close @p socket from a test body that is not a coroutine, and report how it went.
///
/// core-cpp's `shutdownWrite` is an operation to await rather than a call that returns nothing
/// (#1596), because a TLS socket's half-close sends a record and can park on the write. The
/// sockets a test half-closes -- an in-memory pair, a blocking client -- answer inline, so
/// `syncRun` completes it; one that parked would make `syncRun` throw, which names the case rather
/// than hanging it.
/// @param socket The socket whose sending side ends.
/// @return Success, or why the socket could not half-close.
[[nodiscard]] inline std::expected<void, core::net::NetError> ShutdownWrite(core::net::ISocket& socket)
{
    return core::async::syncRun(core::async::asTask(socket.shutdownWrite()));
}

} // namespace FastCache::Testing

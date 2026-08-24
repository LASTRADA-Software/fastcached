// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/TcpClient.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

Task<SocketResult> ConnectTcp(std::string host,
                              std::uint16_t port,
                              std::chrono::milliseconds connectTimeout,
                              std::chrono::milliseconds ioTimeout)
{
    // The connector outlives the await because it is a local of THIS coroutine's
    // frame, not of the call expression -- which is the whole reason this is a
    // coroutine rather than a function returning the connector's task.
    BlockingConnector connector { DefaultAddressResolver(), BlockingConnectorOptions { .ioTimeout = ioTimeout } };
    co_return co_await connector.Connect(std::move(host), port, connectTimeout);
}

Task<bool> SendAll(ISocket* socket, std::span<std::byte const> bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
        // A peer that closed mid-transfer surfaces here as an error rather than as
        // a fatal signal, but only because the socket was armed when it was
        // constructed -- see Detail::ArmNoSigPipe.
        auto const wrote = co_await socket->Write(bytes.subspan(sent));
        if (!wrote.has_value() || *wrote == 0)
            co_return false;
        sent += *wrote;
    }
    co_return true;
}

Task<std::optional<std::vector<std::byte>>> RecvExactly(ISocket* socket, std::size_t count)
{
    // Asked for nothing, answered with nothing. Reading zero bytes from a socket
    // is indistinguishable from a peer that closed, so a caller draining a
    // zero-length payload must not be told the connection died.
    if (count == 0)
        co_return std::vector<std::byte> {};

    std::vector<std::byte> out(count);
    std::size_t got = 0;
    while (got < count)
    {
        auto const read = co_await socket->Read(std::span { out }.subspan(got));
        // Zero is EOF for a read, which here means the peer closed before it had
        // sent everything it declared -- a short frame, not a short read to retry.
        if (!read.has_value() || *read == 0)
            co_return std::nullopt;
        got += *read;
    }
    co_return std::move(out);
}

} // namespace FastCache

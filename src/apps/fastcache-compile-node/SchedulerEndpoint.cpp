// SPDX-License-Identifier: Apache-2.0
#include "SchedulerEndpoint.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <format>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Write every byte, or report failure.
    ///
    /// The byte count is checked, not merely the call: a short write leaves the client
    /// blocked on a length the reply promised and never delivered.
    /// @param socket Where to write.
    /// @param bytes What to write.
    /// @return Whether all of it went out.
    [[nodiscard]] Task<bool> WriteAll(ISocket* socket, std::span<std::byte const> bytes)
    {
        auto const written = co_await socket->Write(bytes);
        co_return written.has_value() && *written == bytes.size();
    }
} // namespace

SchedulerServer::SchedulerServer(IListener& listener,
                                 Distributed::SchedulerProtocol& protocol,
                                 Distributed::IMembershipOracle const& membership,
                                 ILogger& logger) noexcept:
    _listener { listener },
    _protocol { protocol },
    _membership { membership },
    _logger { logger }
{
}

void SchedulerServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

Task<void> SchedulerServer::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            // A poll timeout is how the loop wakes to observe Shutdown() on POSIX,
            // where Close() does not unblock a parked accept(). Not a failure.
            auto const code = accepted.error().code;
            if (code == NetErrorCode::WouldBlock || code == NetErrorCode::Timeout)
                continue;
            _logger.Logf(LogLevel::Debug, "scheduler: accept loop ended ({})", accepted.error().ToString());
            co_return;
        }

        auto socket = *std::move(accepted);

        // The peer's HOST, which is the vocabulary `ClusterMembership` speaks -- a
        // connection's source port is ephemeral and is not the peer's endpoint, so
        // there is nothing else here that could identify it.
        auto const peer = socket->PeerAddress();

        ByteReader reader { *socket, /*maxLineBytes*/ 1, MaxRequestBytes };
        auto const header = co_await reader.ReadExactly(Wire::RequestHeaderSize);
        if (header.has_value())
        {
            auto const decoded = Wire::DecodeRequestHeader(*header);
            if (!decoded.has_value())
            {
                // A foreign magic: the peer is not speaking this protocol, and with no
                // declared length there is nowhere to resynchronize to. Closing is the
                // only thing left, and is what an empty answer means here.
            }
            else if (decoded->payloadLength > MaxRequestBytes)
            {
                // Refused with a reply naming BOTH numbers, because "too large" without
                // the ceiling tells an operator nothing about a 64 KiB limit. The bytes
                // are never taken: the check is on the declared length, before the read.
                (void) co_await WriteAll(socket.get(),
                                         Wire::EncodeErrorReply(Wire::ErrorCode::PayloadTooLarge,
                                                                std::format("{} exceeds the scheduler's {}-byte request cap",
                                                                            decoded->payloadLength,
                                                                            MaxRequestBytes)));
            }
            else if (auto const payload = co_await reader.ReadExactly(decoded->payloadLength); payload.has_value())
            {
                std::vector<std::byte> frame { header->begin(), header->end() };
                frame.insert(frame.end(), payload->begin(), payload->end());

                auto const caller = Distributed::CallerContext { .membership = _membership.Classify(peer), .peerId = peer };
                if (auto const reply = _protocol.Answer(frame, caller); !reply.empty())
                    (void) co_await WriteAll(socket.get(), reply);
            }
        }

        socket->Close();
    }
    co_return;
}

SchedulerEndpoint::SchedulerEndpoint(std::unique_ptr<BlockingListener> listener,
                                     Distributed::SchedulerProtocol& protocol,
                                     Distributed::IMembershipOracle const& membership,
                                     std::string boundEndpoint,
                                     ILogger& logger):
    _listener { std::move(listener) },
    _server { std::make_unique<SchedulerServer>(*_listener, protocol, membership, logger) },
    _boundEndpoint { std::move(boundEndpoint) },
    _thread { [server = _server.get()] { SyncRun(server->Run()); } }
{
}

SchedulerEndpoint::~SchedulerEndpoint()
{
    // Order, not tidiness: closing the listener is what returns `Run()`, and the
    // jthread destructor that follows joins a loop which would otherwise still be
    // parked in `accept()`.
    _server->Shutdown();
}

std::expected<std::unique_ptr<SchedulerEndpoint>, std::string> SchedulerEndpoint::Start(
    std::string_view listenSpec,
    std::string_view defaultHost,
    Distributed::SchedulerProtocol& protocol,
    Distributed::IMembershipOracle const& membership,
    ILogger& logger)
{
    auto const endpoint = ParseEndpoint(listenSpec, defaultHost);
    if (!endpoint.has_value())
        return std::unexpected { std::format("'{}' is not [<address>:]<port>", listenSpec) };

    auto listener = BlockingListener::Bind(endpoint->first, endpoint->second);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot bind {}:{} ({})",
                                             endpoint->first,
                                             endpoint->second,
                                             listener ? listener->BindError() : std::string_view { "null listener" }) };

    // Applied here rather than left to the caller, for the reason
    // `AdoptInheritedListeners` was changed to take them as parameters: there must be
    // no way to obtain a listener this endpoint cannot stop.
    listener->SetTimeouts(SchedulerServer::AcceptPoll, SchedulerServer::RequestTimeout);

    // The port the listener ACTUALLY bound, not the one asked for. `0` means "pick a
    // free one", and an endpoint that echoed `:0` back could not tell an operator --
    // or a test -- where it ended up.
    auto bound = std::format("{}:{}", endpoint->first, listener->BoundPort());
    logger.Logf(LogLevel::Info, "scheduler listening on {}", bound);

    // `new` rather than `make_unique` because the constructor is private: the two ways
    // to reach it are this factory, which has already proved the listener is bound,
    // and nothing else.
    return std::unique_ptr<SchedulerEndpoint> { new SchedulerEndpoint {
        std::move(listener), protocol, membership, std::move(bound), logger } };
}

} // namespace FastCache::Node

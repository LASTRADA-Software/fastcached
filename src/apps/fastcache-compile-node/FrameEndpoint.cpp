// SPDX-License-Identifier: Apache-2.0
#include "FrameEndpoint.hpp"

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

FrameServer::FrameServer(IListener& listener, IFrameResponder& responder, std::string_view what, ILogger& logger) noexcept:
    _listener { listener },
    _responder { responder },
    _what { what },
    _logger { logger }
{
}

void FrameServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

Task<void> FrameServer::Run()
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
            _logger.Logf(LogLevel::Debug, "{}: accept loop ended ({})", _what, accepted.error().ToString());
            co_return;
        }

        auto socket = *std::move(accepted);

        // The peer's HOST. A connection's source port is ephemeral and is not the
        // peer's endpoint, so for a surface whose policy needs an identity -- the
        // scheduler's -- there is nothing else here that could supply one.
        auto const peer = socket->PeerAddress();

        auto const cap = _responder.MaxRequestBytes();
        ByteReader reader { *socket, /*maxLineBytes*/ 1, cap };
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
            else if (decoded->payloadLength > cap)
            {
                // Refused with a reply naming BOTH numbers, because "too large" without
                // the ceiling tells an operator nothing about a 64 KiB limit. The bytes
                // are never taken: the check is on the declared length, before the read.
                (void) co_await WriteAll(
                    socket.get(),
                    Wire::EncodeErrorReply(
                        Wire::ErrorCode::PayloadTooLarge,
                        std::format("{} exceeds the {} {}-byte request cap", decoded->payloadLength, _what, cap)));
            }
            else if (auto const payload = co_await reader.ReadExactly(decoded->payloadLength); payload.has_value())
            {
                std::vector<std::byte> frame { header->begin(), header->end() };
                frame.insert(frame.end(), payload->begin(), payload->end());

                if (auto const reply = _responder.Answer(frame, peer); !reply.empty())
                    (void) co_await WriteAll(socket.get(), reply);
            }
        }

        socket->Close();
    }
    co_return;
}

FrameEndpoint::FrameEndpoint(std::unique_ptr<BlockingListener> listener,
                             IFrameResponder& responder,
                             std::string_view what,
                             std::string boundEndpoint,
                             ILogger& logger):
    _listener { std::move(listener) },
    _server { std::make_unique<FrameServer>(*_listener, responder, what, logger) },
    _boundEndpoint { std::move(boundEndpoint) },
    _thread { [server = _server.get()] { SyncRun(server->Run()); } }
{
}

FrameEndpoint::~FrameEndpoint()
{
    // Order, not tidiness: closing the listener is what returns `Run()`, and the
    // jthread destructor that follows joins a loop which would otherwise still be
    // parked in `accept()`.
    _server->Shutdown();
}

std::expected<std::unique_ptr<FrameEndpoint>, std::string> FrameEndpoint::Start(std::string_view listenSpec,
                                                                                std::string_view defaultHost,
                                                                                IFrameResponder& responder,
                                                                                std::string_view what,
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
    listener->SetTimeouts(FrameServer::AcceptPoll, FrameServer::RequestTimeout);

    // The port the listener ACTUALLY bound, not the one asked for. `0` means "pick a
    // free one", and an endpoint that echoed `:0` back could not tell an operator --
    // or a test -- where it ended up.
    auto bound = std::format("{}:{}", endpoint->first, listener->BoundPort());
    logger.Logf(LogLevel::Info, "{} listening on {}", what, bound);

    // `new` rather than `make_unique` because the constructor is private: the two ways
    // to reach it are this factory, which has already proved the listener is bound,
    // and nothing else.
    return std::unique_ptr<FrameEndpoint> { new FrameEndpoint {
        std::move(listener), responder, what, std::move(bound), logger } };
}

} // namespace FastCache::Node

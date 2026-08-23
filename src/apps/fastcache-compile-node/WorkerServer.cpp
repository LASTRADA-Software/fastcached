// SPDX-License-Identifier: Apache-2.0
#include "WorkerServer.hpp"

#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Largest request a worker will buffer.
    ///
    /// A COMPILE carries a preprocessed translation unit, which for real C++ runs to
    /// several megabytes; 256 MiB is far above any of them and matches the daemon's
    /// own default value ceiling. It exists so a peer cannot declare a length this
    /// worker would try to allocate.
    constexpr std::size_t MaxRequestBytes = 256ULL * 1024ULL * 1024ULL;

    /// Write every byte, or report failure.
    [[nodiscard]] Task<bool> WriteAll(ISocket* socket, std::span<std::byte const> bytes)
    {
        auto const written = co_await socket->Write(bytes);
        // The byte count is checked, not merely the call: a short write leaves the
        // client blocked on a length the reply promised and never delivered.
        co_return written.has_value() && *written == bytes.size();
    }
} // namespace

WorkerServer::WorkerServer(
    IListener& listener, Cc::WorkerProtocol& protocol, std::size_t slots, IMetricsSink& metrics, ILogger& logger) noexcept:
    _listener { listener },
    _protocol { protocol },
    _slots { slots },
    _metrics { metrics },
    _logger { logger }
{
}

void WorkerServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

std::size_t WorkerServer::InFlight() const noexcept
{
    return _inFlight.load(std::memory_order_acquire);
}

Task<void> WorkerServer::Run()
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
            _logger.Logf(LogLevel::Debug, "worker: accept loop ended ({})", accepted.error().ToString());
            co_return;
        }

        auto socket = *std::move(accepted);

        // The cap is checked before the request is read, so an over-capacity client
        // is refused without this worker buffering its multi-megabyte payload
        // first. Refused, never queued: queueing hides the overload from the
        // scheduler that is trying to route around it, and the client has a local
        // compile waiting either way.
        auto const before = _inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (before >= _slots)
        {
            _inFlight.fetch_sub(1, std::memory_order_acq_rel);
            _metrics.Increment(IMetricsSink::Counter::WorkerJobsRefusedNoSlot);
            (void) co_await WriteAll(socket.get(), Wire::EncodeErrorReply(Wire::ErrorCode::NoCapacity, {}));
            socket->Close();
            continue;
        }

        // Served inline rather than detached. A compile is CPU-bound and seconds
        // long, so spawning it would only let more of them contend for the same
        // cores -- the opposite of what the slot cap exists to prevent. The accept
        // loop being busy IS the backpressure, and a waiting client is refused by
        // the cap above as soon as the loop comes back round.
        ByteReader reader { *socket, /*maxLineBytes*/ 1, MaxRequestBytes };
        auto const header = co_await reader.ReadExactly(Wire::RequestHeaderSize);
        if (header.has_value())
        {
            auto const decoded = Wire::DecodeRequestHeader(*header);
            if (decoded.has_value() && decoded->payloadLength <= MaxRequestBytes)
            {
                auto const payload = co_await reader.ReadExactly(decoded->payloadLength);
                if (payload.has_value())
                {
                    std::vector<std::byte> frame { header->begin(), header->end() };
                    frame.insert(frame.end(), payload->begin(), payload->end());

                    // Counted at the socket, which is what "bytes received" means
                    // to an operator sizing a link: the payload as it arrived, not
                    // what it decompressed to.
                    _metrics.Increment(IMetricsSink::Counter::WorkerBytesReceived, static_cast<std::uint64_t>(frame.size()));

                    if (auto const reply = _protocol.Answer(frame); reply.has_value())
                    {
                        _metrics.Increment(IMetricsSink::Counter::WorkerBytesReturned,
                                           static_cast<std::uint64_t>(reply->size()));
                        (void) co_await WriteAll(socket.get(), *reply);
                    }
                    // No reply means a foreign magic: the peer is not speaking this
                    // protocol, and there is no framing in which an answer would be
                    // meaningful. Closing is the only thing left.
                }
            }
            else if (decoded.has_value())
                (void) co_await WriteAll(socket.get(), Wire::EncodeErrorReply(Wire::ErrorCode::PayloadTooLarge, {}));
        }

        _inFlight.fetch_sub(1, std::memory_order_acq_rel);
        socket->Close();
    }
    co_return;
}

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/LingeringClose.hpp>
#include <FastCache/Net/NetError.hpp>
#include <FastCache/Net/SocketDeadline.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>

namespace FastCache
{

namespace
{
    /// Enough to take a typical refused request's remainder in one read.
    constexpr std::size_t DrainChunkBytes = 16 * 1024;
} // namespace

Task<LingerOutcome> CloseLingering(ISocket* socket, IReactor* reactor, LingerBounds bounds)
{
    // Closed or, as `ISocket::IsClosed` also allows, seen at EOF: nothing to listen to.
    // `Close()` is idempotent, and a transport that answers true at EOF still owns a handle.
    if (socket->IsClosed())
    {
        socket->Close();
        co_return LingerOutcome { .end = LingerEnd::AlreadyClosed, .reads = 0 };
    }

    socket->ShutdownWrite();

    // Past every read without the peer finishing, unless a read below says otherwise.
    auto outcome = LingerOutcome { .end = LingerEnd::ReadCap, .reads = 0 };
    SocketDeadlineTarget target { .socket = socket, .expired = false };
    {
        // A reactor socket: one deadline over the whole drain, which closes the socket and so
        // completes the parked read. A blocking one: each read carries its share instead.
        auto const deadline = ArmSocketDeadline(reactor, bounds.total, &target);
        if (bounds.total > std::chrono::milliseconds::zero() && bounds.reads > 0)
        {
            // At least a millisecond each, so a read count past the total's milliseconds
            // cannot divide it down to zero -- which `SetReceiveDeadline` reads as "leave the
            // current deadline alone".
            auto const shares = std::min<std::size_t>(bounds.reads, static_cast<std::size_t>(bounds.total.count()));
            socket->SetReceiveDeadline(bounds.total / static_cast<std::chrono::milliseconds::rep>(shares));
        }

        std::array<std::byte, DrainChunkBytes> discard {};
        auto discarded = std::size_t { 0 };
        for (auto const read: std::views::iota(std::size_t { 0 }, bounds.reads))
        {
            auto const got = co_await socket->Read(std::span<std::byte> { discard });
            outcome.reads = read + 1;
            if (got.has_value() && *got > 0)
            {
                discarded += *got;
                if (discarded < bounds.maxBytes)
                    continue;
                outcome.end = LingerEnd::ByteCap;
                break;
            }
            if (got.has_value())
                outcome.end = LingerEnd::PeerFinished;
            else if (target.expired || IsDeadlineExpiry(got.error().code))
                outcome.end = LingerEnd::Expired;
            else
                outcome.end = LingerEnd::PeerFailed;
            break;
        }
    }
    socket->Close();
    co_return outcome;
}

} // namespace FastCache

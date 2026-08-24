// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/ConnectFlow.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <cerrno>

    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>

    #include <netinet/in.h>
#endif

namespace FastCache
{

namespace
{

#if defined(_WIN32)
    using AddrLen = int;

    /// Whether the last error means "connect is under way", not "connect failed".
    /// @param code The error code from `::connect`.
    /// @return True when the attempt is still in progress.
    [[nodiscard]] bool ConnectInProgress(int code) noexcept
    {
        return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
    }

    /// Wait for a connecting socket to become writable, or for the deadline.
    /// @param s The connecting socket.
    /// @param timeout How long to wait; non-positive waits indefinitely.
    /// @return 1 when ready, 0 on timeout, negative on error.
    [[nodiscard]] int WaitWritable(Detail::NativeSocket s, std::chrono::milliseconds timeout) noexcept
    {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(static_cast<SOCKET>(s), &writable);
        fd_set failed = writable;

        timeval tv {};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

        auto const ready = ::select(0, nullptr, &writable, &failed, timeout.count() > 0 ? &tv : nullptr);
        if (ready <= 0)
            return ready;
        // Windows reports a refused connect through the exception set only, so a
        // socket that is only in `failed` is a failure rather than readiness.
        return FD_ISSET(static_cast<SOCKET>(s), &writable) ? 1 : -1;
    }
#else
    using AddrLen = socklen_t;

    [[nodiscard]] bool ConnectInProgress(int code) noexcept
    {
        return code == EINPROGRESS;
    }

    [[nodiscard]] int WaitWritable(Detail::NativeSocket s, std::chrono::milliseconds timeout) noexcept
    {
        pollfd entry {};
        entry.fd = static_cast<int>(s);
        entry.events = POLLOUT;

        // `poll` reports a refused connect as POLLOUT together with POLLERR, so
        // readiness alone does not mean success -- SO_ERROR below is what
        // decides. A negative timeout means "wait indefinitely" to poll(2),
        // which is the documented meaning of a non-positive argument here.
        auto const millis = timeout.count() > 0 ? static_cast<int>(timeout.count()) : -1;
        int ready = 0;
        do
        {
            ready = ::poll(&entry, 1, millis);
        } while (ready < 0 && errno == EINTR);
        return ready;
    }
#endif

    /// Dial one resolved endpoint.
    /// @param endpoint The candidate to try.
    /// @param timeout How long to allow.
    /// @return The connected native handle, or why not.
    [[nodiscard]] std::expected<Detail::NativeSocket, NetError> DialOne(ResolvedEndpoint const& endpoint,
                                                                        std::chrono::milliseconds timeout)
    {
        auto const native = static_cast<Detail::NativeSocket>(::socket(endpoint.family, SOCK_STREAM, endpoint.protocol));
        if (native == Detail::InvalidSocket)
            return std::unexpected { Detail::MakeNetError(Detail::LastNetworkError(), "socket() failed") };

        if (!Detail::SetNonBlocking(native))
        {
            auto const code = Detail::LastNetworkError();
            Detail::CloseNativeSocket(native);
            return std::unexpected { Detail::MakeNetError(code, "could not make the socket non-blocking") };
        }

        // The address bytes are opaque here; ResolvedEndpoint carries whatever the
        // resolver produced, which is what keeps this file free of family-specific
        // branching.
        auto const* const address = reinterpret_cast<sockaddr const*>(endpoint.storage.data());
        auto const result = ::connect(static_cast<
#if defined(_WIN32)
                                          SOCKET
#else
                                          int
#endif
                                          >(native),
                                      address,
                                      static_cast<AddrLen>(endpoint.length));

        if (result != 0)
        {
            auto const pending = Detail::LastNetworkError();
            if (!ConnectInProgress(pending))
            {
                Detail::CloseNativeSocket(native);
                return std::unexpected { Detail::MakeNetError(pending, "connect() failed") };
            }

            auto const ready = WaitWritable(native, timeout);
            if (ready == 0)
            {
                Detail::CloseNativeSocket(native);
                return std::unexpected { NetError { .code = NetErrorCode::Timeout,
                                                    .systemCode = 0,
                                                    .context =
                                                        std::format("connect timed out after {} ms", timeout.count()) } };
            }
            if (ready < 0)
            {
                auto const code = Detail::LastNetworkError();
                Detail::CloseNativeSocket(native);
                return std::unexpected { Detail::MakeNetError(code, "waiting for connect to complete failed") };
            }

            // Readiness is not success: a refused connect also makes the socket
            // ready. SO_ERROR is the only thing that distinguishes them, and
            // skipping it hands the caller a socket whose first write fails.
            int pendingError = 0;
            auto length = static_cast<AddrLen>(sizeof(pendingError));
            auto const probed = ::getsockopt(static_cast<
#if defined(_WIN32)
                                                 SOCKET
#else
                                                 int
#endif
                                                 >(native),
                                             SOL_SOCKET,
                                             SO_ERROR,
                                             reinterpret_cast<char*>(&pendingError),
                                             &length);
            if (probed != 0 || pendingError != 0)
            {
                auto const code = probed != 0 ? Detail::LastNetworkError() : pendingError;
                Detail::CloseNativeSocket(native);
                return std::unexpected { Detail::MakeNetError(code, "connect did not complete") };
            }
        }

        if (!Detail::SetBlocking(native))
        {
            auto const code = Detail::LastNetworkError();
            Detail::CloseNativeSocket(native);
            return std::unexpected { Detail::MakeNetError(code, "could not restore blocking mode") };
        }

        Detail::ApplyHotSocketOptions(native);
        return native;
    }

} // namespace

BlockingConnector::BlockingConnector(IAddressResolver& resolver, BlockingConnectorOptions options, IClock* clock) noexcept:
    _resolver { resolver },
    _options { options },
    _clock { clock != nullptr ? *clock : _ownClock }
{
}

namespace
{

    /// What one candidate attempt needs to know beyond the endpoint itself.
    struct BlockingDialState
    {
        IClock* clock { nullptr };
        std::chrono::milliseconds ioTimeout { 0 };
        std::string const* host { nullptr };
        std::uint16_t port { 0 };
    };

    /// `Detail::DialStep` over the blocking dial.
    ///
    /// A coroutine that never suspends, which is what keeps `SyncRun` sound over
    /// the whole flow. `deadline` is the TOTAL budget, so what this attempt gets
    /// is whatever is left of it -- that is what makes a two-candidate host cost
    /// one budget rather than two.
    Task<SocketResult> BlockingDial(void* state, ResolvedEndpoint endpoint, TimePoint deadline)
    {
        auto const& dial = *static_cast<BlockingDialState*>(state);

        auto remaining = std::chrono::milliseconds { 0 };
        if (dial.clock != nullptr && deadline != TimePoint::max())
        {
            auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - dial.clock->Now());
            // Never zero: DialOne reads a non-positive timeout as "wait forever",
            // so handing it an exhausted budget would remove the bound entirely at
            // exactly the moment it matters most.
            remaining = left > std::chrono::milliseconds { 0 } ? left : std::chrono::milliseconds { 1 };
        }

        auto dialed = DialOne(endpoint, remaining);
        if (!dialed.has_value())
            co_return std::unexpected(dialed.error());

        // Armed BEFORE the socket is handed over, so there is no window in which
        // it is reachable and unbounded.
        Detail::SetIoTimeouts(*dialed, dial.ioTimeout, dial.ioTimeout);
        co_return std::make_unique<BlockingSocket>(*dialed, std::format("{}:{}", *dial.host, dial.port));
    }

} // namespace

Task<SocketResult> BlockingConnector::Connect(std::string host, std::uint16_t port, std::chrono::milliseconds connectTimeout)
{
    Detail::EnsureNetworkInitialised();

    BlockingDialState state { .clock = &_clock, .ioTimeout = _options.ioTimeout, .host = &host, .port = port };

    // `reactor` is null: this connector is for threads that may block, and a null
    // reactor is what tells the shared flow and the inline resolver never to
    // suspend. That is the property `SyncRun` rests on.
    co_return co_await Detail::RunConnectFlow(
        &_resolver, /*reactor*/ nullptr, &_clock, std::move(host), port, connectTimeout, &BlockingDial, &state);
}

} // namespace FastCache

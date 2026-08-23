// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <chrono>
#include <cstring>
#include <format>
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

    /// Put a socket into non-blocking mode.
    /// @param s The socket.
    /// @return True on success.
    [[nodiscard]] bool SetNonBlocking(Detail::NativeSocket s) noexcept
    {
        u_long mode = 1;
        return ::ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) == 0;
    }

    /// Put a socket back into blocking mode, which is what `BlockingSocket`'s
    /// reads and writes expect.
    /// @param s The socket.
    /// @return True on success.
    [[nodiscard]] bool SetBlocking(Detail::NativeSocket s) noexcept
    {
        u_long mode = 0;
        return ::ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) == 0;
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

    [[nodiscard]] bool SetNonBlocking(Detail::NativeSocket s) noexcept
    {
        auto const flags = ::fcntl(static_cast<int>(s), F_GETFL, 0);
        return flags >= 0 && ::fcntl(static_cast<int>(s), F_SETFL, flags | O_NONBLOCK) == 0;
    }

    [[nodiscard]] bool SetBlocking(Detail::NativeSocket s) noexcept
    {
        auto const flags = ::fcntl(static_cast<int>(s), F_GETFL, 0);
        return flags >= 0 && ::fcntl(static_cast<int>(s), F_SETFL, flags & ~O_NONBLOCK) == 0;
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

        if (!SetNonBlocking(native))
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

        if (!SetBlocking(native))
        {
            auto const code = Detail::LastNetworkError();
            Detail::CloseNativeSocket(native);
            return std::unexpected { Detail::MakeNetError(code, "could not restore blocking mode") };
        }

        Detail::ApplyHotSocketOptions(native);
        return native;
    }

} // namespace

BlockingConnector::BlockingConnector(IAddressResolver& resolver) noexcept:
    _resolver { resolver }
{
}

std::expected<std::unique_ptr<ISocket>, NetError> BlockingConnector::Connect(std::string_view host,
                                                                             std::uint16_t port,
                                                                             std::chrono::milliseconds timeout)
{
    Detail::EnsureNetworkInitialised();

    auto const resolved = _resolver.Resolve(host, port);
    if (!resolved.has_value())
        return std::unexpected { NetError { .code = NetErrorCode::AddressNotAvail,
                                            .systemCode = 0,
                                            .context =
                                                std::format("could not resolve {}:{}: {}", host, port, resolved.error()) } };

    // Every candidate is tried in preference order, and the LAST failure is what
    // is reported. A host with both an AAAA and an A record on a machine with no
    // IPv6 route fails the first and succeeds the second, and a dial that gave up
    // after one would report that as the peer being down.
    auto failure = NetError { .code = NetErrorCode::AddressNotAvail,
                              .systemCode = 0,
                              .context = std::format("no usable address for {}:{}", host, port) };

    for (auto const& endpoint: *resolved)
    {
        auto dialed = DialOne(endpoint, timeout);
        if (dialed.has_value())
            return std::make_unique<BlockingSocket>(*dialed, std::format("{}:{}", host, port));
        failure = std::move(dialed.error());
    }

    return std::unexpected { std::move(failure) };
}

} // namespace FastCache

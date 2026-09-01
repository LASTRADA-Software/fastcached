// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpConnector.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/DeadlineTimer.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/ConnectFlow.hpp>
    #include <FastCache/Net/IocpSocket.hpp>

    #include <winsock2.h>

    #include <coroutine>
    #include <cstring>
    #include <memory>
    #include <utility>

    #include <mswsock.h>
    #include <ws2tcpip.h>

namespace FastCache
{

namespace
{

    /// One dial in flight.
    ///
    /// `completion` is FIRST because the reactor reinterpret_casts the
    /// `lpOverlapped` it dequeues back to an `IocpCompletion*` and then to the
    /// enclosing op -- the same layout contract `IocpSocket::Impl::Op` relies on.
    /// It lives in the dialling coroutine's frame, so its address is stable for
    /// exactly as long as the port can reach it.
    ///
    /// **That last sentence is the invariant, and it is why this type carries no
    /// `inFlight` reference the way `IocpSocket::Impl::Op` and
    /// `IocpListener::Impl::AcceptOp` do. Do not "make it consistent" with them.**
    ///
    /// The op outlives the operation because the OWNER IS SUSPENDED ON IT: the
    /// coroutine cannot leave the frame holding this struct without the
    /// completion having arrived, since `co_await ConnectPark { &op }` is resumed
    /// only by `OnConnectComplete`. Even the timeout path below cancels and then
    /// keeps waiting, precisely because `CancelIoEx` makes the completion arrive
    /// rather than retracting it.
    ///
    /// The socket and the listener cannot use this shape: they are owned by
    /// whoever holds their `unique_ptr`, and a destructor is not a coroutine and
    /// cannot suspend until a completion lands. That asymmetry is the whole of
    /// #465 -- removing this comment and adding a reference count here would be
    /// harmless, but removing the *waiting* on the belief that a reference count
    /// replaces it would reintroduce the defect in the one place that never had
    /// it.
    struct ConnectOp
    {
        IocpCompletion completion {};
        IocpReactor* reactor { nullptr };
        std::coroutine_handle<> waiter {};
        SOCKET socket { INVALID_SOCKET };
        DWORD error { 0 };
        bool settled { false };
        bool timedOut { false };
    };

    /// Suspends until the completion arrives.
    struct ConnectPark
    {
        ConnectOp* op { nullptr };

        [[nodiscard]] bool await_ready() const noexcept
        {
            return op->settled;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            if (op->settled)
                return false;
            op->waiter = handle;
            return true;
        }

        void await_resume() const noexcept {}
    };

    /// Reactor callback: the port has an answer for this dial.
    ///
    /// The completion is the SINGLE writer of the outcome, which is why the
    /// deadline below cancels the operation rather than settling the op itself.
    /// That removes the two-writer race the readiness path has to guard against.
    void OnConnectComplete(IocpCompletion* base, DWORD /*bytes*/, DWORD err)
    {
        auto* const op = reinterpret_cast<ConnectOp*>(base);
        if (op->settled)
            return;
        op->settled = true;

        // `overlapped.Internal` is an NTSTATUS and the reactor hands it over
        // as-is, so 0xC0000236 (connection refused) would fall through every
        // WSAE* row and land on SystemError -- useless to a connector whose whole
        // job is to tell refused from unreachable. `WSAGetOverlappedResult` is
        // the documented conversion.
        if (err != 0)
        {
            DWORD transferred = 0;
            DWORD flags = 0;
            if (WSAGetOverlappedResult(
                    op->socket, reinterpret_cast<LPWSAOVERLAPPED>(&op->completion), &transferred, FALSE, &flags)
                == FALSE)
                err = static_cast<DWORD>(WSAGetLastError());
        }
        op->error = err;

        if (auto waiter = std::exchange(op->waiter, {}); waiter)
            op->reactor->Submit(waiter);
    }

    /// The wildcard address for a family, which `ConnectEx` requires the socket
    /// to be bound to before it is called.
    ///
    /// A two-row table rather than an if, and the step has no precedent in this
    /// tree: `AcceptEx` needs no such bind. Omitting it makes `ConnectEx` fail
    /// with WSAEINVAL, which names nothing at all.
    [[nodiscard]] bool BindWildcard(SOCKET socket, int family) noexcept
    {
        if (family == AF_INET)
        {
            sockaddr_in any {};
            any.sin_family = AF_INET;
            any.sin_addr.s_addr = htonl(INADDR_ANY);
            return ::bind(socket, reinterpret_cast<sockaddr const*>(&any), sizeof(any)) == 0;
        }
        if (family == AF_INET6)
        {
            sockaddr_in6 any {};
            any.sin6_family = AF_INET6;
            any.sin6_addr = in6addr_any;
            return ::bind(socket, reinterpret_cast<sockaddr const*>(&any), sizeof(any)) == 0;
        }
        return false;
    }

} // namespace

/// State the dial step needs beyond the endpoint.
struct DialContext
{
    IocpReactor* reactor { nullptr };
    std::array<IocpConnector::ExtensionCache, 2>* cache { nullptr };
};

namespace
{

    /// Look up `ConnectEx` for a family, filling the cache on first use.
    /// @param cache The connector's two-row table.
    /// @param socket A socket of the family in question; WSAIoctl needs one.
    /// @param family AF_INET or AF_INET6.
    /// @return The function pointer, or nullptr when it cannot be obtained.
    [[nodiscard]] LPFN_CONNECTEX ConnectExFor(std::array<IocpConnector::ExtensionCache, 2>& cache,
                                              SOCKET socket,
                                              int family) noexcept
    {
        for (auto& row: cache)
            if (row.family == family && row.connectEx != nullptr)
                return reinterpret_cast<LPFN_CONNECTEX>(row.connectEx);

        GUID guid = WSAID_CONNECTEX;
        LPFN_CONNECTEX fn = nullptr;
        DWORD returned = 0;
        if (WSAIoctl(socket,
                     SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &guid,
                     sizeof(guid),
                     &fn,
                     sizeof(fn),
                     &returned,
                     nullptr,
                     nullptr)
            != 0)
            return nullptr;

        for (auto& row: cache)
            if (row.connectEx == nullptr)
            {
                row.family = family;
                row.connectEx = reinterpret_cast<void*>(fn);
                break;
            }
        return fn;
    }

} // namespace

namespace
{

    /// `Detail::DialStep` over ConnectEx.
    Task<SocketResult> Dial(void* state, ResolvedEndpoint endpoint, TimePoint deadline)
    {
        auto& context = *static_cast<DialContext*>(state);

        // WSA_FLAG_NO_HANDLE_INHERIT is the close-on-exec equivalent, and it is
        // here for the same reason: a process that dials and also spawns children
        // would otherwise hand each child an open peer connection.
        Detail::OwnedNativeSocket holder { static_cast<Detail::NativeSocket>(
            ::WSASocketW(endpoint.family,
                         SOCK_STREAM,
                         endpoint.protocol,
                         nullptr,
                         0,
                         WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)) };
        if (!holder.Valid())
            co_return std::unexpected(Detail::MakeNetError(WSAGetLastError(), "WSASocketW() failed"));

        auto const socket = static_cast<SOCKET>(holder.Get());

        auto* const connectEx = ConnectExFor(*context.cache, socket, endpoint.family);
        if (connectEx == nullptr)
            co_return std::unexpected(Detail::MakeNetError(WSAGetLastError(), "WSAIoctl(ConnectEx) failed"));

        if (!BindWildcard(socket, endpoint.family))
            co_return std::unexpected(Detail::MakeNetError(WSAGetLastError(), "ConnectEx requires a bound socket"));

        // Associated BEFORE the operation is issued, which ConnectEx requires --
        // and if that fails there is nothing to wait for, because no completion
        // will ever be dequeued. Abandoning here rather than awaiting is what
        // `IocpSocket::IsAttached` warns about, applied one step earlier.
        if (!context.reactor->AttachHandle(reinterpret_cast<void*>(static_cast<std::uintptr_t>(socket))))
            co_return std::unexpected(NetError { .code = NetErrorCode::SystemError,
                                                 .systemCode = 0,
                                                 .context = "could not associate the dialling socket with the port" });

        ConnectOp op;
        op.reactor = context.reactor;
        op.socket = socket;
        op.completion.dispatch = &OnConnectComplete;

        auto const* const address = reinterpret_cast<sockaddr const*>(endpoint.storage.data());
        auto const issued = connectEx(socket,
                                      address,
                                      static_cast<int>(endpoint.length),
                                      nullptr,
                                      0,
                                      nullptr,
                                      reinterpret_cast<LPOVERLAPPED>(&op.completion));

        // TRUE and WSA_IO_PENDING both mean parked: the port is not set to skip
        // completion on success, so a synchronous success still queues a packet.
        // `IocpListener::Accept` already treats them the same way.
        if (issued == FALSE)
        {
            auto const pending = WSAGetLastError();
            if (pending != WSA_IO_PENDING)
                co_return std::unexpected(Detail::MakeNetError(pending, "ConnectEx() failed"));
        }

        {
            // Cancels the operation and lets the completion report what happened,
            // rather than settling the op itself -- so the completion stays the
            // single writer and there is no race to guard. Strictly better than
            // the readiness path, which has two writers and needs a flag.
            DeadlineTimer const timer { *context.reactor,
                                        deadline,
                                        [](void* timedOut) {
                                            auto& pendingOp = *static_cast<ConnectOp*>(timedOut);
                                            if (pendingOp.settled)
                                                return;
                                            pendingOp.timedOut = true;
                                            // ERROR_NOT_FOUND here just means the completion is already
                                            // in flight, which is not a failure of anything.
                                            std::ignore =
                                                ::CancelIoEx(reinterpret_cast<HANDLE>(pendingOp.socket),
                                                             reinterpret_cast<LPOVERLAPPED>(&pendingOp.completion));
                                        },
                                        &op };

            co_await ConnectPark { .op = &op };
        }

        if (op.error != 0)
        {
            if (op.timedOut)
                co_return std::unexpected(NetError { .code = NetErrorCode::Timeout,
                                                     .systemCode = static_cast<int>(op.error),
                                                     .context = "connect timed out" });
            co_return std::unexpected(Detail::MakeNetError(static_cast<int>(op.error), "ConnectEx did not complete"));
        }

        // Without this the socket is genuinely connected and yet getpeername,
        // shutdown and the ordinary calls all fail on it: ConnectEx leaves the
        // handle's context unset until it is asked for.
        if (::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
            co_return std::unexpected(Detail::MakeNetError(WSAGetLastError(), "SO_UPDATE_CONNECT_CONTEXT failed"));

        Detail::ApplyHotSocketOptions(holder.Get());
        auto peer = FormatPeerAddress(endpoint);

        co_return std::make_unique<IocpSocket>(*context.reactor,
                                               static_cast<std::uintptr_t>(holder.Release()),
                                               std::move(peer),
                                               IocpAttachment::AlreadyAttached);
    }

} // namespace

IocpConnector::IocpConnector(IocpReactor& reactor, IAsyncAddressResolver& resolver, IClock& clock) noexcept:
    _reactor { reactor },
    _resolver { resolver },
    _clock { clock }
{
}

Task<SocketResult> IocpConnector::Connect(std::string host, std::uint16_t port, std::chrono::milliseconds connectTimeout)
{
    Detail::EnsureNetworkInitialised();
    DialContext context { .reactor = &_reactor, .cache = &_connectEx };
    co_return co_await Detail::RunConnectFlow(
        &_resolver, &_reactor, &_clock, std::move(host), port, connectTimeout, &Dial, &context);
}

} // namespace FastCache

#endif // _WIN32

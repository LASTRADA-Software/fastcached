// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpConnector.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/DeadlineTimer.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/ConnectFlow.hpp>
    #include <FastCache/Net/IocpDial.hpp>
    #include <FastCache/Net/IocpSocket.hpp>
    #include <FastCache/Net/IocpStatus.hpp>
    #include <FastCache/Net/KeepAlive.hpp>

    #include <winsock2.h>

    #include <coroutine>
    #include <cstring>
    #include <memory>
    #include <tuple>
    #include <utility>

    #include <mswsock.h>
    #include <ws2tcpip.h>

namespace FastCache
{

namespace
{

    /// The dial op this connector instantiates.
    ///
    /// `ConnectOp`, `ConnectPark` and the hand-back they exist for used to live in
    /// this anonymous namespace, which is why nothing outside this translation unit
    /// could reach the `Submit` site
    /// ([#1138](https://github.com/LASTRADA-Software/fastcached/issues/1138)). They
    /// are in `Net/IocpDial.hpp` now, the way `ReactorDial.hpp` already holds the
    /// readiness half, and this alias is the one instantiation production uses.
    using IocpConnectOp = Detail::ConnectOp<IocpReactor>;

    /// Reactor callback: the port has an answer for this dial.
    ///
    /// The completion is the SINGLE writer of the outcome, which is why the
    /// deadline below cancels the operation rather than settling the op itself.
    /// That removes the two-writer race the readiness path has to guard against.
    ///
    /// It decides only WHAT happened; `Detail::SettleConnect` does the hand-back.
    /// The split is what makes the hand-back reachable from a test at all, and it
    /// mirrors the readiness path, where the platform callbacks likewise only
    /// decide an outcome and `SettleDial` posts the chain.
    /// @param base The completion the port dequeued, which is this op's first member.
    /// @param status What the port reported.
    void OnConnectComplete(IocpCompletion* base, DWORD /*bytes*/, IocpStatus status)
    {
        auto* const op = reinterpret_cast<IocpConnectOp*>(base);

        // Asked HERE as well as inside `SettleConnect`, and the duplication is
        // deliberate rather than a leftover: the conversion below is an ARGUMENT, so
        // it would run on an already-settled op before the settle could refuse it --
        // and it reads a SOCKET a settled op may no longer own.
        if (op->settled)
            return;

        // The NTSTATUS the reactor read is not a WSA code: 0xC0000236 (connection
        // refused) matches no `WSAE*` row and lands on `SystemError`, which is useless
        // to a connector whose whole job is to tell refused from unreachable. This
        // function used to carry that observation and its own private conversion --
        // correct, and invisible to every other consumer of `IocpCompletion::dispatch`,
        // one of which is `IocpSocket` and had the same defect for as long. The
        // conversion now lives in one place and the `dispatch` signature no longer
        // offers a `DWORD` to forget to convert.
        Detail::SettleConnect(*op, Detail::WsaErrorOf(op->socket, op->completion, status));
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
                     // Explicit: `&fn` is a pointer to a FUNCTION pointer, and letting
                     // that reach `LPVOID` implicitly is a multilevel conversion the
                     // analyser refuses -- rightly, since the arity is easy to get
                     // wrong here and WSAIoctl cannot check it.
                     static_cast<void*>(&fn),
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
    Task<SocketResult> Dial(void* state, ResolvedEndpoint endpoint, TimePoint deadline, KeepAlive keepAlive)
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

        IocpConnectOp op;
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
                                            auto& pendingOp = *static_cast<IocpConnectOp*>(timedOut);
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

            co_await Detail::ConnectPark<IocpConnectOp> { .op = &op };
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

        // AFTER the hot options and separate from them, which is the point: that
        // function is what every socket this process owns passes through, and this
        // is asked for by ONE dial. Best-effort by contract -- see `ArmKeepAlive` --
        // so a socket that would not take it is still handed over rather than
        // failing a build over a tuning option.
        if (keepAlive == KeepAlive::Yes)
            std::ignore = Detail::ArmKeepAlive(holder.Get(), KeepAliveSettings {});

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

Task<SocketResult> IocpConnector::Connect(std::string host, std::uint16_t port, DialOptions options)
{
    Detail::EnsureNetworkInitialised();
    DialContext context { .reactor = &_reactor, .cache = &_connectEx };
    co_return co_await Detail::RunConnectFlow(
        &_resolver, &_reactor, &_clock, std::move(host), port, options, &Dial, &context);
}

} // namespace FastCache

#endif // _WIN32

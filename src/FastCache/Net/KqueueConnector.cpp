// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/KqueueConnector.hpp>

#if defined(__APPLE__)

    #include <FastCache/Net/ConnectFlow.hpp>
    #include <FastCache/Net/KqueueSocket.hpp>
    #include <FastCache/Net/ReactorDial.hpp>

    #include <sys/socket.h>

    #include <cerrno>
    #include <utility>

namespace FastCache
{

namespace
{

    /// The platform triple `Detail::DialReadiness` is written against.
    ///
    /// Three aliases and one function: everything else about the dial is shared
    /// with kqueue, whose traits differ only in these names.
    struct KqueueDialTraits
    {
        using Reactor = KqueueReactor;
        using Handler = KqueueFdHandler;
        using Socket = KqueueSocket;

        /// Reactor callback for all three of readable, writable and error.
        ///
        /// One routine for all three because the question a dial asks is "has the
        /// connect settled", and every one of those answers it. Which of them the
        /// kernel picks varies -- a refused connect can arrive as an error with
        /// neither direction set -- so distinguishing them here would only be a
        /// way to miss one.
        static void Settle(KqueueFdHandler* self)
        {
            Detail::SettleDial(*static_cast<Detail::ReadinessDialOp<KqueueDialTraits>*>(self->owner),
                               std::expected<void, NetError> {});
        }
    };

    /// `Detail::DialStep` over the epoll dial.
    Task<SocketResult> Dial(void* state, ResolvedEndpoint endpoint, TimePoint deadline)
    {
        co_return co_await Detail::DialReadiness<KqueueDialTraits>(static_cast<KqueueReactor*>(state), endpoint, deadline);
    }

} // namespace

KqueueConnector::KqueueConnector(KqueueReactor& reactor, IAsyncAddressResolver& resolver, IClock& clock) noexcept:
    _reactor { reactor },
    _resolver { resolver },
    _clock { clock }
{
}

Task<SocketResult> KqueueConnector::Connect(std::string host, std::uint16_t port, std::chrono::milliseconds connectTimeout)
{
    Detail::EnsureNetworkInitialised();
    co_return co_await Detail::RunConnectFlow(
        &_resolver, &_reactor, &_clock, std::move(host), port, connectTimeout, &Dial, &_reactor);
}

} // namespace FastCache

#endif // __APPLE__

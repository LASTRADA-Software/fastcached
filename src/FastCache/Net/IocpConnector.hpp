// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Net/IAsyncAddressResolver.hpp>
    #include <FastCache/Net/IConnector.hpp>

    #include <array>
    #include <chrono>
    #include <cstdint>
    #include <string>

namespace FastCache
{

/// `IConnector` that never blocks the calling thread, over IOCP.
///
/// The Windows counterpart to `EpollConnector`, and it does NOT share their
/// implementation: epoll and kqueue are readiness models, so a dial there arms
/// write interest and waits to be told. IOCP is a completion model, so the dial
/// is issued through `ConnectEx` and the port delivers the answer. The shared
/// half -- resolve, budget, try every candidate, report the last failure -- is
/// `Detail::RunConnectFlow`, which both sides use.
///
/// One property this gets for free that the readiness path has to work for: the
/// completion is the single writer of the outcome, so a deadline firing
/// concurrently cannot race it. The timer here cancels the operation and lets
/// the completion report what happened, rather than settling the dial itself.
class IocpConnector final: public IConnector
{
  public:
    /// @param reactor Reactor the returned sockets are pinned to.
    /// @param resolver Name-resolution seam. Must outlive this connector.
    /// @param clock Source for the total-call budget.
    IocpConnector(IocpReactor& reactor, IAsyncAddressResolver& resolver, IClock& clock) noexcept;

    /// @copydoc IConnector::Connect
    [[nodiscard]] Task<SocketResult> Connect(std::string host,
                                             std::uint16_t port,
                                             std::chrono::milliseconds connectTimeout) override;

    /// The `ConnectEx` pointer for one address family.
    ///
    /// Cached per family rather than once, because the pointer is a property of
    /// the transport provider and not of the process -- and unlike a listener,
    /// which has exactly one family, a connector dials whichever the resolver
    /// hands it. Two rows, filled lazily on first use of each.
    struct ExtensionCache
    {
        int family { 0 };
        void* connectEx { nullptr };
    };

  private:
    IocpReactor& _reactor;
    IAsyncAddressResolver& _resolver;
    IClock& _clock;
    std::array<ExtensionCache, 2> _connectEx {};
};

} // namespace FastCache

#endif // _WIN32

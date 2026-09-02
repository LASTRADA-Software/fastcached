// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(__linux__)

    #include <FastCache/Async/EpollReactor.hpp>
    #include <FastCache/Net/IAsyncAddressResolver.hpp>
    #include <FastCache/Net/IConnector.hpp>

    #include <chrono>
    #include <cstdint>
    #include <string>

namespace FastCache
{

/// `IConnector` that never blocks the calling thread.
///
/// The counterpart to `EpollListener`: the connect is issued non-blocking and the
/// coroutine suspends on write readiness, so a reactor thread carrying thousands
/// of other connections keeps serving them while this dial is outstanding. Name
/// resolution goes through the injected async resolver for the same reason --
/// `getaddrinfo` is the one genuinely unbounded step and it needs a thread of its
/// own, not this one.
///
/// The socket it produces is an `EpollSocket` pinned to the reactor given here,
/// which is why the reactor is a constructor parameter rather than a per-call
/// one: a socket wired to a loop other than the one its caller runs on is a data
/// race with no symptom until load.
class EpollConnector final: public IConnector
{
  public:
    /// @param reactor Reactor the returned sockets are pinned to.
    /// @param resolver Name-resolution seam. Must outlive this connector.
    /// @param clock Source for the total-call budget.
    EpollConnector(EpollReactor& reactor, IAsyncAddressResolver& resolver, IClock& clock) noexcept;

    /// @copydoc IConnector::Connect
    [[nodiscard]] Task<SocketResult> Connect(std::string host, std::uint16_t port, DialOptions options) override;

  private:
    EpollReactor& _reactor;
    IAsyncAddressResolver& _resolver;
    IClock& _clock;
};

} // namespace FastCache

#endif // __linux__

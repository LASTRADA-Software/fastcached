// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/IAsyncAddressResolver.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace FastCache
{

/// How long a `BlockingConnector` leaves its sockets unbounded for.
///
/// At namespace scope rather than nested, so a defaulted constructor parameter
/// can name it -- a nested type whose own default member initializers are not yet
/// complete is rejected outright.
struct BlockingConnectorOptions
{
    /// Per-call deadline armed on the returned socket, as `SO_RCVTIMEO` and
    /// `SO_SNDTIMEO`; non-positive leaves it unbounded.
    ///
    /// This lives here rather than on `IConnector::Connect` because it is a
    /// property only a *blocking* socket has. A reactor socket's reads suspend
    /// rather than block, so the option is inert there, and an interface carrying
    /// a parameter only one implementation can honour hands every other caller a
    /// bound that does not exist.
    ///
    /// Note what it does and does not bound: it is per **call**, so a peer that
    /// dribbles bytes slower than the timeout can still take arbitrarily long. It
    /// bounds the failure that matters, which is a peer that stops entirely.
    std::chrono::milliseconds ioTimeout { 0 };
};

/// `IConnector` over the platform socket API, for threads that may block.
///
/// The dial is issued **non-blocking and then waited on** with `poll`/`select`,
/// rather than as a plain blocking `connect()`. That is what makes the timeout
/// mean anything: a blocking connect to a black-holed address is governed by the
/// kernel's own retry schedule, measured in minutes, and no caller can shorten
/// it. The socket is handed back in blocking mode, which is what
/// `BlockingSocket`'s reads and writes expect.
///
/// ## It is a coroutine that never suspends, and that is the point
///
/// `Connect` returns a `Task` because the interface does, but nothing in this
/// implementation ever suspends: the resolver is inline and the wait is a
/// syscall on the calling thread. So the task is never left suspended, which is
/// precisely the precondition `SyncRun` states -- the same argument
/// `Net/TcpClient.hpp` makes for `SendAll` and `RecvExactly` over a blocking
/// socket.
///
/// That is what this class is *for*. Some threads may legitimately block: the
/// compile node's heartbeat thread, a one-shot CLI, an accept loop that serves
/// its request inline. Those callers keep this connector and drive it with
/// `SyncRun`; a caller on a reactor takes `PlatformConnector` instead. Helpers
/// meant only for the first group take a `BlockingConnector&` rather than an
/// `IConnector&`, so the precondition is enforced by the type rather than by a
/// comment somebody will not read.
class BlockingConnector final: public IConnector
{
  public:
    /// @param resolver Name resolution seam; defaults to the process-wide
    ///        getaddrinfo-backed one. Injected so a test can dial a scripted
    ///        endpoint without a DNS lookup.
    /// @param options Socket-level timeouts armed before the socket is handed
    ///        over -- before, so there is no window in which it is reachable and
    ///        unbounded.
    /// @param clock Source for the total-call budget, or nullptr to use a
    ///        `SteadyClock` of this connector's own. Injected so the budget is a
    ///        `ManualClock` unit test rather than a sleep -- the same
    ///        nullable-clock shape `RunReactorServer` already takes.
    explicit BlockingConnector(IAddressResolver& resolver = DefaultAddressResolver(),
                               BlockingConnectorOptions options = {},
                               IClock* clock = nullptr) noexcept;

    /// @copydoc IConnector::Connect
    [[nodiscard]] Task<SocketResult> Connect(std::string host,
                                             std::uint16_t port,
                                             std::chrono::milliseconds connectTimeout) override;

  private:
    /// Declared before `_clock`, which may bind to it. Order is load-bearing and
    /// the compiler checks it here, which is why it is a member rather than a
    /// local somebody has to sequence by hand.
    SteadyClock _ownClock;

    InlineAddressResolver _resolver;
    BlockingConnectorOptions _options;
    IClock& _clock;
};

} // namespace FastCache

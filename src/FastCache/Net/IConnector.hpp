// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace FastCache
{

/// How a component opens an **outbound** connection.
///
/// The counterpart to `IListener`, and the seam this codebase was missing: every
/// server-side path reaches the network through `IListener`/`ISocket`, while the
/// one place that dialled out -- `HttpHealthProbe` -- did it through a free
/// function with the socket API inlined, which is exactly the shape the
/// dependency-injection rule exists to prevent.
///
/// ## Why this used to be synchronous, and why that argument was wrong
///
/// `Connect` blocked the calling thread, and this header argued at length that
/// it should: the caller has nothing to do until the connection exists, so a
/// coroutine "would buy the ability to interleave work that does not exist". The
/// rule it leaned on was that **a reactor thread never calls this**. Three things
/// were wrong with that, and each has already cost something:
///
/// - **The caller has nothing to do; the *thread* has thousands of other
///   connections.** The argument reasoned about one caller's own work and quietly
///   ignored whose thread it was spending. That is the same mistake, in the same
///   direction, that `Net/PlatformListener.hpp` records for the accept side.
/// - **The rule was not free -- it was paid for.** `RaftPeerTransport` owns a
///   thread per peer because of this interface, and that is the compromise rather
///   than the design. Worse, it could not be made safe: its `Stop()` joins a
///   sender that may be parked in `::send` with no send timeout, which nothing
///   can wake, so a node with a peer that accepted and stopped reading hangs at
///   shutdown until the supervisor escalates to SIGKILL.
/// - **It made the reactor sockets unreachable outbound.** Nothing in this tree
///   could obtain an `EpollSocket`/`KqueueSocket`/`IocpSocket` for a connection it
///   opened, so every outbound conversation ran over a `BlockingSocket` whose only
///   bound was `SO_RCVTIMEO`.
///
/// And the rule did not even cover the worst case. `connectTimeout` bounds the
/// dial, but **name resolution runs first and is bounded by nothing** --
/// `getaddrinfo` takes no timeout. A wedged resolver therefore parks the caller
/// for as long as the platform's resolver library feels like, which for
/// `fastcache-cc` is every translation unit in the build.
///
/// So `Connect` is a coroutine. A reactor thread may call it. A thread that may
/// block may also call it, because `BlockingConnector` resolves every awaitable
/// inline and so leaves its task never suspended -- which is exactly what
/// `SyncRun` requires, the same argument `Net/TcpClient.hpp` makes for `SendAll`
/// and `RecvExactly`. Do **not** drive a reactor connector with `SyncRun`.
///
/// ## The timeout is a parameter, not a policy
///
/// Per call rather than fixed at construction, and load-bearing rather than a
/// nicety: the OS default connect timeout runs to minutes on some systems, so a
/// caller that wants to notice a dead peer and retry -- or simply to shut down --
/// cannot be made to wait for it. The same lesson `BlockingListener::SetTimeouts`
/// records for the accept side.
///
/// ## What is deliberately NOT here
///
/// **A per-call reactor.** The socket handed back is pinned to one reactor, so
/// which reactor is a property of the *connector*, chosen where it is
/// constructed. Passing one per call would let a caller obtain a socket wired to
/// a loop other than the one its coroutine runs on -- a data race with no symptom
/// until load.
///
/// **A cancellation token.** `Async/Cancellation` is poll-only and cannot wake a
/// parked dial; the deadline can, and does. A caller shutting down waits at most
/// `connectTimeout`, which is the same answer the accept side gives.
///
/// **A post-connect I/O timeout.** It used to take one, and it was
/// `SO_RCVTIMEO`/`SO_SNDTIMEO` -- which bounds a *blocking* syscall and means
/// nothing to a socket whose reads suspend. Keeping it would hand a reactor
/// caller a bound that does not exist, which is worse than having none. It
/// survives as `BlockingConnector::Options::ioTimeout`, construction-time
/// configuration where it belongs; a reactor caller bounds the transfer it
/// actually cares about with `Async/DeadlineTimer`, which is strictly more than
/// the socket option offered -- that one bounds a single call, so a peer
/// dribbling a byte at a time could still take forever.
class IConnector
{
  public:
    IConnector() = default;
    IConnector(IConnector const&) = delete;
    IConnector(IConnector&&) = delete;
    IConnector& operator=(IConnector const&) = delete;
    IConnector& operator=(IConnector&&) = delete;
    virtual ~IConnector() = default;

    /// Open a connection to `host:port`.
    ///
    /// The returned task is **lazy**: nothing is resolved and no descriptor is
    /// created until it is awaited, so discarding it unawaited costs nothing.
    /// Once awaited it must be awaited to completion -- destroying a suspended
    /// task frees a frame the reactor still points into, the same contract
    /// `IoAwaitable` states for its buffers.
    ///
    /// @param host Hostname or literal address, IPv4 or IPv6, **unbracketed**.
    ///        Taken by value: this is a coroutine, its frame outlives the call
    ///        expression, and a `string_view` parameter would name storage the
    ///        caller is entitled to destroy before the first suspend -- the hazard
    ///        `Net/TcpClient.hpp` records for reference parameters, reached by
    ///        another route. The value is also what a threaded resolver hands to
    ///        a worker, so the copy is one the implementation needed anyway.
    /// @param port TCP port in host byte order.
    /// @param connectTimeout How long to allow for the **whole call**, name
    ///        resolution and every candidate address included. A non-positive
    ///        value leaves the platform default in place, which may be minutes.
    ///        A total and not a per-candidate budget: a host with both an AAAA
    ///        and an A record used to be able to take twice what the caller
    ///        asked for, which is a bound that is not one.
    /// @return The connected socket, or why the attempt did not succeed.
    [[nodiscard]] virtual Task<SocketResult> Connect(std::string host,
                                                     std::uint16_t port,
                                                     std::chrono::milliseconds connectTimeout) = 0;
};

} // namespace FastCache

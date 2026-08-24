// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IAsyncAddressResolver.hpp>

#include <cstddef>
#include <memory>

namespace FastCache
{

/// Name resolution moved off the calling thread, onto a fixed pool.
///
/// `getaddrinfo` has no portable asynchronous form and takes no timeout, so a
/// thread is unavoidable: the choice is only whose. This puts it on a small pool
/// of its own so that a reactor thread -- which is carrying every other
/// connection on that loop -- never waits for a resolver.
///
/// It **decorates** the existing blocking `IAddressResolver` rather than calling
/// `getaddrinfo` itself, which keeps `SocketAddress.cpp` the single place that
/// issues the syscall and lets every rule below be tested against a scripted
/// inner with no DNS anywhere.
///
/// ## A literal never reaches the pool
///
/// Load-bearing rather than an optimisation. Every internal dial in this
/// codebase is to a literal -- Raft peers, the launcher's `127.0.0.1:6674`, an
/// endpoint discovery proved -- and the launcher makes one per translation unit,
/// thousands of times per build. Paying a thread hand-off and two context
/// switches for `inet_pton` would be a real regression on the hot path. It is
/// also what lets the whole connect path be exercised without a thread existing.
///
/// ## The queue is bounded, and a full queue is refused rather than waited on
///
/// An unbounded queue is a memory-exhaustion hole reachable by whatever provokes
/// dials, which is the same shape as the pre-auth payload cap and the
/// per-node pending-challenge table. And blocking the caller to wait for room
/// would reintroduce, on the reactor thread, precisely the stall this class
/// exists to remove -- so over-depth returns `WouldBlock` immediately.
/// `WouldBlock` and not `SystemError` because a caller can retry the first and
/// can do nothing at all with the second.
/// How many resolver threads, and how much work may wait for them.
///
/// At namespace scope rather than nested in `ThreadedAddressResolver`, and that
/// is not filing: a defaulted parameter of a nested type whose own default member
/// initializers are not yet complete is rejected outright -- "default member
/// initializer for 'threads' needed within definition of enclosing class". The
/// same reason `AsyncQueueOptions` sits beside its class.
struct ThreadedResolverOptions
{
    /// Fixed pool size. Never one thread per dial.
    ///
    /// Two rather than one, because a single 5-second SERVFAIL would otherwise
    /// head-of-line-block every other dial behind it. Two rather than
    /// `hardware_concurrency`, because this work is I/O-bound and a pool sized to
    /// cores puts thirty idle threads in a daemon that dials three peers.
    std::size_t threads { 2 };

    /// Largest number of lookups that may be waiting for a thread.
    std::size_t maxQueueDepth { 256 };
};

class ThreadedAddressResolver final: public IAsyncAddressResolver
{
  public:
    /// @param inner The blocking resolver to delegate to. Must outlive this.
    /// @param options Pool size and queue bound.
    explicit ThreadedAddressResolver(IAddressResolver& inner = DefaultAddressResolver(),
                                     ThreadedResolverOptions options = {});

    ThreadedAddressResolver(ThreadedAddressResolver const&) = delete;
    ThreadedAddressResolver(ThreadedAddressResolver&&) = delete;
    ThreadedAddressResolver& operator=(ThreadedAddressResolver const&) = delete;
    ThreadedAddressResolver& operator=(ThreadedAddressResolver&&) = delete;

    /// Stops and joins. See `Stop()` for what happens to work in flight.
    ~ThreadedAddressResolver() override;

    /// @copydoc IAsyncAddressResolver::Resolve
    [[nodiscard]] Task<ResolveResult> Resolve(std::string host, std::uint16_t port, IReactor* reactor) override;

    /// Refuse new work, fail everything queued, and wake the threads.
    ///
    /// Idempotent. A queued lookup is resumed with `Cancelled`, so no coroutine
    /// is stranded. A lookup already **inside** `getaddrinfo` cannot be
    /// interrupted -- there is no portable way -- so the join waits for it; it
    /// writes into state the abandoned task still holds a reference to, and
    /// nobody reads the answer.
    ///
    /// **Stop this before the reactors it hands results back to.** `Submit` on a
    /// reactor whose `Run` has already returned queues a handle nobody will ever
    /// resume, which is a leaked coroutine frame.
    void Stop() noexcept;

    /// @return How many lookups have been refused for want of queue room. For
    ///         tests and diagnostics; an operator seeing this move should raise
    ///         `maxQueueDepth` or find out what is provoking the dials.
    [[nodiscard]] std::size_t Refused() const noexcept;

    /// @return How many lookups this resolver has handed to a thread. Zero for a
    ///         process that only ever dials literals, which is the property the
    ///         fast path exists to give and the one a test asserts.
    [[nodiscard]] std::size_t Offloaded() const noexcept;

    /// Implementation detail; public so the .cpp's worker can name it.
    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
};

} // namespace FastCache

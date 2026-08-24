// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    /// One lookup's rendezvous between a worker thread and the parked coroutine.
    ///
    /// Shared, because either side may reach it last: the worker must be able to
    /// publish into it after the task was abandoned, and the task must be able to
    /// read it after the worker is gone.
    struct ResolveSlot
    {
        std::mutex mutex;
        ResolveResult result { std::vector<ResolvedEndpoint> {} };
        std::coroutine_handle<> waiter {};
        bool done { false };
    };

    /// Publish a result and hand the waiter back to its reactor.
    ///
    /// It never calls `resume()`. The consumer must continue on the reactor's
    /// thread and not on whichever worker happened to finish the lookup -- that
    /// is the entire point of the hand-back, and resuming here would run the
    /// caller's continuation on a resolver thread.
    void Settle(ResolveSlot& slot, IReactor* reactor, ResolveResult result)
    {
        std::coroutine_handle<> waiter {};
        {
            std::scoped_lock const guard { slot.mutex };
            slot.result = std::move(result);
            slot.done = true;
            waiter = std::exchange(slot.waiter, {});
        }
        if (waiter && reactor != nullptr)
            reactor->Submit(waiter);
    }

    /// Suspends until a slot is filled.
    ///
    /// Race-free the way `IoAwaitable` is: the completion check and the waiter
    /// registration happen under one lock, so a result landing between them
    /// resumes through the normal path rather than parking on an answer that has
    /// already arrived.
    struct SlotPark
    {
        std::shared_ptr<ResolveSlot> slot;

        [[nodiscard]] bool await_ready() const noexcept
        {
            std::scoped_lock const guard { slot->mutex };
            return slot->done;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            std::scoped_lock const guard { slot->mutex };
            if (slot->done)
                return false;
            slot->waiter = handle;
            return true;
        }

        [[nodiscard]] ResolveResult await_resume() const
        {
            std::scoped_lock const guard { slot->mutex };
            return slot->result;
        }
    };

} // namespace

/// Pool, queue, and the state Stop() has to reach.
struct ThreadedAddressResolver::Impl
{
    IAddressResolver& inner;
    ThreadedResolverOptions options;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping { false };

    /// One queued lookup.
    struct Job
    {
        std::shared_ptr<ResolveSlot> slot;
        IReactor* reactor { nullptr };
        std::string host;
        std::uint16_t port { 0 };
    };

    std::deque<Job> queue;
    std::vector<std::jthread> threads;

    std::atomic<std::size_t> refused { 0 };
    std::atomic<std::size_t> offloaded { 0 };

    Impl(IAddressResolver& resolver, ThreadedResolverOptions opts) noexcept:
        inner { resolver },
        options { opts }
    {
    }

    /// One worker's whole life: take a job, resolve it, publish it.
    void RunWorker()
    {
        while (true)
        {
            Job job;
            {
                std::unique_lock lock { mutex };
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }

            auto resolved = inner.Resolve(job.host, job.port);
            if (resolved.has_value())
                Settle(*job.slot, job.reactor, std::move(*resolved));
            else
                Settle(*job.slot, job.reactor, std::unexpected(ResolveFailure(job.host, job.port, resolved.error())));
        }
    }

    /// Start the pool on first use.
    ///
    /// Lazily, because a process whose dials are all literals -- which is most of
    /// them here -- must not pay for threads it never uses. Caller holds `mutex`.
    void EnsureStarted()
    {
        if (!threads.empty() || stopping)
            return;
        threads.reserve(options.threads);
        for (std::size_t i = 0; i < options.threads; ++i)
            threads.emplace_back([this] { RunWorker(); });
    }
};

ThreadedAddressResolver::ThreadedAddressResolver(IAddressResolver& inner, ThreadedResolverOptions options):
    _impl { std::make_unique<Impl>(inner, options) }
{
}

ThreadedAddressResolver::~ThreadedAddressResolver()
{
    Stop();
}

std::size_t ThreadedAddressResolver::Refused() const noexcept
{
    return _impl->refused.load(std::memory_order_relaxed);
}

std::size_t ThreadedAddressResolver::Offloaded() const noexcept
{
    return _impl->offloaded.load(std::memory_order_relaxed);
}

void ThreadedAddressResolver::Stop() noexcept
{
    std::deque<Impl::Job> abandoned;
    {
        std::scoped_lock const guard { _impl->mutex };
        if (_impl->stopping)
            return;
        _impl->stopping = true;
        abandoned.swap(_impl->queue);
    }
    _impl->wake.notify_all();

    // Every queued lookup is resumed rather than dropped, so no coroutine is left
    // parked on an answer that will never come.
    for (auto& job: abandoned)
        Settle(*job.slot,
               job.reactor,
               std::unexpected(NetError {
                   .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "resolver stopped before the lookup ran" }));

    _impl->threads.clear(); // joins
}

Task<ResolveResult> ThreadedAddressResolver::Resolve(std::string host, std::uint16_t port, IReactor* reactor)
{
    // A literal needs no lookup, so it needs no thread. See the class comment:
    // here that is the common case, not the exotic one.
    //
    // The null-reactor arm is a correctness requirement rather than a second
    // optimisation: with nowhere to Submit a result back to, offloading would
    // park a coroutine nothing could ever resume. Resolving inline is the only
    // answer that works for a caller without a reactor, and it is what keeps
    // `SyncRun` sound over this resolver.
    if (reactor == nullptr || Detail::IsNumericHost(host))
    {
        auto resolved = _impl->inner.Resolve(host, port);
        if (!resolved.has_value())
            co_return std::unexpected(ResolveFailure(host, port, resolved.error()));
        co_return std::move(*resolved);
    }

    auto slot = std::make_shared<ResolveSlot>();
    {
        std::scoped_lock const guard { _impl->mutex };
        if (_impl->stopping)
            co_return std::unexpected(
                NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "resolver is stopping" });

        if (_impl->queue.size() >= _impl->options.maxQueueDepth)
        {
            _impl->refused.fetch_add(1, std::memory_order_relaxed);
            co_return std::unexpected(NetError {
                .code = NetErrorCode::WouldBlock,
                .systemCode = 0,
                .context = std::format(
                    "resolver queue is full ({} waiting); not resolving {}:{}", _impl->queue.size(), host, port) });
        }

        _impl->EnsureStarted();
        _impl->queue.push_back(Impl::Job { .slot = slot, .reactor = reactor, .host = std::move(host), .port = port });
        _impl->offloaded.fetch_add(1, std::memory_order_relaxed);
    }
    _impl->wake.notify_one();

    co_return co_await SlotPark { .slot = std::move(slot) };
}

} // namespace FastCache

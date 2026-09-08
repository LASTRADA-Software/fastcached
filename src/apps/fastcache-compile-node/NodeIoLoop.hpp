// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/PlatformConnector.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace FastCache::Node
{

class FrameServer;

/// The reactor the node's framed surfaces share, the connector over it, and the
/// thread that runs them -- owned as one thing.
///
/// ## Why this exists at all
///
/// Each `FrameEndpoint` used to own a `std::jthread` and serve its connections one
/// at a time. That is what made a slow upstream a node-wide fault: the cache
/// surface consults the shared cache from inside its own answer, so one dial that
/// took five seconds held every local `fastcache-cc` behind it. Making the dial
/// suspendable is only half the fix -- it needs a loop to suspend on.
///
/// ## This is NOT consensus's reactor, deliberately
///
/// `ConsensusTier` runs its own, and the two stay apart because a cache answer now
/// awaits an upstream dial with a multi-second ceiling. Putting that in front of
/// the Raft heartbeat timer would be this same defect moved one layer over, and
/// this repository already has a name for what that looks like: nine role changes
/// in twelve seconds.
///
/// The two framed surfaces DO share one loop with each other. Both are short
/// request/reply, neither is CPU-bound, and they have the same latency budget --
/// so a second thread would buy nothing but a second thread.
///
/// ## Header hygiene
///
/// This is the only node header that includes `PlatformReactor.hpp`, and that is
/// load-bearing on Windows: it drags in `<windows.h>`, and a header that did so
/// while also being included by anything wanting `<winsock2.h>` is the classic
/// redefinition. Every other signature takes `NodeIoLoop&`, which is
/// forward-declarable.
class NodeIoLoop
{
  public:
    NodeIoLoop();

    NodeIoLoop(NodeIoLoop const&) = delete;
    NodeIoLoop(NodeIoLoop&&) = delete;
    NodeIoLoop& operator=(NodeIoLoop const&) = delete;
    NodeIoLoop& operator=(NodeIoLoop&&) = delete;

    /// Joins the loop thread. The adopted servers stop the reactor themselves.
    ~NodeIoLoop();

    /// @return The reactor every adopted loop and every socket here is pinned to.
    [[nodiscard]] PlatformReactor& Reactor() noexcept
    {
        return _reactor;
    }

    /// @return The loop's resolver, for a component that holds a resolved address
    ///         itself rather than re-resolving per operation (`RemoteUpstream`).
    ///
    /// The same one `_connector` dials through, deliberately: two resolvers would be
    /// two thread pools and two answers to one question.
    [[nodiscard]] IAsyncAddressResolver& Resolver() noexcept
    {
        return _resolver;
    }

    /// @return A connector whose sockets belong to `Reactor()`.
    ///
    /// Reactor-driven, so a dial from inside an answer suspends rather than
    /// stalling every other connection on the loop.
    [[nodiscard]] IConnector& Connector() noexcept
    {
        return _connector;
    }

    /// Register a server whose loop must finish before the reactor may stop.
    ///
    /// Called before `Start()`. Adoption is what makes the reference-counted stop
    /// below possible: the reactor may only stop once every loop that could still
    /// be parked on it has ended.
    /// @param server The server to run; must outlive this loop.
    void Adopt(FrameServer& server);

    /// Spawn every adopted loop, then run the reactor on a thread of its own.
    ///
    /// The loops are spawned on the CALLING thread and the reactor thread starts
    /// after, which is the ordering `ConsensusTier::Launch` already uses: a client
    /// that dials the moment `Start()` returns must not find a listener nobody is
    /// accepting on.
    void Start();

    /// Take ownership of a listener this reactor owns, and free it once the
    /// reactor has stopped.
    ///
    /// **The rule is `IReactor::TeardownIsSerialisedWithDispatch()`**: an object a
    /// reactor owns is destroyed on that reactor's worker thread, or with that
    /// reactor stopped, because clearing a pending awaitable anywhere else races the
    /// completion dispatch
    /// ([#668](https://github.com/LASTRADA-Software/fastcached/issues/668)).
    /// `~FrameEndpoint` could satisfy neither on its own: it runs on whatever thread
    /// owns the endpoint, and `FrameServer::Shutdown()`'s drain waits for the very
    /// condition that *triggers* the stop -- the last loop decrements before it calls
    /// `NoteLoopFinished()`, and `Stop()` only sets a flag and posts a wakeup, so
    /// `Running()` stays true until `Run()` returns. The endpoint's members were then
    /// destroyed inside that gap
    /// ([#840](https://github.com/LASTRADA-Software/fastcached/issues/840)), which is
    /// what `Windows-cl-debug` reported intermittently and in a different
    /// `FrameEndpoint_test` case each run
    /// ([#737](https://github.com/LASTRADA-Software/fastcached/issues/737)).
    ///
    /// **Deferring is not the same as posting, and it is why this is here rather than
    /// in the endpoint.** Posting the destruction onto the reactor and waiting for it
    /// answers the multi-surface case and hangs on the ordinary one: a node runs a
    /// single framed surface, so the endpoint being destroyed is usually the last
    /// loop -- the reactor is stopping, the posted task will never be dequeued, and
    /// the wait can only end at its ceiling. Handing the listener here needs no wait
    /// at all: `_retired` is declared after the reactor and before the thread, so it
    /// is destroyed *after* the thread has been joined and *before* the reactor it
    /// points at, which is the "with that reactor stopped" arm of the rule, reached
    /// by construction rather than by racing for it.
    ///
    /// The cost is that a retired listener's memory lives until this loop does. The
    /// descriptor does not: `FrameServer::Shutdown()` has already closed it on the
    /// reactor, so the port is free the moment the endpoint stops. The number
    /// retained is the number of endpoints this loop ever served.
    ///
    /// Safe from any thread; an endpoint may be dropped from one that is not the
    /// owner's. @p listener may be null, which retires nothing.
    /// @param listener The listener to hold until the reactor has stopped.
    void Retire(std::unique_ptr<IListener> listener);

    /// Note that one more loop is running.
    ///
    /// For loops a server spawns for itself rather than ones the owner adopted --
    /// the request-deadline sweeper is the case. A frame parked on the timer wheel
    /// is exactly what the reactor must not return over, so it has to be counted
    /// like any other.
    void NoteLoopStarted() noexcept;

    /// Note that one adopted loop has ended; the last one stops the reactor.
    ///
    /// The reactor is stopped by its loops rather than by a destructor, and that is
    /// not tidiness: `IReactor::Run` returns with its timer heap and its parked work
    /// exactly where they were, so stopping it while any per-connection task or
    /// sweeper is still suspended leaves a coroutine frame nobody resumes and nobody
    /// frees.
    void NoteLoopFinished() noexcept;

    /// @return How many adopted loops have not yet finished. For teardown
    ///         assertions.
    [[nodiscard]] std::size_t LoopsRunning() const noexcept
    {
        return _loopsRunning.load(std::memory_order_acquire);
    }

  private:
    // Declaration order IS construction order and each is referenced by the one
    // below it, which is the ordering this class exists to make the language check.
    SteadyClock _clock;
    PlatformReactor _reactor { _clock };

    /// Name resolution for the upstream dial, off this thread.
    ///
    /// Threaded rather than inline because `getaddrinfo` takes no timeout: a shared
    /// cache named by hostname whose resolver is wedged would otherwise park the
    /// loop carrying every local client's connection. A literal address -- which is
    /// what `FASTCACHE_ADDR` almost always is -- never reaches a thread.
    ThreadedAddressResolver _resolver;

    PlatformConnector _connector { _reactor, _resolver, _clock };

    std::vector<FrameServer*> _loops;
    std::atomic<std::size_t> _loopsRunning { 0 };

    /// Listeners handed over by `Retire`, freed when this loop is.
    ///
    /// **Its POSITION is the mechanism.** Declared after `_reactor` and before
    /// `_thread`, so destruction -- which runs in reverse -- joins the thread first,
    /// then frees these with `Run()` provably returned, then frees the reactor they
    /// name. Moving this declaration either way breaks the guarantee silently: above
    /// `_reactor` and a listener outlives the reactor its destructor asks; below
    /// `_thread` and it is freed while the loop still turns, which is the defect.
    std::mutex _retiredMutex;
    std::vector<std::unique_ptr<IListener>> _retired;

    std::jthread _thread;
};

} // namespace FastCache::Node

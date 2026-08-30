// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// What a stop should do next about the compiles still running.
///
/// Split out of `~WorkerServer` because the interesting branch **ends the process**,
/// and a side effect no test can survive is one no test will check. The decision is
/// arithmetic over three values and is exhaustively unit-tested here; the destructor
/// is left with nothing but carrying it out.
enum class DrainAction : std::uint8_t
{
    /// Nothing is running. Stop cleanly.
    Finished = 0,
    /// Still inside the bound. Say what is outstanding and keep waiting.
    Report,
    /// The bound is spent. Say what is being abandoned and end the process.
    Abandon,
    Last, ///< Not an action; `EnumTable`'s length.
};

/// Decide what a stop does next.
///
/// `Finished` outranks everything, including an expired bound: a stop that has
/// nothing left to wait for is clean however long it took to get there, and
/// reporting it as an abandonment would put a false alarm in the operator's log at
/// exactly the moment the thing worked.
///
/// A zero @p timeout never expires. That is the behaviour this had before the bound
/// existed, kept sayable so an operator who prefers the supervisor's timeout to this
/// one can ask for it.
/// @param outstanding Compiles still holding a slot.
/// @param waited How long the stop has been waiting.
/// @param timeout The bound, or zero to wait forever.
/// @return What to do next.
[[nodiscard]] DrainAction NextDrainAction(std::size_t outstanding,
                                          std::chrono::steady_clock::duration waited,
                                          std::chrono::seconds timeout) noexcept;

/// Largest request this worker surface will buffer.
///
/// A COMPILE carries a preprocessed translation unit, which for real C++ runs to
/// several megabytes; 256 MiB is far above any of them and matches the daemon's own
/// default value ceiling. It exists so a peer cannot declare a length this worker
/// would try to allocate.
///
/// **Exported rather than file-local because a second party needs the same figure.**
/// `Cc::WorkerProtocol` refuses a codec envelope whose *declared decompressed* size
/// exceeds this surface's ceiling, and it cannot see the listener that enforced the
/// frame length — so the surface has to hand it the number. Left to each side's own
/// constant, the two are two literals that must agree forever, and lowering one
/// silently stops bounding the other.
inline constexpr std::size_t WorkerMaxRequestBytes = 256ULL * 1024ULL * 1024ULL;

/// Accepts connections and answers each with one compile.
///
/// Shaped after `AdminHttpServer` rather than `Server`, and for the reason that
/// governs the whole node: `Connection` is built around a `CacheEngine`, and a
/// worker has no cache. Taking an `IListener&` and running its own loop keeps the
/// node clear of the cache stack entirely while still reusing the reactor, the
/// socket abstraction and the TLS wrapper.
///
/// ## The compile runs on a pool, and the loop keeps accepting
///
/// A compile spawns a process and blocks for seconds, so it cannot run on the
/// accept loop -- served inline, a worker advertising thirty slots ran exactly one
/// at a time, `_inFlight` could never exceed 1, and the cap below was unreachable
/// (#213). It cannot run on a reactor either, for the same reason: it would stall
/// every other coroutine that reactor owns.
///
/// So it is awaited onto an `IExecutor` -- one line inside an otherwise linear
/// body -- and the accept loop goes straight back to accepting. Sizing that
/// executor to `slots` is what makes an admitted job always find a thread, which
/// is why the cap and the pool are the same number and why this takes the executor
/// rather than making one.
///
/// ## One request per connection
///
/// A client opens a connection, sends one `Compile`, reads one reply, and closes —
/// the same shape the launcher already uses for every cache operation. There is no
/// command loop here on purpose: a compile occupies a slot for seconds, so a
/// connection that could send a second one would let a single client hold a slot
/// indefinitely, and the slot accounting the scheduler does would stop meaning
/// anything.
///
/// ## The concurrency cap is the promise the scheduler relies on
///
/// A worker advertises `slots` at registration and the scheduler dispatches on that
/// number. If the worker accepted more, its compiles would contend for the same
/// cores and every one of them would get slower — the fleet would be *fuller* than
/// the scheduler thinks and *slower* than it thinks, in the same moment. So the cap
/// is enforced here as well as advertised, and a job over it is refused rather than
/// queued: refusing costs the client one local compile, while queueing hides the
/// overload from the scheduler that is trying to route around it.
class WorkerServer
{
  public:
    /// @param jobs Where a compile runs; must outlive the run. Size it to `slots`.
    /// @param listener Bound listener; must outlive the run.
    /// @param protocol Answers each request; must outlive the run.
    /// @param slots Maximum concurrent compiles.
    /// @param metrics Counts the refusals this loop makes; must outlive the run.
    /// @param logger Shared logger.
    ///
    /// The cap refusal is counted **here** rather than in the protocol, because
    /// that is where it happens: the check is before the request is read, so the
    /// protocol never sees the job at all. Counting it beside the other refusals
    /// would mean reporting a busy worker as one whose toolchain does not match.
    /// @param membership Decides who may spend this machine's CPU; must outlive
    ///        the run. Checked **before** the request is read, for the reason the
    ///        slot cap is: a caller with no claim on this machine must not be able
    ///        to make it buffer a multi-megabyte preprocessed payload first.
    WorkerServer(IListener& listener,
                 Cc::WorkerProtocol& protocol,
                 std::size_t slots,
                 Distributed::IMembershipOracle const& membership,
                 IMetricsSink& metrics,
                 ILogger& logger,
                 IExecutor& jobs,
                 std::chrono::seconds drainTimeout = std::chrono::seconds { 30 }) noexcept;

    /// Stops admitting, then waits for every compile still running to finish.
    ///
    /// A job decrements `_inFlight` when it ends, and that member lives HERE -- so a
    /// server destroyed while one of its own jobs was still on the pool would be
    /// freeing the counter out from under it. Waiting here rather than relying on
    /// the pool being destroyed first also removes an ordering an assembler has to
    /// get right: the executor is injected, so nothing about this class can require
    /// it to be declared in a particular place.
    ///
    /// `Shutdown()` first, and that is not tidiness: a drain that does not close the
    /// door can be overtaken by the accept loop admitting one more job, and would
    /// then return having waited for a count that went back up behind it.
    ///
    /// The wait is bounded by `drainTimeout`, and what it does on expiry is a
    /// decision rather than an omission (#239).
    ///
    /// This used to be unbounded, on reasoning that was **correct and is still
    /// correct**: a job on the executor holds a pointer into this object -- the
    /// counter, the protocol, the metrics sink, the logger, the byte budget -- so
    /// returning from here while one is still running frees every one of them under
    /// it. Bounding the wait does not make that safe, and nothing here pretends it
    /// does.
    ///
    /// What was wrong was the conclusion that the wait therefore had to be
    /// unbounded. That does not avoid the ending, it only chooses who picks it: the
    /// supervisor's own timeout, answered with `SIGKILL` and no diagnostic -- on
    /// Windows an SCM stop timeout an operator reads as "the service is hung". One
    /// wedged compiler is enough, and nothing bounds a compile today.
    ///
    /// So on expiry this **says what it is abandoning and ends the process**, rather
    /// than returning into a teardown that would free members a running compile is
    /// still inside. That is the same ending, taken deliberately, at a moment this
    /// process chooses, with the count and the timeout on the record.
    ///
    /// It is strictly better once a compile is bounded too: with #239's other half
    /// the wedge is killed, the slot comes back, and this returns normally. Until
    /// then this converts a silent kill into a stated one.
    ~WorkerServer();

    WorkerServer(WorkerServer const&) = delete;
    WorkerServer& operator=(WorkerServer const&) = delete;
    WorkerServer(WorkerServer&&) = delete;
    WorkerServer& operator=(WorkerServer&&) = delete;

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    ///
    /// Returning does NOT mean the worker is idle: jobs admitted before the listener
    /// closed are still running on the executor. The destructor is what waits.
    [[nodiscard]] Task<void> Run();

    /// Close the listener to unblock `Run()`.
    void Shutdown() noexcept;

    /// Compiles running right now, for the heartbeat.
    [[nodiscard]] std::size_t InFlight() const noexcept;

  private:
    /// Give back one slot and wake a drain that may be waiting for it.
    ///
    /// Two callers -- a job that finished, and a job that could not be started at
    /// all -- and both must wake the destructor, so it is one function rather than
    /// two spellings of a three-line critical section.
    void ReleaseSlot() noexcept;

    /// Serve one accepted connection, on the executor, and release its slot.
    ///
    /// Detached rather than awaited, which is the point: awaiting it here would put
    /// the accept loop back to serving one connection at a time. A firewall around
    /// the body because a `DetachedTask`'s `unhandled_exception` calls
    /// `std::terminate` -- an exception serving one client must cost that client,
    /// not the worker and every compile on it. The same shape `Server.cpp` uses.
    /// @param socket The accepted connection.
    DetachedTask ServeDetached(std::unique_ptr<ISocket> socket);

    /// The linear body: read the request, run it, answer, close.
    ///
    /// Takes the socket BY VALUE. A coroutine's reference parameter is not kept
    /// alive by its frame, so it only works while some other frame happens to
    /// outlive this one -- true here, and the kind of true that stops being true
    /// when somebody reorders the caller. Owning it makes the lifetime the
    /// coroutine's own.
    /// @param socket The accepted connection, owned for the duration.
    [[nodiscard]] Task<void> Serve(std::unique_ptr<ISocket> socket);

    IListener& _listener;
    IExecutor& _jobs;
    Cc::WorkerProtocol& _protocol;
    std::size_t _slots;
    Distributed::IMembershipOracle const& _membership;
    IMetricsSink& _metrics;
    ILogger& _logger;
    std::atomic<bool> _shuttingDown { false };

    /// Payload bytes the jobs running right now are reading, against a budget.
    ///
    /// The slot cap bounds CPU; this bounds memory, and they are not the same
    /// question -- `slots` jobs each declaring the per-request maximum is `slots`
    /// times it. Serving one at a time used to answer both at once.
    std::atomic<std::size_t> _bytesInFlight { 0 };

    /// Atomic for the cap check, which is on the accept path and takes no lock.
    std::atomic<std::size_t> _inFlight { 0 };

    /// Guards the drain, and it has to be a lock rather than `_inFlight.wait()`.
    ///
    /// An atomic wait can return on observing the store alone, without the paired
    /// notify -- so the destructor could see zero, run to completion and free this
    /// object while the job that released the last slot was still inside
    /// `notify_all` on a member of it. Waking through a mutex the notifier holds
    /// means the notifier is provably past its critical section first.
    std::mutex _drainMutex;
    std::condition_variable _drained;

    /// How long a stop waits before it abandons what is still running. Zero waits
    /// forever, which is what this did before the bound existed.
    std::chrono::seconds _drainTimeout { 30 };
};

} // namespace FastCache::Node

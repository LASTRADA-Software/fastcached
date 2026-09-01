// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompileCapacity.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

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

/// What a compile surface refuses with, each pairing the wire code with its counter.
///
/// **Two surfaces spend these now.** They were a file-local table in `WorkerServer.cpp`
/// while an accept loop was the only way a compile could arrive; since #290 a
/// `CompileResponder` on the merged `0xFC` listener admits compiles too, and it refuses
/// the same callers for the same reasons. Two tables would be two answers to "what does
/// this worker refuse with", read side by side on one `/metrics` page -- the drift this
/// codebase treats as a defect rather than a coincidence.
///
/// Distinct from `WorkerProtocol`'s own rows, which are about a frame that was ADMITTED
/// and then would not decode. These are the admission refusals: who is asking, whether
/// a core is free, and whether the memory is.
///
/// `PayloadTooLarge` had no counter at all until #326, and it is the one that most
/// needed one: the frame-level check needs only a header, where the envelope refusals
/// need a whole frame to have been sent and read. So an operator alerting on the
/// envelope series watched a client hammer this port with oversized declarations, saw
/// every one refused correctly, and read a flat graph as "nobody is talking to us".
namespace CompileRefusal
{
    /// The caller has no claim on this machine's CPU.
    inline constexpr Cc::SurfaceRefusal NotAMember {
        .code = CompileCacheWire::ErrorCode::NotAMember,
        .counter = IMetricsSink::Counter::WorkerJobsRefusedNotAMember,
    };
    /// Every slot is taken; the fleet is full and the scheduler should route around it.
    inline constexpr Cc::SurfaceRefusal NoCapacity {
        .code = CompileCacheWire::ErrorCode::NoCapacity,
        .counter = IMetricsSink::Counter::WorkerJobsRefusedNoSlot,
    };
    /// A slot was free and the memory was not; come back shortly.
    inline constexpr Cc::SurfaceRefusal EndpointBusy {
        .code = CompileCacheWire::ErrorCode::EndpointBusy,
        .counter = IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy,
    };
    /// The declared frame is above what this surface will buffer at all.
    inline constexpr Cc::SurfaceRefusal PayloadTooLarge {
        .code = CompileCacheWire::ErrorCode::PayloadTooLarge,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedPayloadTooLarge,
    };
    /// This worker has begun stopping and admits nothing more.
    ///
    /// **Not `NoCapacity`, and the split is the whole reason a row is a row.** An
    /// operator acts on the two oppositely: `NoCapacity` says the fleet is too small,
    /// this says a node is draining and a retry lands elsewhere. Summed, a rolling
    /// restart reads as permanent under-capacity. The client sees one code either way,
    /// because it does the same thing with both -- which is exactly why the counter is
    /// the half that has to differ.
    inline constexpr Cc::SurfaceRefusal Stopping {
        .code = CompileCacheWire::ErrorCode::NoCapacity,
        .counter = IMetricsSink::Counter::WorkerJobsRefusedStopping,
    };
    /// A pre-payload decision naming a verb this build has no row for.
    inline constexpr Cc::SurfaceRefusal UnknownOpcode {
        .code = CompileCacheWire::ErrorCode::UnknownOpcode,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnknownOpcode,
    };
    /// A compile verb reached before a credential. Zero on every shipped shape.
    inline constexpr Cc::SurfaceRefusal Unauthenticated {
        .code = CompileCacheWire::ErrorCode::Unauthenticated,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnauthenticated,
    };
} // namespace CompileRefusal

/// Refuse a caller with no claim on this machine's CPU, or admit it.
///
/// **The one implementation of the anti-leeching rule**, asked by both doors into this
/// worker: `WorkerServer`'s accept loop and, since #290, `CompileResponder` on the
/// merged `0xFC` listener. Written twice it would be two policies that agree today, on
/// a question whose wrong answer is "this machine ran a stranger's compiler for them".
///
/// Answered on the peer's HOST alone, which is what lets both callers ask it before a
/// payload byte is read -- a caller with no claim here must not be able to make this
/// process buffer a multi-megabyte preprocessed translation unit on the way to being
/// refused. It is a *reply* rather than a close, so a misconfigured peer learns which
/// of the two it is instead of seeing a connection it cannot tell from a dead host.
///
/// @param membership Decides who may spend this machine's CPU.
/// @param metrics Where the refusal is counted, exactly once.
/// @param peer The caller's peer host.
/// @return The encoded refusal, or nullopt when the caller is admitted.
[[nodiscard]] std::optional<std::vector<std::byte>> RefuseUnlessMember(Distributed::IMembershipOracle const& membership,
                                                                       IMetricsSink& metrics,
                                                                       std::string_view peer);

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

    /// Close every door into this worker, then wait for what is running.
    ///
    /// The destructor's body, callable early -- and calling it early is not a
    /// convenience, it is an ORDERING that the merged surface made load-bearing
    /// (#290).
    ///
    /// A compile admitted through the merged `0xFC` listener finishes on the pool and
    /// then hops back onto the node's reactor to return its reply. That reactor is
    /// stopped by `NodeIoLoop` when the last ADOPTED loop ends -- the accept loop and
    /// the sweeper -- and a connection task parked off-reactor is not one of them. So
    /// if the surface is torn down first, the reactor can stop while a compile is still
    /// out on the pool: the hop home is posted to a port nobody drains, the coroutine
    /// is never resumed, its slot and its byte reservation are never released, and this
    /// class then waits the whole drain timeout and `_Exit`s reporting compiles still
    /// running that had in fact already finished. The bounded stop would blame the
    /// thing it broke.
    ///
    /// Destruction order cannot fix that on its own: the surface holds a pointer to the
    /// responder and the responder holds this worker's capacity, so the three must be
    /// destroyed surface-first -- which is the opposite of the order the drain needs.
    /// Separating the stop from the destruction is what lets both be right. `WorkerBody`
    /// calls this while every loop is still turning; the destructor calls it again and
    /// finds nothing left to wait for.
    ///
    /// Idempotent. `BeginShutdown` is a latch and a drain with no outstanding compiles
    /// returns `Finished` at once.
    void StopAndWait();

    /// The slot cap, byte budget and drain this worker answers to.
    ///
    /// Handed out rather than kept private, because a worker has two doors since #290
    /// and **both must spend the same accounting**: this accept loop and the
    /// `CompileResponder` on the merged `0xFC` listener. A second `CompileCapacity`
    /// would make the worker answer to two caps depending on which door a client came
    /// through, and the slot figure it advertises to the fleet would describe neither.
    ///
    /// It is also what makes one drain cover both. `~WorkerServer` closes this door and
    /// then waits on the count, and a compile admitted through the other door holds a
    /// slot in the very same counter -- so the bounded stop (#239) covers the merged
    /// surface without a second wait that would have to be ordered against this one.
    /// @return The shared accounting.
    [[nodiscard]] CompileCapacity& Capacity() noexcept
    {
        return _capacity;
    }

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
    Distributed::IMembershipOracle const& _membership;
    IMetricsSink& _metrics;
    ILogger& _logger;

    /// The slot cap, the in-flight byte budget and the drain, in one place.
    ///
    /// Held rather than spelled out here, because #290 gives all three a second
    /// user: when the compile verbs move onto the merged `0xFC` surface a responder
    /// answers them, and it must spend the SAME capacity this accept loop does --
    /// otherwise a worker answers to two caps depending on which door a client came
    /// through, and the figure it advertises to the fleet describes neither.
    ///
    /// The reasoning that used to sit on the individual members lives on
    /// `CompileCapacity` now, including why the two counters are not one question
    /// and why the drain waits on a condition variable rather than `atomic::wait`.
    CompileCapacity _capacity;
};

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// What a worker may spend on compiles at once, and how it stops.
///
/// Three ceilings and a shutdown, held together because they are one question
/// asked from more than one place. They lived inside `WorkerServer`'s accept loop,
/// which was correct while an accept loop was the only way a compile could arrive.
/// It is not any more: #290 folds the compile verbs onto the merged `0xFC` surface,
/// where a frame reaches a responder rather than a listener, and the accounting has
/// to be the same accounting or a worker would answer to two different caps
/// depending on which door a client used.
///
/// This is step 1 of that: **the state moves, the behaviour does not.** Nothing here
/// decides anything new, and `NextDrainAction` — which is the only real decision —
/// stays exactly where it was, pure and separately tested.
///
/// Two counters, not one, and they are not interchangeable:
///
///   * **Slots** bound how many compilers run at once. That is a CPU question, and
///     the answer is what the worker advertises to the fleet.
///   * **Bytes** bound what is held in memory for requests that have arrived and are
///     not finished. Detaching the compiles turned the per-request payload cap into
///     a per-connection one, so a worker with a free slot could still be out of
///     memory — which is why `EndpointBusy` is a separate refusal from `NoCapacity`.
///     An operator sent to buy machines over a transient byte budget is being sent
///     to fix something that was never wrong.
///
/// Not thread-safe by accident: it is thread-safe on purpose, because the compiles
/// it counts run on an executor while the loop that admitted them keeps accepting.
class CompileCapacity
{
  public:
    /// @param slots How many compiles may run at once.
    /// @param byteBudget How many bytes of in-flight request payload may be held.
    /// @param drainTimeout How long `Drain` waits before abandoning what is running.
    /// @param logger Where the drain reports progress; must outlive this.
    CompileCapacity(std::size_t slots, std::size_t byteBudget, std::chrono::seconds drainTimeout, ILogger& logger) noexcept:
        _slots { slots },
        _byteBudget { byteBudget },
        _drainTimeout { drainTimeout },
        _logger { logger }
    {
    }

    CompileCapacity(CompileCapacity const&) = delete;
    CompileCapacity& operator=(CompileCapacity const&) = delete;
    CompileCapacity(CompileCapacity&&) = delete;
    CompileCapacity& operator=(CompileCapacity&&) = delete;
    ~CompileCapacity() = default;

    /// A held byte reservation, released when it goes out of scope.
    ///
    /// RAII because the paths out of a compile are many — a refusal, a decode
    /// failure, a socket that closed, the ordinary reply — and a release written at
    /// each of them is a release that will be missed at the next one added.
    class Bytes
    {
      public:
        /// @param owner What to release back to.
        /// @param bytes How much is held.
        Bytes(CompileCapacity* owner, std::size_t bytes) noexcept:
            _owner { owner },
            _bytes { bytes }
        {
        }

        Bytes(Bytes const&) = delete;
        Bytes& operator=(Bytes const&) = delete;
        Bytes(Bytes&& other) noexcept:
            _owner { other._owner },
            _bytes { other._bytes }
        {
            other._owner = nullptr;
        }
        Bytes& operator=(Bytes&&) = delete;

        ~Bytes()
        {
            if (_owner != nullptr)
                _owner->_bytesInFlight.fetch_sub(_bytes, std::memory_order_acq_rel);
        }

        /// Raise this reservation to @p total, or leave it untouched.
        ///
        /// A codec envelope declares an expansion larger than the frame it arrived
        /// in, and that larger figure is what the request actually costs. Raising
        /// rather than taking a second reservation keeps one release on one object.
        /// @param total The new total for this request.
        /// @return True when the budget allowed it.
        [[nodiscard]] bool TryRaiseTo(std::size_t total) noexcept
        {
            if (_owner == nullptr || total <= _bytes)
                return true;
            if (!_owner->TakeBytes(total - _bytes))
                return false;
            _bytes = total;
            return true;
        }

      private:
        CompileCapacity* _owner;
        std::size_t _bytes;
    };

    /// Take one slot, if the cap allows.
    /// @return True when a slot was taken; the caller then owes a `ReleaseSlot`.
    [[nodiscard]] bool TryTakeSlot() noexcept;

    /// Give one slot back, and wake a drain that may be waiting on it.
    void ReleaseSlot() noexcept;

    /// Reserve @p want bytes of request payload.
    /// @param want How many bytes this request declared.
    /// @return The reservation, or nullopt when the budget is spent.
    [[nodiscard]] std::optional<Bytes> TryTakeBytes(std::size_t want) noexcept;

    /// Stop admitting. Idempotent, and it closes nothing — whoever owns the door
    /// closes the door.
    void BeginShutdown() noexcept;

    /// @return Whether `BeginShutdown` has been called.
    [[nodiscard]] bool IsShuttingDown() const noexcept;

    /// @return How many compiles are running now.
    [[nodiscard]] std::size_t InFlight() const noexcept;

    /// @return The slot cap this was built with.
    [[nodiscard]] std::size_t Slots() const noexcept
    {
        return _slots;
    }

    /// @return The in-flight byte budget.
    [[nodiscard]] std::size_t ByteBudget() const noexcept
    {
        return _byteBudget;
    }

    /// Whether @p footprint is a price this budget may be asked to pay at all.
    ///
    /// **A predicate rather than a comparison, because its inversion has a name.** A
    /// request costing more than the WHOLE budget can never be afforded, however idle
    /// this worker is -- so charging it would answer `EndpointBusy`, which means "come
    /// back shortly", to a frame that will never fit. The client then retries forever
    /// against a ceiling it cannot see. Left uncharged, it reaches the decoder, which
    /// refuses it by name as `payload-too-large` and without allocating a byte.
    ///
    /// The hand-off is sound only while the decoder's ceiling is no larger than this
    /// budget, which is why `WorkerMaxRequestBytes` is one exported constant rather
    /// than a literal per side.
    ///
    /// Both doors into this worker ask it -- the accept loop and the merged surface's
    /// responder -- and that is the whole reason it is here rather than spelled at each.
    /// They still REACH the charge differently, and legitimately: the accept loop
    /// reserves the frame length first and raises, because it has not read the payload
    /// yet; the responder is handed a complete frame and charges once. What they arrive
    /// at is `ChargeFor` below, and it is the same number.
    /// @param footprint What the request declares it will cost.
    /// @return False when no state of this budget could ever afford it.
    [[nodiscard]] bool IsChargeable(std::size_t footprint) const noexcept
    {
        return footprint <= _byteBudget;
    }

    /// What a request declaring @p footprint over @p framed bytes is charged.
    ///
    /// One number, asked by both doors, and it exists because they disagreed about
    /// exactly one case (#448). An unpayable footprint is not charged as a footprint
    /// -- `IsChargeable` above says why -- but the frame has still arrived and is
    /// still held, so the bytes are real and something must carry them. The accept
    /// loop kept the frame length it had already reserved; the responder, which takes
    /// its reservation in one go, charged nothing at all. Both were defensible alone
    /// and the pair was not: the whole thesis of one worker behind two doors is that
    /// the accounting does not depend on which door was used.
    ///
    /// `framed` is the floor rather than a second policy: `DeclaredRequestFootprint`
    /// already returns exactly this for a frame it cannot look inside, so the
    /// unpayable case is charged what an undecodable one is.
    ///
    /// @param footprint What the request declares it will cost.
    /// @param framed The declared payload length of the frame that carried it.
    /// @return The figure to reserve; never more than the budget could hold.
    [[nodiscard]] std::size_t ChargeFor(std::size_t footprint, std::size_t framed) const noexcept
    {
        return IsChargeable(footprint) ? footprint : framed;
    }

    /// Wait for the running compiles to finish, reporting and then abandoning.
    ///
    /// Bounded, and it ENDS rather than returning to a caller that would free
    /// members a running job is still inside. An unbounded drain does not avoid an
    /// ending — it hands the choice to the supervisor, which answers `SIGKILL` with
    /// no diagnostic (#239).
    ///
    /// A condition variable, never `atomic::wait`: an atomic wait can return without
    /// the notify and free the object the notifier is still inside.
    void Drain();

  private:
    /// @param want How many bytes to add to the in-flight total.
    /// @return True when the budget allowed it.
    [[nodiscard]] bool TakeBytes(std::size_t want) noexcept;

    std::size_t _slots;
    std::size_t _byteBudget;
    std::chrono::seconds _drainTimeout;
    ILogger& _logger;

    std::atomic<bool> _shuttingDown { false };
    std::atomic<std::size_t> _bytesInFlight { 0 };
    std::atomic<std::size_t> _inFlight { 0 };

    std::mutex _drainMutex;
    std::condition_variable _drained;
};

// --- what the compile surface refuses with, and how a stop ends -----------------
//
// **Moved here from `WorkerServer.hpp` when #290 stage 3 retired the dedicated
// compile port.** None of it belonged to the accept loop: the drain decision is
// arithmetic over this object's own counters, the request ceiling is what bounds a
// frame this object budgets, and the refusal table is what every door onto the
// compile verbs answers with. They lived beside the listener because the listener
// used to be the only door.

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

} // namespace FastCache::Node

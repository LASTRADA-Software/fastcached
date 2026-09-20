// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// What `CompileCapacity::TryTakeSlot` decided about one compile.
///
/// Four answers rather than a `bool`, because three of them are refusals an operator
/// acts on differently -- each is its own `CompileRefusal` row and its own counter --
/// and the decision between them has to be made in ONE step with the take. A cordon
/// checked beside the take rather than inside it lets a compile slip in after the
/// cordon's own drained report, which is then a report that stopping this node abandons
/// nothing, written while it would abandon something.
///
/// Private to this process: nothing transmits or persists it.
enum class SlotAdmission : std::uint8_t
{
    Taken,    ///< A slot was taken; the caller owes a `ReleaseSlot`.
    Stopping, ///< This worker has begun stopping.
    Cordoned, ///< An operator cordoned this worker.
    Full,     ///< Every slot is busy.
    Last,     ///< Not a decision; `EnumTable`'s length.
};

/// How the heartbeat's wait between two rounds ended.
///
/// Three answers, because the caller does something different for each: a stop ends
/// the loop, and the other two start a round -- but only one of them is an event worth
/// asserting, since a heartbeat that merely came round again would announce a changed
/// cordon too, a whole interval late.
///
/// Private to this process: nothing transmits or persists it.
enum class HeartbeatWake : std::uint8_t
{
    Elapsed,       ///< The interval passed and the cordon is still what was announced.
    CordonChanged, ///< The cordon moved away from what the last round announced.
    Stopped,       ///< A stop was requested, before the wait or during it.
};

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
/// **And whether an operator has cordoned it** (#1303), which is admission state for the
/// same reason the shutdown is: a cordon refuses new compiles and lets the running ones
/// finish, so it is asked at the take and answered at the release. It lives on this
/// object and nowhere else -- not in the configuration, not in the cluster's replicated
/// state -- so a restart un-cordons by construction. A cordon that survived a restart
/// would be a machine that silently never came back to the fleet.
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
    /// @param abandonment What `Drain` does with the compiles still running when the bound is
    ///        spent. Production's ends the process, which is why the arm went untested for so
    ///        long: a side effect no in-process case survives is one no case will check (#297).
    ///        A case supplies one that RETURNS and counts. Must outlive this.
    ///
    /// **`abandonment` is DEFAULTED on purpose, and the alternative was considered and measured**
    /// (#297). Requiring it would put the obligation at construction, which is not where the
    /// hazard is: a capacity that is never drained cannot end anything. Counted at the time --
    /// 18 construction sites, 17 of them tests, against 12 `Drain()` calls in the whole tree --
    /// so roughly eleven sites would have to name an abandonment they can never reach, which is
    /// *forgot* in the vocabulary of *decided* and teaches people to stop reading the argument.
    /// What closes the hazard instead is that `EndProcessOnAbandonedDrain` now says on STDERR
    /// what it is doing before it ends the process, so the default is legible rather than silent
    /// -- see `Core/BoundedDrain.hpp`. That fix sits in the shared seam, so it covers
    /// `ExpiryReaper`, the seam's other defaulted consumer, at the same time.
    CompileCapacity(std::size_t slots,
                    std::size_t byteBudget,
                    std::chrono::seconds drainTimeout,
                    ILogger& logger,
                    IDrainAbandonment& abandonment = DefaultDrainAbandonment()) noexcept:
        _slots { slots },
        _byteBudget { byteBudget },
        _drainTimeout { drainTimeout },
        _logger { logger },
        _abandonment { abandonment }
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

    /// Take one slot, unless this worker is stopping, is cordoned, or is full.
    ///
    /// Under the drain's mutex, which is what makes the cordon and the take one step: see
    /// `SlotAdmission`.
    /// @return `Taken` when a slot was taken, and the caller then owes a `ReleaseSlot`;
    ///         otherwise which refusal this is.
    [[nodiscard]] SlotAdmission TryTakeSlot() noexcept;

    /// Give one slot back, wake a drain that may be waiting on it, and report a cordoned
    /// worker that has just drained.
    void ReleaseSlot() noexcept;

    /// Cordon this worker, or lift its cordon.
    ///
    /// Idempotent: asking for the state it already has changes nothing and reports
    /// nothing, so a script that cordons twice does not log a second drained line.
    /// Cordoning a worker with nothing running reports it drained at once -- the report
    /// is the moment `ReleaseSlot` would have made it, and that moment has already passed.
    /// @param cordoned True to refuse new compiles, false to take them again.
    /// @return The state after the request took effect.
    CompileCacheWire::CordonFields Cordon(bool cordoned);

    /// @return Whether an operator has cordoned this worker.
    [[nodiscard]] bool IsCordoned() const noexcept;

    /// @return Serving, or cordoned and whether anything is still running.
    [[nodiscard]] CompileCacheWire::WireCordonState CordonState() const noexcept;

    /// Wait out the heartbeat interval, ending early on a stop or on the cordon moving.
    ///
    /// A heartbeat is the only way the scheduler learns a cordon -- nothing replicates
    /// it -- so a cordon that waited for the next scheduled round would leave this worker
    /// leased for up to a whole interval, every job refused here and compiled locally by
    /// its client. So `Cordon` wakes this wait (#1303).
    ///
    /// **The stop token and the cordon both take part in the wait**, the rule #1339 set
    /// for this loop: a poll in slices observes either a slice late and spends a wakeup
    /// per slice doing it. A condition variable, never `atomic::wait`, for `Drain`'s reason.
    /// @param stop Participates in the wait; a request ends it at once.
    /// @param announced The cordon the last round carried to the scheduler.
    /// @param interval How long to wait when nothing changes.
    /// @return Which of the three ENDED it: a stop wins over a cordon that moved as well,
    ///         and a cordon that moved without waking the wait before the interval ran
    ///         out is `Elapsed`, since the interval is what ended it.
    [[nodiscard]] HeartbeatWake WaitForHeartbeat(std::stop_token const& stop,
                                                 bool announced,
                                                 std::chrono::milliseconds interval);

    /// Reserve @p want bytes of request payload.
    /// @param want How many bytes this request declared.
    /// @return The reservation, or nullopt when the budget is spent.
    [[nodiscard]] std::optional<Bytes> TryTakeBytes(std::size_t want) noexcept;

    /// Stop admitting. Idempotent, and it closes nothing — whoever owns the door
    /// closes the door.
    void BeginShutdown() noexcept;

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
    /// Bounded, and in production it ENDS rather than returning to a caller that would
    /// free members a running job is still inside. An unbounded drain does not avoid an
    /// ending — it hands the choice to the supervisor, which answers `SIGKILL` with
    /// no diagnostic (#239).
    ///
    /// **It returns from an abandonment only when the injected `IDrainAbandonment`
    /// returns, and then the hazard above is the CALLER's** — the seam's own contract.
    /// A case that drives this past the ceiling therefore holds the in-flight count
    /// without a running job (`TryTakeSlot` and no release), so there is nothing
    /// borrowing this object for the return to free (#297).
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
    IDrainAbandonment& _abandonment;

    /// Say that a cordoned worker has nothing running. Called under `_drainMutex`.
    void ReportDrained() const;

    std::atomic<bool> _shuttingDown { false };
    std::atomic<bool> _cordoned { false };
    std::atomic<std::size_t> _bytesInFlight { 0 };
    std::atomic<std::size_t> _inFlight { 0 };

    std::mutex _drainMutex;
    std::condition_variable _drained;
    /// Notified by `Cordon` whenever the cordon moves; waited on by `WaitForHeartbeat`.
    /// `_any` because the wait takes a stop token.
    std::condition_variable_any _cordonMoved;
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
/// Split out of `~WorkerServer` -- now `WorkerTier::StopAndDrain` -- because the
/// interesting branch **ends the process**. The decision is arithmetic over three
/// values and is exhaustively unit-tested here; `Drain` is left with nothing but
/// carrying it out.
///
/// **The reason recorded here used to be "a side effect no test can survive is one no
/// test will check", and that has stopped being true** (#297): what ends the process is
/// an injected `IDrainAbandonment`, so a case supplies one that returns and asserts the
/// arm was reached. Splitting the arithmetic out is still right -- it is exhaustive
/// where a timing case can only be a sample -- but it is no longer the ONLY thing
/// covered, and a comment claiming an arm is untestable is a comment telling the next
/// person not to try.
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
    /// An operator cordoned this worker (#1303).
    ///
    /// `NoCapacity` on the wire for `Stopping`'s reason -- the client compiles locally
    /// either way -- and its own counter for the reason `Stopping` has one: a stop ends by
    /// itself and a cordon lasts until a person lifts it.
    inline constexpr Cc::SurfaceRefusal Cordoned {
        .code = CompileCacheWire::ErrorCode::NoCapacity,
        .counter = IMetricsSink::Counter::WorkerJobsRefusedCordoned,
    };
    /// A cordon asked for from another machine.
    ///
    /// `NotAMember`, the code the cache tier refuses a stranger with for the same rule:
    /// this verb serves this machine only.
    inline constexpr Cc::SurfaceRefusal CordonNotLocal {
        .code = CompileCacheWire::ErrorCode::NotAMember,
        .counter = IMetricsSink::Counter::WorkerCordonsRefusedNotLocal,
    };
    /// A cordon whose one-byte payload would not decode.
    ///
    /// The worker's undecodable-payload series, which is what `Cc::WorkerProtocol` counts a
    /// COMPILE that would not decode under: the refusal is the same one, on another verb of
    /// the same surface.
    inline constexpr Cc::SurfaceRefusal MalformedPayload {
        .code = CompileCacheWire::ErrorCode::MalformedFrame,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedPayload,
    };
    /// A pre-payload decision naming a verb this build has no row for.
    inline constexpr Cc::SurfaceRefusal UnknownOpcode {
        .code = CompileCacheWire::ErrorCode::UnknownOpcode,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnknownOpcode,
    };
    /// An `AUTH` payload that would not decode.
    ///
    /// Unreachable on this surface for the reason `Unauthenticated` below is:
    /// `MergedResponder` routes the `Session` family to the scheduler, which owns the
    /// credential. It carries a row anyway, so that a shape in which a compile surface
    /// did check one cannot answer on the wire while nothing rises -- which is the
    /// whole of #327 and, on the merged listener, of #447.
    ///
    /// Its OWN counter rather than the undecodable-payload one, although both answer
    /// `MalformedFrame`. That is the rulebook's load-bearing clause -- the row is the
    /// refusal, not the code -- and a dead row is exactly where it is easiest to get
    /// wrong: nothing would ever have shown the two summed.
    inline constexpr Cc::SurfaceRefusal MalformedCredential {
        .code = CompileCacheWire::ErrorCode::MalformedFrame,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedCredential,
    };
    /// An `AUTH` payload that decoded and did not verify.
    ///
    /// Its own counter rather than `Unauthenticated` below, for the reason
    /// `MalformedCredential` above has one: three refusals answer `unauthenticated` or
    /// `malformed-frame` across this worker and an operator acts on each differently.
    /// Unreachable here today, which is exactly where the split is easiest to forget.
    inline constexpr Cc::SurfaceRefusal RejectedCredential {
        .code = CompileCacheWire::ErrorCode::Unauthenticated,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedRejectedCredential,
    };
    /// A compile verb reached before a credential. Zero on every shipped shape.
    inline constexpr Cc::SurfaceRefusal Unauthenticated {
        .code = CompileCacheWire::ErrorCode::Unauthenticated,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnauthenticated,
    };
} // namespace CompileRefusal

/// What this surface answers a caller with no claim on its CPU.
///
/// The row and the sentence; the DECISION is `Node::RefuseUnlessMember`
/// (`MembershipGate.hpp`), which every surface on this node shares. Kept beside the
/// other compile rows rather than inlined at the call site because there is more than
/// one door into this worker -- `WorkerServer`'s accept loop and, since #290,
/// `CompileResponder` on the merged `0xFC` listener -- and two doors spelling the
/// anti-leeching refusal by hand would be two policies that agree today, on a question
/// whose wrong answer is "this machine ran a stranger's compiler for them".
///
/// It is a *reply* rather than a close, so a misconfigured peer learns which of the two
/// it is instead of seeing a connection it cannot tell from a dead host. And it is
/// decided on the peer's HOST alone, which is what lets both doors ask before a payload
/// byte is read: a caller with no claim here must not be able to make this process
/// buffer a multi-megabyte preprocessed translation unit on the way to being refused.
inline constexpr std::string_view NotAMemberWhy = "this worker compiles for its own machine and its cluster";

} // namespace FastCache::Node

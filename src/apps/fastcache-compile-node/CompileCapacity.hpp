// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>

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

} // namespace FastCache::Node

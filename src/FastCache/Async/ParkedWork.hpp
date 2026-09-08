// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <coroutine>
#include <utility>

namespace FastCache
{

/// A coroutine parked on an executor: the handle to resume, and -- only when this
/// chain belongs to nobody -- the frame that executor may free if it never resumes it.
///
/// **The two halves are different questions and the second one has a default that is
/// safe.** `IExecutor::Submit` and `IReactor::Schedule` BORROW: the contract on both
/// is "resume this", and the caller guarantees the frame stays alive until they do.
/// That is why a reactor may not simply destroy what it holds at teardown --
/// `ExpiryReaper`, `RaftPeerTransport` and a dozen tests park a handle whose frame a
/// live `Task` object owns, and destroying it there is a double free rather than a
/// leak fixed. Measured on `TestReactor::Stop short-circuits the loop`, which parks a
/// `Task` local, never starts it, and destroys it before the reactor.
///
/// So `abandon` is the caller's statement that nothing else can free this chain,
/// and it is derived rather than decided: `Detail::ParkedWorkFor` fills it in exactly
/// when the await chain bottoms out in a `DetachedTask`, which is the one coroutine
/// shape in this tree that no object owns. Empty means *somebody else owns this* and
/// is what every direct `Submit(handle)` / `Schedule(deadline, handle)` call gets.
///
/// **It is the ROOT, never the parked frame itself, and that distinction is the whole
/// fix.** Destroying the parked frame runs its own destructors and stops there: the
/// caller waiting on it through a `Task::Awaiter` is left holding a dangling handle
/// and is itself unreachable, so the chain goes on leaking -- measured on
/// [#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)'s own shape,
/// where destroying the innermost frame took a four-allocation leak to three and
/// LeakSanitizer stayed red. Destroying the ROOT frees all of it, because ownership
/// in a `Task` chain runs downward: each frame's `Awaiter` owns the frame it awaits.
struct ParkedWork
{
    std::coroutine_handle<> resume {};  ///< The coroutine to resume. Never owned by the executor.
    std::coroutine_handle<> abandon {}; ///< The chain root to free if it is never resumed; empty when owned elsewhere.
};

namespace Detail
{

    /// One entry in an executor's parked-work container, owning `abandon` for as long
    /// as it sits there.
    ///
    /// **The ownership is folded into the operation rather than called beside it**,
    /// which is `ReadSlot.hpp`'s rule about which shape a guard should take: `Resume()`
    /// releases and resumes in one expression, so there is no line an arm site can
    /// forget the release on, and a container that is simply cleared -- at teardown, or
    /// by the vector's own destructor -- frees exactly the chains nobody else can.
    class Parked
    {
      public:
        Parked() noexcept = default;

        /// @param work The handle to resume and the chain root to free if it is not.
        explicit Parked(ParkedWork work) noexcept:
            _work { work }
        {
        }

        Parked(Parked const&) = delete;
        Parked& operator=(Parked const&) = delete;

        Parked(Parked&& other) noexcept:
            _work { std::exchange(other._work, ParkedWork {}) }
        {
        }

        Parked& operator=(Parked&& other) noexcept
        {
            if (this != &other)
            {
                Abandon();
                _work = std::exchange(other._work, ParkedWork {});
            }
            return *this;
        }

        ~Parked()
        {
            Abandon();
        }

        /// @return The handle this entry would resume; null once it has been taken.
        [[nodiscard]] std::coroutine_handle<> Handle() const noexcept
        {
            return _work.resume;
        }

        /// Resume the parked coroutine, giving the chain back to whoever it belongs to.
        ///
        /// Disowns first and resumes last, so a body that runs to completion and frees
        /// its own frame cannot be freed a second time by this entry going out of scope.
        ///
        /// **A handle that cannot be resumed is FREED here, not dropped.** Taking the
        /// work out and then declining to resume it would discard an owned chain root
        /// without destroying it -- a silent leak on the one path this whole type exists
        /// to close. Nothing reaches that branch through a reactor today, because a
        /// chain whose `abandon` is set is owned by nobody and so cannot have been
        /// resumed to completion by anything else; the branch is written for the
        /// contract rather than for a caller that exists, and the contract is *resumed
        /// or freed, never neither*.
        void Resume()
        {
            auto const work = std::exchange(_work, ParkedWork {});
            if (work.resume && !work.resume.done())
            {
                work.resume.resume();
                return;
            }
            if (work.abandon)
                work.abandon.destroy();
        }

        /// Hand the parked work to a caller taking it off this executor.
        ///
        /// `IReactor::CancelPending`'s contract: after this the caller is the only one
        /// who may resume or destroy it, so this entry must do neither.
        /// @return What was parked here; empty afterwards.
        [[nodiscard]] ParkedWork Take() noexcept
        {
            return std::exchange(_work, ParkedWork {});
        }

      private:
        /// Free the chain root, if this entry still holds one.
        void Abandon() noexcept
        {
            auto const work = std::exchange(_work, ParkedWork {});
            if (work.abandon)
                work.abandon.destroy();
        }

        ParkedWork _work {};
    };

} // namespace Detail

} // namespace FastCache

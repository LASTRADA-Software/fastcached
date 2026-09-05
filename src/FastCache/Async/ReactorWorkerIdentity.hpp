// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <thread>

namespace FastCache
{

/// Who is inside a reactor's `Run()`, if anyone.
///
/// **Two facts, held together because neither one answers anything alone**
/// ([#668](https://github.com/LASTRADA-Software/fastcached/issues/668)): whether a
/// thread is currently dequeuing for this reactor, and which thread that is. A
/// caller about to destroy something the reactor owns needs both, and needs them as
/// one observation -- "is a worker running" and "am I it" read separately can
/// straddle the moment `Run()` returns.
///
/// **Two variables rather than one**, and that is not redundancy: a
/// default-constructed `std::thread::id` is a legal value to compare against, so a
/// single id field cannot tell "nobody is running" from "some thread whose id
/// happens to compare equal". `IocpReactor` already carried exactly this pair with
/// exactly this reasoning; this type is that pair given a name so the other three
/// reactors do not each grow their own copy of it, which is how the two spellings
/// that matter drift apart.
class ReactorWorkerIdentity
{
  public:
    /// Marks a thread as this reactor's worker for the scope of its `Run()`.
    ///
    /// RAII rather than a matched pair of calls, for the reason every other guard in
    /// this tree is: `Run()` has more than one way out -- the loop condition, a
    /// `break`, and on some implementations a throw from a resumed coroutine -- and
    /// the exit that gets forgotten leaves the reactor claiming a worker that has
    /// gone, which is the FALSE-SAFE direction. A teardown would then be told it was
    /// on the worker thread when no thread is dequeuing at all.
    class Scope
    {
      public:
        /// @param identity The identity to claim for the calling thread.
        explicit Scope(ReactorWorkerIdentity& identity) noexcept:
            _identity { identity }
        {
            _identity._workerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
            _identity._running.store(true, std::memory_order_release);
        }

        ~Scope()
        {
            _identity._running.store(false, std::memory_order_release);
        }

        Scope(Scope const&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope const&) = delete;
        Scope& operator=(Scope&&) = delete;

      private:
        ReactorWorkerIdentity& _identity;
    };

    /// Whether a thread is currently inside `Run()`.
    ///
    /// False before `Run()` is entered and after it returns, which is the honest
    /// answer: with nothing dequeuing there is no worker thread to be on. A caller
    /// that legitimately tears down outside a running reactor asks this first rather
    /// than reading `IsOnWorkerThread()`'s false as a violation.
    /// @return True between entry to and return from `Run()`.
    [[nodiscard]] bool Running() const noexcept
    {
        return _running.load(std::memory_order_acquire);
    }

    /// @return True when the calling thread is the one currently inside `Run()`.
    [[nodiscard]] bool IsOnWorkerThread() const noexcept
    {
        return _running.load(std::memory_order_acquire)
               && _workerThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

  private:
    std::atomic<std::thread::id> _workerThread {};
    std::atomic<bool> _running { false };
};

} // namespace FastCache

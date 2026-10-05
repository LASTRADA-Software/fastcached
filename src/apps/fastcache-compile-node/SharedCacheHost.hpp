// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace FastCache::Node
{

// Declared, not included: the host holds tiers and asks an opener for them, and a caller that only
// tells it what the cluster applied -- the consensus tier -- has no use for a store's definition.
class ISharedTierOpener;
class SharedCacheTier;

/// Where `SharedCacheHost` reconciles. **PRIVATE: in-process only**, never transmitted or
/// persisted, so it carries no values.
enum class ReconcileOn : std::uint8_t
{
    OwnThread, ///< A thread of the host's own, woken at every apply and on `RetiredSweepInterval`.
    Caller,    ///< Nobody but a caller of `Reconcile()` -- a test, so no case waits on a thread.
};

/// What the host last reconciled to.
struct SharedCacheHostStatus
{
    bool named { false };    ///< The applied state names this machine as the fleet's shared cache.
    bool serving { false };  ///< A tier is published.
    std::string unavailable; ///< Why the tier will not open, while `named` and not `serving`; empty otherwise.

    /// Field by field.
    /// @param other The status to compare with.
    /// @return True when both say the same.
    [[nodiscard]] bool operator==(SharedCacheHostStatus const& other) const = default;
};

/// How long a retired tier whose last answer has finished may stay open with no apply to wake the
/// reconciler.
///
/// The only timer here, and it bounds how long a moved-away store stays claimed -- not
/// correctness: a tier is freed only once nothing but the host holds it, whenever that is asked.
inline constexpr std::chrono::seconds RetiredSweepInterval { 30 };

/// What `~SharedCacheHost` does when something still borrows the host it is destroying.
///
/// A seam so the refusal is reachable by a test: production ends the process, and a case supplies
/// one that records the count and RETURNS -- after which the host tears down as usual, so such a
/// case must not call through the borrower it left alive.
class IOutlivedHostRefusal
{
  public:
    IOutlivedHostRefusal() = default;
    IOutlivedHostRefusal(IOutlivedHostRefusal const&) = delete;
    IOutlivedHostRefusal(IOutlivedHostRefusal&&) = delete;
    IOutlivedHostRefusal& operator=(IOutlivedHostRefusal const&) = delete;
    IOutlivedHostRefusal& operator=(IOutlivedHostRefusal&&) = delete;
    virtual ~IOutlivedHostRefusal() = default;

    /// Refuse to destroy a host that is still borrowed. Production does not return.
    /// @param borrowers How many `SharedCacheHost::Borrow`s were alive; never zero.
    virtual void Refuse(std::size_t borrowers) noexcept = 0;
};

/// The line the production refusal prints: the count, what outlived what, and the remedy.
/// @param borrowers How many borrowers were alive.
/// @return The line, newline-terminated.
[[nodiscard]] std::string OutlivedHostMessage(std::size_t borrowers);

/// Production `IOutlivedHostRefusal`: prints `OutlivedHostMessage` on stderr, then aborts.
///
/// `std::abort` rather than `std::_Exit`: this is an ownership ORDER got wrong in `main`, a
/// programmer error, so a crash dump is worth more than a clean status.
class AbortOnOutlivedHost final: public IOutlivedHostRefusal
{
  public:
    [[noreturn]] void Refuse(std::size_t borrowers) noexcept override;
};

/// Process-singleton `AbortOnOutlivedHost`. Mirrors `DefaultDrainAbandonment()`; tests pass
/// their own.
/// @return Reference to a singleton with static storage.
[[nodiscard]] IOutlivedHostRefusal& DefaultOutlivedHostRefusal() noexcept;

/// Decides, at every applied state, whether this machine serves the fleet's shared tier, and
/// keeps the published tier in step with that.
///
/// **Three lifetimes, and each is shaped for a reason:**
///
/// - **Opening is off the apply callback.** `Applied` records the desire and wakes the reconciler;
///   `CowTreeStorage::Open` may walk the store to build its key index, and on the consensus apply
///   callback that would be a voter missing heartbeats.
/// - **Closing is off the reactor.** An answer holds a `std::shared_ptr` for its whole length, so
///   unpublishing moves the host's pointer to a retired list rather than dropping it, and
///   `Reconcile()` frees a retired tier only once the host holds the last reference. Nothing can
///   take a new reference to a pointer that is no longer published, so that count only falls; the
///   store's final flush and its file unlock then run on the reconciling thread, never on the
///   reactor that finished the last answer.
/// - **Naming this machine again revives a retired tier** rather than opening the file a second
///   time, which the store's exclusive claim would refuse `InUse` -- against this very process.
class SharedCacheHost
{
  public:
    /// @param selfId This node's id, which the `shared-cache` setting names when it names this machine.
    /// @param opener Opens the tier; must outlive this.
    /// @param conditions Where `SharedCacheUnavailable` is raised and cleared, or nullptr on a node
    ///        that runs no consensus, whose conditions answer that row from its scope.
    /// @param logger Where a transition is reported, once.
    /// @param on Where reconciliation runs.
    /// @param refusal What the destructor does while something still borrows this host.
    SharedCacheHost(std::string selfId,
                    ISharedTierOpener& opener,
                    NodeConditions* conditions,
                    ILogger& logger,
                    ReconcileOn on,
                    IOutlivedHostRefusal& refusal = DefaultOutlivedHostRefusal());

    SharedCacheHost(SharedCacheHost const&) = delete;
    SharedCacheHost& operator=(SharedCacheHost const&) = delete;
    SharedCacheHost(SharedCacheHost&&) = delete;
    SharedCacheHost& operator=(SharedCacheHost&&) = delete;

    /// Stops and joins the reconciler, then frees every tier -- at shutdown, after the listener
    /// that answered from them is gone.
    ///
    /// **Ends the process instead while anything still `Borrow`s this host**, through the injected
    /// `IOutlivedHostRefusal`. A borrower reads the
    /// published tier through a reference to this object, so one outliving it would read freed
    /// memory -- and one with a call in flight would hold the tier's last reference, freeing the
    /// store and running its final flush on whatever thread that call finishes on. That is an
    /// ORDER in whoever owns both, so it is checked here, where every order reaches, and said by
    /// name rather than left to a crash that names nothing.
    ~SharedCacheHost();

    /// A reader's claim on this host, held for as long as the reader may call `Current()`.
    ///
    /// Folded into the reader's own construction -- `InProcessSharedUpstream` holds one -- so the
    /// order it depends on is checked by the host's destructor rather than remembered by the owner
    /// of both. Neither copied nor moved: a claim belongs to the object that made it.
    ///
    /// The count it moves is SHARED with the host rather than a member of it, so a claim released
    /// after a refusal that returned touches nothing freed.
    class Borrow
    {
      public:
        /// @param host The host borrowed; the claim is released when this is destroyed.
        explicit Borrow(SharedCacheHost const& host) noexcept;

        Borrow(Borrow const&) = delete;
        Borrow& operator=(Borrow const&) = delete;
        Borrow(Borrow&&) = delete;
        Borrow& operator=(Borrow&&) = delete;

        /// Releases the claim.
        ~Borrow();

        /// @return The host borrowed.
        [[nodiscard]] SharedCacheHost const& Host() const noexcept
        {
            return _host;
        }

      private:
        SharedCacheHost const& _host;
        std::shared_ptr<std::atomic<std::size_t>> _count;
    };

    /// @return How many `Borrow`s are alive. For tests.
    [[nodiscard]] std::size_t Borrowers() const noexcept;

    /// Record what @p state asks of this machine and wake the reconciler. Any thread; never opens.
    /// @param state The state the cluster just applied.
    void Applied(Cluster::ClusterState const& state);

    /// Reconcile once: open, revive or retire the tier, and free every retired tier nothing else holds.
    ///
    /// On the reconciler thread, or on a test's.
    void Reconcile();

    /// The published tier, held for as long as the caller needs it.
    /// @return The tier, or null while this machine is not serving.
    [[nodiscard]] std::shared_ptr<SharedCacheTier> Current() const;

    /// What the last reconciliation came to.
    /// @return The status.
    [[nodiscard]] SharedCacheHostStatus Status() const;

  private:
    /// The reconciler thread's loop.
    /// @param stop Asked by the destructor.
    void Run(std::stop_token const& stop);

    /// Log a transition once, and raise or clear `SharedCacheUnavailable`.
    /// @param status What the reconciliation came to.
    void Report(SharedCacheHostStatus const& status);

    std::string _selfId;
    ISharedTierOpener& _opener;
    NodeConditions* _conditions;
    ILogger& _logger;
    IOutlivedHostRefusal& _refusal;

    mutable std::mutex _mutex;
    /// `_any` for the `std::stop_token` overload of `wait_for`: a drain waits on a condition
    /// variable, never on `atomic::wait`.
    std::condition_variable_any _wake;
    bool _wantServing { false };                            ///< Guarded by `_mutex`.
    bool _dirty { false };                                  ///< Guarded by `_mutex`.
    std::shared_ptr<SharedCacheTier> _current;              ///< Guarded by `_mutex`.
    std::vector<std::shared_ptr<SharedCacheTier>> _retired; ///< Guarded by `_mutex`.
    SharedCacheHostStatus _status;                          ///< Guarded by `_mutex`.

    /// What `Report` last said. Touched only by whoever reconciles, which is one thread at a time.
    std::optional<SharedCacheHostStatus> _reported;

    /// How many `Borrow`s are alive; the destructor refuses to run past a non-zero count. Shared
    /// with each `Borrow`, so one released after a returning refusal decrements live memory.
    std::shared_ptr<std::atomic<std::size_t>> _borrowers { std::make_shared<std::atomic<std::size_t>>(0) };

    /// Declared last, so it is the first member destroyed -- and the destructor stops it before
    /// then anyway.
    std::jthread _reconciler;
};

} // namespace FastCache::Node

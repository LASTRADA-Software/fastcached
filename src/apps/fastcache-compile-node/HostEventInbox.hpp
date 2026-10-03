// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/HostEvents.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

namespace FastCache::Node
{

/// What the worker's heartbeat does about a host event.
///
/// **Private**: never transmitted or persisted. The enumerator ORDER is the priority: an earlier
/// action pending outranks a later one in `HostEventInbox::Take`.
enum class HostEventAction : std::uint8_t
{
    WithdrawNow, ///< Retire every registration before the machine sleeps.
    AnnounceNow, ///< Run the next round now rather than at the end of the interval.
    Last
};

/// One host event and what this node does about it.
struct HostEventActionRow
{
    HostEvent event {};           ///< Which event; its own index.
    HostEventAction heartbeat {}; ///< What the worker heartbeat does.
    bool wakesPresence {};        ///< Whether the presence round runs at once as well.
    /// Whether it SUPERSEDES every wake still pending, in both loops: a suspend. Deliveries are
    /// serialised by the hub, so each pending wake is older than it, and a round for one after it
    /// would announce a machine about to sleep. Costs at most one interval on a wake the host
    /// never reports -- closed, nothing is leased meanwhile, and healed by the next ordinary round.
    bool supersedesOlderWakes {};
    std::string_view why; ///< The reason, for the reader of the table.
};

/// @param event A host event.
/// @return Its row.
[[nodiscard]] HostEventActionRow const& HostEventActionFor(HostEvent event) noexcept;

/// How long a suspend may hold the machine, withdrawal included. Windows allows about two
/// seconds for a suspend notification; this leaves the rest to everybody else.
inline constexpr std::chrono::milliseconds SuspendWithdrawBudget { 1'500 };

/// Where host events wait for the heartbeat thread, and how a suspend waits for its withdrawal.
///
/// A sink on the host's thread (the SCM's control handler) posting to the heartbeat thread,
/// because the registrars are the heartbeat's alone. A suspend then WAITS, bounded by
/// `SuspendWithdrawBudget` and measured through the injected `IDrainWait`, for the heartbeat to
/// say it has withdrawn -- and returns when the budget is spent whatever the heartbeat is doing.
/// A withdrawal the heartbeat had not TAKEN by then is dropped rather than left pending: it could
/// only run once the machine is awake again.
///
/// **Only a suspend blocks.** A resume or a network change sets its action and wakes the
/// heartbeat, and the round -- the dialling -- runs on the heartbeat's thread, never on the one
/// that delivered (`IHostEventSink`'s contract).
class HostEventInbox final: public IHostEventSink
{
  public:
    /// @param wake Ends the heartbeat's wait (`CompileCapacity::WakeHeartbeat`).
    /// @param wait Where a suspend's bounded wait blocks and reads time.
    HostEventInbox(std::function<void()> wake, IDrainWait& wait);

    /// @copydoc IHostEventSink::OnHostEvent
    void OnHostEvent(HostEvent event) override;

    /// The highest-priority action pending, cleared as it is taken.
    /// @return The action, or nullopt when nothing is pending.
    [[nodiscard]] std::optional<HostEventAction> Take();

    /// @return Whether any action is pending.
    [[nodiscard]] bool HasPending() const;

    /// The withdrawal last taken has been attempted; the suspend waiting for it may return.
    void Settle();

    /// @return How the last suspend's wait ended, or nullopt before the first.
    [[nodiscard]] std::optional<DrainResult> LastSuspendWait() const;

  private:
    std::function<void()> _wake;                  ///< Ends the heartbeat's wait; called with no lock held.
    IDrainWait& _wait;                            ///< Where a suspend's bounded wait blocks, and its clock.
    mutable std::mutex _mutex;                    ///< Guards every member below except `_settled`'s reads.
    EnumTable<HostEventAction, bool> _pending {}; ///< What the heartbeat owes, by action. Under `_mutex`.
    std::uint64_t _posted { 0 };                  ///< Suspends posted so far; a suspend's ticket. Under `_mutex`.
    std::uint64_t _taken { 0 };                   ///< The newest suspend ticket the heartbeat has taken. Under `_mutex`.
    /// The newest ticket the heartbeat has settled. WRITTEN under `_mutex`, READ without it by the
    /// suspend's drain predicate on the delivering thread -- which is why it is atomic.
    std::atomic<std::uint64_t> _settled { 0 };
    std::optional<DrainResult> _lastSuspendWait; ///< How the last suspend's wait ended. Under `_mutex`.
};

} // namespace FastCache::Node

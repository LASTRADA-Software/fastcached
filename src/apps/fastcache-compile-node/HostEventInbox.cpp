// SPDX-License-Identifier: Apache-2.0
#include "HostEventInbox.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace FastCache::Node
{

namespace
{
    constexpr auto ActionRows = EnumTable<HostEvent, HostEventActionRow> { {
        { .event = HostEvent::Suspending,
          .heartbeat = HostEventAction::WithdrawNow,
          .wakesPresence = false,
          .supersedesOlderWakes = true,
          .why = "a sleeping machine must not be leased; its presence row expires on its own" },
        { .event = HostEvent::Resumed,
          .heartbeat = HostEventAction::AnnounceNow,
          .wakesPresence = true,
          .supersedesOlderWakes = false,
          .why = "re-register and re-adopt the roster at once rather than an interval later" },
        { .event = HostEvent::NetworkChanged,
          .heartbeat = HostEventAction::AnnounceNow,
          .wakesPresence = true,
          .supersedesOlderWakes = false,
          .why = "an address may have moved; say where this machine is now" },
    } };

    static_assert(RowsInEnumeratorOrder(ActionRows, [](HostEventActionRow const& row) { return row.event; }),
                  "every HostEvent needs a row, at its own index");
} // namespace

HostEventActionRow const& HostEventActionFor(HostEvent event) noexcept
{
    return ActionRows[static_cast<std::size_t>(event)];
}

HostEventInbox::HostEventInbox(std::function<void()> wake, IDrainWait& wait):
    _wake { std::move(wake) },
    _wait { wait }
{
}

void HostEventInbox::OnHostEvent(HostEvent event)
{
    auto const& row = HostEventActionFor(event);
    auto const action = row.heartbeat;
    auto ticket = std::uint64_t { 0 };
    {
        std::scoped_lock const lock { _mutex };
        // An announcement still pending is older than a suspend, and superseded by it
        // (`HostEventActionRow::supersedesOlderWakes`); one posted AFTER the suspend is owed as ever.
        if (row.supersedesOlderWakes)
            _pending[static_cast<std::size_t>(HostEventAction::AnnounceNow)] = false;
        _pending[static_cast<std::size_t>(action)] = true;
        if (action == HostEventAction::WithdrawNow)
            ticket = ++_posted;
    }
    _wake();
    if (action != HostEventAction::WithdrawNow)
        return;

    auto const waited =
        DrainWithin([this, ticket] { return _settled.load(std::memory_order_acquire) < ticket; },
                    DrainBound { .ceiling = SuspendWithdrawBudget, .poll = std::chrono::milliseconds { 10 } },
                    _wait);
    std::scoped_lock const lock { _mutex };
    _lastSuspendWait = waited;
    // Returning is what lets the machine sleep, so a withdrawal nobody has TAKEN yet could only
    // run once it is awake again: withdrawing a machine that is serving, and leaving it filed
    // nowhere until the round after. Dropped; the scheduler's expiry covers the sleep, as it did
    // before any of this existed. One already taken is the heartbeat's to finish, and `Take`
    // cleared its flag, so this clears nothing of it.
    if (waited == DrainResult::Ceiling)
        _pending[static_cast<std::size_t>(HostEventAction::WithdrawNow)] = false;
}

std::optional<HostEventAction> HostEventInbox::Take()
{
    std::scoped_lock const lock { _mutex };
    for (auto const action: Enumerators<HostEventAction>())
    {
        auto& pending = _pending[static_cast<std::size_t>(action)];
        if (!pending)
            continue;
        pending = false;
        if (action == HostEventAction::WithdrawNow)
            _taken = _posted;
        return action;
    }
    return std::nullopt;
}

bool HostEventInbox::HasPending() const
{
    std::scoped_lock const lock { _mutex };
    return std::ranges::any_of(_pending, std::identity {});
}

void HostEventInbox::Settle()
{
    std::scoped_lock const lock { _mutex };
    _settled.store(_taken, std::memory_order_release);
}

std::optional<DrainResult> HostEventInbox::LastSuspendWait() const
{
    std::scoped_lock const lock { _mutex };
    return _lastSuspendWait;
}

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#include "HostEventInbox.hpp"

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Platform/HostEvents.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

#include <core/platform/Clock.hpp>
#include <tests/SteppedDrainWait.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using Testing::SteppedDrainWait;

namespace
{

/// What Windows allows a suspend notification before it sleeps anyway -- the HOST's number, written
/// here independently of `SuspendWithdrawBudget` so a budget that outgrew it fails a case rather than
/// moving every bound that is read relative to it.
constexpr auto HostSuspendAllowance = std::chrono::seconds { 2 };

} // namespace

TEST_CASE("Each host event is one row saying what the heartbeat and the presence round do", "[node][host-events]")
{
    CHECK(HostEventActionFor(HostEvent::Suspending).heartbeat == HostEventAction::WithdrawNow);
    CHECK_FALSE(HostEventActionFor(HostEvent::Suspending).wakesPresence);
    CHECK(HostEventActionFor(HostEvent::Resumed).heartbeat == HostEventAction::AnnounceNow);
    CHECK(HostEventActionFor(HostEvent::Resumed).wakesPresence);
    CHECK(HostEventActionFor(HostEvent::NetworkChanged).heartbeat == HostEventAction::AnnounceNow);
    CHECK(HostEventActionFor(HostEvent::NetworkChanged).wakesPresence);
    // A suspend supersedes every wake posted before it, in both loops; nothing else supersedes.
    CHECK(HostEventActionFor(HostEvent::Suspending).supersedesOlderWakes);
    CHECK_FALSE(HostEventActionFor(HostEvent::Resumed).supersedesOlderWakes);
    CHECK_FALSE(HostEventActionFor(HostEvent::NetworkChanged).supersedesOlderWakes);
}

TEST_CASE("A suspend nobody settles gives the machine back once its budget is spent", "[node][host-events]")
{
    // A suspend must never hold the machine past its budget, whatever the heartbeat is doing: it
    // may be stuck in a ten-second exchange, or not running yet.
    auto wakes = 0;
    SteppedDrainWait wait;
    HostEventInbox inbox { [&wakes] { ++wakes; }, wait };

    inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(wakes == 1);
    CHECK(inbox.LastSuspendWait() == std::optional { DrainResult::Ceiling });
    CHECK(wait.Elapsed() >= SuspendWithdrawBudget);
    CHECK(wait.Elapsed() < SuspendWithdrawBudget + 20ms);
    // And inside what the host allows, or the machine sleeps with the handler still running.
    CHECK(wait.Elapsed() < HostSuspendAllowance);
}

TEST_CASE("A suspend the heartbeat settles ends as soon as it is settled", "[node][host-events]")
{
    HostEventInbox* inboxSeen = nullptr;
    SteppedDrainWait wait { [&inboxSeen] {
        if (inboxSeen != nullptr && inboxSeen->Take() == std::optional { HostEventAction::WithdrawNow })
            inboxSeen->Settle();
    } };
    HostEventInbox inbox { [] {}, wait };
    inboxSeen = &inbox;

    inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(inbox.LastSuspendWait() == std::optional { DrainResult::Drained });
    CHECK(wait.Sleeps() == 1);
}

TEST_CASE("A resume or a network change wakes the heartbeat and never waits", "[node][host-events]")
{
    auto wakes = 0;
    SteppedDrainWait wait;
    HostEventInbox inbox { [&wakes] { ++wakes; }, wait };

    inbox.OnHostEvent(HostEvent::Resumed);
    inbox.OnHostEvent(HostEvent::NetworkChanged);

    CHECK(wakes == 2);
    CHECK(wait.Sleeps() == 0);
    CHECK(inbox.HasPending());
    CHECK(inbox.Take() == std::optional { HostEventAction::AnnounceNow });
    // Coalesced: two announcements asked for are one round owed.
    CHECK_FALSE(inbox.Take().has_value());
    CHECK_FALSE(inbox.HasPending());
}

namespace
{

/// An inbox whose wait plays the heartbeat inside a suspend: the first poll takes one action and
/// settles, as `AwaitNextRound` does on its own thread.
struct SettlingInbox
{
    std::optional<HostEventAction> firstTaken; ///< What the heartbeat took, once.
    SteppedDrainWait wait { [this] {
        if (firstTaken.has_value())
            return;
        firstTaken = inbox.Take();
        inbox.Settle();
    } };
    HostEventInbox inbox { [] {}, wait }; ///< The inbox under test.
};

} // namespace

TEST_CASE("A suspend supersedes every announcement posted before it", "[node][host-events]")
{
    // Deliveries are serialised by the hub, so an announcement still pending when a suspend is
    // posted is OLDER than the suspend: running it after the withdrawal would re-register a machine
    // about to sleep. The cost is at most one interval on a wake the host never reports, and it
    // fails closed: nothing is leased, and the next ordinary round registers.
    SettlingInbox rig;

    rig.inbox.OnHostEvent(HostEvent::Resumed);
    rig.inbox.OnHostEvent(HostEvent::NetworkChanged);
    rig.inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(rig.firstTaken == std::optional { HostEventAction::WithdrawNow });
    CHECK_FALSE(rig.inbox.Take().has_value());
}

TEST_CASE("An announcement posted after a suspend is still owed", "[node][host-events]")
{
    // The control for the case above: only what the suspend outdated is dropped.
    SettlingInbox rig;

    rig.inbox.OnHostEvent(HostEvent::Suspending);
    REQUIRE(rig.firstTaken == std::optional { HostEventAction::WithdrawNow });
    rig.inbox.OnHostEvent(HostEvent::Resumed);

    CHECK(rig.inbox.Take() == std::optional { HostEventAction::AnnounceNow });
}

TEST_CASE("A suspend whose budget ran out leaves no withdrawal behind for the machine to make awake", "[node][host-events]")
{
    // The handler returning is what lets the machine sleep, so a withdrawal the heartbeat had not
    // TAKEN by then could only run after the machine wakes -- withdrawing an awake machine and
    // then waiting a whole interval to register it again. Dropped instead; expiry covers the sleep.
    SteppedDrainWait wait;
    HostEventInbox inbox { [] {}, wait };

    inbox.OnHostEvent(HostEvent::Suspending);

    REQUIRE(inbox.LastSuspendWait() == std::optional { DrainResult::Ceiling });
    CHECK_FALSE(inbox.HasPending());
}

TEST_CASE("A withdrawal the heartbeat took in time is its own, however late it settles", "[node][host-events]")
{
    // The control for the case above: what is dropped is only what nobody took.
    HostEventInbox* inboxSeen = nullptr;
    std::optional<HostEventAction> taken;
    SteppedDrainWait wait { [&] {
        if (inboxSeen != nullptr && !taken.has_value())
            taken = inboxSeen->Take();
    } };
    HostEventInbox inbox { [] {}, wait };
    inboxSeen = &inbox;

    inbox.OnHostEvent(HostEvent::Suspending);

    CHECK(taken == std::optional { HostEventAction::WithdrawNow });
    CHECK(inbox.LastSuspendWait() == std::optional { DrainResult::Ceiling });
    inbox.Settle();
    CHECK_FALSE(inbox.HasPending());

    // And the next suspend waits for ITS withdrawal, not for the late settle of the last one.
    taken.reset();
    auto const before = wait.Elapsed();
    inbox.OnHostEvent(HostEvent::Suspending);
    CHECK(inbox.LastSuspendWait() == std::optional { DrainResult::Ceiling });
    CHECK(wait.Elapsed() - before >= SuspendWithdrawBudget);
}

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostEvents.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ReturnsWithin.hpp>
#include <tests/ScriptedHostEvents.hpp>
#include <tests/Unwrap.hpp>

using FastCache::HostEvent;
using FastCache::Testing::ReturnsWithin;
using FastCache::Testing::WaitHangGuard;
using FastCache::Testing::WaitOptions;
using FastCache::Testing::WaitUntil;
using FastCache::Testing::WaitUntilOutcome;
using namespace std::chrono_literals;

namespace
{

/// Records what it was told, in order.
class RecordingSink final: public FastCache::IHostEventSink
{
  public:
    void OnHostEvent(HostEvent event) override
    {
        heard.push_back(event);
    }

    std::vector<HostEvent> heard; ///< Every event, in order.
};

/// A sink that parks inside its delivery until released, and says when it entered and left.
class ParkingSink final: public FastCache::IHostEventSink
{
  public:
    void OnHostEvent(HostEvent /*event*/) override
    {
        _entered.store(true, std::memory_order_release);
        {
            std::unique_lock lock { _mutex };
            // Bounded, so a case that never releases fails rather than hangs.
            (void) _released.wait_for(lock, WaitHangGuard, [this] { return _open; });
        }
        _left.store(true, std::memory_order_release);
    }

    /// Let the parked delivery leave, and every later one pass straight through.
    void Release()
    {
        {
            std::scoped_lock const lock { _mutex };
            _open = true;
        }
        _released.notify_all();
    }

    /// @return What the sink has done so far, in words.
    [[nodiscard]] std::string Describe() const
    {
        return std::format("entered={} left={}", Entered(), Left());
    }

    /// @return Whether a delivery reached the sink.
    [[nodiscard]] bool Entered() const noexcept
    {
        return _entered.load(std::memory_order_acquire);
    }

    /// @return Whether that delivery has left it.
    [[nodiscard]] bool Left() const noexcept
    {
        return _left.load(std::memory_order_acquire);
    }

  private:
    std::atomic<bool> _entered { false };
    std::atomic<bool> _left { false };
    std::mutex _mutex;
    std::condition_variable _released;
    bool _open { false };
};

/// Releases a `ParkingSink` on the way out, so a failing assertion cannot leave a thread parked
/// in it and turn a red into a hang at the join.
class ReleaseOnExit
{
  public:
    explicit ReleaseOnExit(ParkingSink& sink):
        _sink { sink }
    {
    }
    ~ReleaseOnExit()
    {
        _sink.Release();
    }
    ReleaseOnExit(ReleaseOnExit const&) = delete;
    ReleaseOnExit(ReleaseOnExit&&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit const&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit&&) = delete;

  private:
    ParkingSink& _sink;
};

/// Runs an action from inside its next delivery, once, then records like a `RecordingSink`.
class ActingSink final: public FastCache::IHostEventSink
{
  public:
    void OnHostEvent(HostEvent event) override
    {
        if (auto act = std::exchange(actOnNext, {}))
            act();
        heard.push_back(event);
    }

    std::function<void()> actOnNext; ///< What the next delivery does before it is recorded; then emptied.
    std::vector<HostEvent> heard;    ///< Every event, in order.
};

/// How long a delivery that must not deadlock is given before the case calls it one.
constexpr auto DeliveryEndsWithin = WaitHangGuard;

/// How long a `MeetingSink` delivery waits for a second one to join it inside the sink.
constexpr auto MeetingWindow = 200ms;

/// A sink that, inside each delivery, waits a while for a second delivery to be inside it too, then
/// leaves -- what two producers racing into one sink do. It counts how many were ever inside at once.
class MeetingSink final: public FastCache::IHostEventSink
{
  public:
    /// @param hub The hub the sink leaves.
    explicit MeetingSink(FastCache::HostEventHub& hub):
        _hub { hub }
    {
    }

    void OnHostEvent(HostEvent /*event*/) override
    {
        {
            std::scoped_lock const lock { _mutex };
            ++_inside;
            ++_heard;
            _mostInside = std::max(_mostInside, _inside);
        }
        // Not a Catch2 assertion: this runs on a producer's thread, and the case reads the counts.
        (void) WaitUntilOutcome(
            "a second delivery inside the sink",
            [this] {
                std::scoped_lock const lock { _mutex };
                return _inside >= 2;
            },
            [this] { return Describe(); },
            WaitOptions { .step = {}, .context = {}, .bound = MeetingWindow, .rest = FastCache::Testing::WaitRest });
        _hub.Unsubscribe(*this);
        std::scoped_lock const lock { _mutex };
        --_inside;
    }

    /// @return What the sink has seen so far, in words.
    [[nodiscard]] std::string Describe() const
    {
        std::scoped_lock const lock { _mutex };
        return std::format("inside={} most={} heard={}", _inside, _mostInside, _heard);
    }

    /// @return The most deliveries ever inside the sink at once.
    [[nodiscard]] int MostInside() const
    {
        std::scoped_lock const lock { _mutex };
        return _mostInside;
    }

    /// @return How many deliveries reached the sink.
    [[nodiscard]] int Heard() const
    {
        std::scoped_lock const lock { _mutex };
        return _heard;
    }

  private:
    FastCache::HostEventHub& _hub;
    mutable std::mutex _mutex;
    int _inside { 0 };
    int _mostInside { 0 };
    int _heard { 0 };
};

/// An instant @p offset after an arbitrary origin.
[[nodiscard]] core::platform::SteadyTimePoint At(std::chrono::milliseconds offset)
{
    return core::platform::SteadyTimePoint {} + offset;
}

} // namespace

TEST_CASE("The hub tells every subscriber, and nobody who has left", "[platform][host-events]")
{
    FastCache::HostEventHub hub;
    RecordingSink first;
    RecordingSink second;
    {
        FastCache::HostEventSubscription const one { hub, first };
        FastCache::HostEventSubscription const two { hub, second };
        CHECK(hub.SubscriberCount() == 2);
        hub.OnHostEvent(HostEvent::Resumed);
    }
    CHECK(hub.SubscriberCount() == 0);
    hub.OnHostEvent(HostEvent::NetworkChanged);

    CHECK(first.heard == std::vector { HostEvent::Resumed });
    CHECK(second.heard == std::vector { HostEvent::Resumed });
}

TEST_CASE("Unsubscribe returns only once a delivery in flight to that sink has left it", "[platform][host-events]")
{
    FastCache::HostEventHub hub;
    ParkingSink sink;
    hub.Subscribe(sink);

    std::atomic<bool> returned { false };
    std::atomic<bool> leftBeforeReturn { false };
    std::jthread deliverer;
    std::jthread leaver;
    ReleaseOnExit const release { sink }; // after the threads, so it opens the park before they join

    deliverer = std::jthread { [&hub] { hub.OnHostEvent(HostEvent::Suspending); } };
    REQUIRE(WaitUntil(
        "the delivery to be inside the sink", [&sink] { return sink.Entered(); }, [&sink] { return sink.Describe(); }));

    leaver = std::jthread { [&] {
        hub.Unsubscribe(sink);
        leftBeforeReturn.store(sink.Left());
        returned.store(true);
    } };
    // The one reading a correct hub never produces: Unsubscribe back while the sink still runs.
    auto const early = WaitUntilOutcome(
        "Unsubscribe to return while the delivery is still inside the sink",
        [&returned] { return returned.load(); },
        [&sink] { return sink.Describe(); },
        WaitOptions { .step = {}, .context = {}, .bound = 200ms, .rest = FastCache::Testing::WaitRest });
    CHECK_FALSE(early.reached);

    sink.Release();
    leaver.join();
    deliverer.join();
    CHECK(returned.load());
    CHECK(leftBeforeReturn.load());
}

TEST_CASE("A sink that left during a delivery is not called by it, and one that joined hears the next",
          "[platform][host-events]")
{
    FastCache::HostEventHub hub;
    RecordingSink leaving;
    RecordingSink joining;
    ActingSink first;
    first.actOnNext = [&] {
        hub.Unsubscribe(leaving);
        hub.Subscribe(joining);
    };
    hub.Subscribe(first);
    hub.Subscribe(leaving);

    hub.OnHostEvent(HostEvent::Suspending);
    hub.OnHostEvent(HostEvent::Resumed);
    hub.Unsubscribe(joining);
    hub.Unsubscribe(first);

    CHECK(first.heard == std::vector { HostEvent::Suspending, HostEvent::Resumed });
    CHECK(leaving.heard.empty());
    CHECK(joining.heard == std::vector { HostEvent::Resumed });
}

TEST_CASE("A sink may leave from inside its own delivery", "[platform][host-events]")
{
    // A hub that waited for every delivery to the leaving sink would wait here for itself. Owned
    // by the call, so a delivery that never returns can be left behind.
    struct Scene
    {
        FastCache::HostEventHub hub;
        ActingSink sink;
    };
    auto const scene = std::make_shared<Scene>();
    scene->sink.actOnNext = [&hub = scene->hub, &sink = scene->sink] {
        hub.Unsubscribe(sink);
    };
    scene->hub.Subscribe(scene->sink);

    INFO("the deliveries did not return within DeliveryEndsWithin: the sink leaving itself waited for itself");
    REQUIRE(ReturnsWithin(DeliveryEndsWithin, [scene] {
        scene->hub.OnHostEvent(HostEvent::Suspending);
        scene->hub.OnHostEvent(HostEvent::Resumed);
    }));

    CHECK(scene->sink.heard == std::vector { HostEvent::Suspending });
    CHECK(scene->hub.SubscriberCount() == 0);
}

TEST_CASE("Two producers delivering at once never meet inside a sink, so one leaving from both cannot deadlock",
          "[platform][host-events]")
{
    // A resume on the SCM's handler thread and a network change on the watcher's, released together.
    // Were both inside the sink at once, each would leave, and each Unsubscribe would wait for the
    // OTHER thread's delivery to that sink to end -- for ever. The turn keeps the second producer
    // out until the first has left, and by then the sink is gone.
    struct Scene
    {
        FastCache::HostEventHub hub;
        MeetingSink sink { hub };
        std::latch start { 2 };
    };
    auto const scene = std::make_shared<Scene>();
    scene->hub.Subscribe(scene->sink);

    INFO("the two deliveries did not both return within DeliveryEndsWithin: the sink leaving from both threads "
         "deadlocked");
    REQUIRE(ReturnsWithin(DeliveryEndsWithin, [scene] {
        auto const deliver = [scene](HostEvent event) {
            scene->start.arrive_and_wait();
            scene->hub.OnHostEvent(event);
        };
        std::jthread const resume { deliver, HostEvent::Resumed };
        std::jthread const network { deliver, HostEvent::NetworkChanged };
    }));

    CHECK(scene->sink.MostInside() == 1);
    CHECK(scene->sink.Heard() == 1);
    CHECK(scene->hub.SubscriberCount() == 0);
}

TEST_CASE("The scripted host delivers through the hub and counts its listeners", "[platform][host-events]")
{
    FastCache::Testing::ScriptedHostEvents host;
    RecordingSink sink;
    {
        FastCache::HostEventSubscription const subscription { host, sink };
        CHECK(host.SubscriberCount() == 1);
        host.Fire(HostEvent::Suspending);
    }
    CHECK(host.SubscriberCount() == 0);
    host.Fire(HostEvent::Resumed);

    CHECK(sink.heard == std::vector { HostEvent::Suspending });
}

TEST_CASE("A power broadcast is one row, and one the table does not name is not an event", "[platform][host-events]")
{
    // PBT_APMSUSPEND, PBT_APMRESUMESUSPEND, PBT_APMRESUMEAUTOMATIC, as numbers so this runs
    // everywhere; HostEvents.cpp static_asserts them against <windows.h>.
    CHECK(FastCache::HostEventForPowerBroadcast(0x4) == std::optional { HostEvent::Suspending });
    CHECK(FastCache::HostEventForPowerBroadcast(0x7) == std::optional { HostEvent::Resumed });
    CHECK(FastCache::HostEventForPowerBroadcast(0x12) == std::optional { HostEvent::Resumed });
    // PBT_APMPOWERSTATUSCHANGE: the battery moved, the machine did not.
    CHECK_FALSE(FastCache::HostEventForPowerBroadcast(0xA).has_value());

    // Every row answers for itself, so two rows cannot share a broadcast and disagree.
    for (auto const& row: FastCache::PowerBroadcastTable())
    {
        INFO(row.name);
        CHECK(FastCache::HostEventForPowerBroadcast(row.broadcast) == std::optional { row.event });
    }
}

TEST_CASE("A burst of network changes is one event, at the end of its quiet", "[platform][host-events]")
{
    FastCache::NetworkChangeDebouncer debouncer { FastCache::NetworkDebounce { .quiet = 2'000ms, .ceiling = 10'000ms } };
    CHECK_FALSE(debouncer.DueAt().has_value());

    debouncer.Observe(At(0ms));
    debouncer.Observe(At(500ms));
    debouncer.Observe(At(900ms));
    REQUIRE(debouncer.DueAt().has_value());
    CHECK(debouncer.DueAt() == std::optional { At(2'900ms) });

    CHECK_FALSE(debouncer.TakeIfDue(At(2'899ms)));
    CHECK(debouncer.TakeIfDue(At(2'900ms)));
    // Once: the burst is spent.
    CHECK_FALSE(debouncer.TakeIfDue(At(5'000ms)));
    CHECK_FALSE(debouncer.DueAt().has_value());
}

TEST_CASE("A network that never goes quiet is still reported, at the ceiling", "[platform][host-events]")
{
    FastCache::NetworkChangeDebouncer debouncer { FastCache::NetworkDebounce { .quiet = 2'000ms, .ceiling = 10'000ms } };
    for (auto const offset: { 0ms, 1'500ms, 3'000ms, 4'500ms, 6'000ms, 7'500ms, 9'000ms })
        debouncer.Observe(At(offset));
    CHECK(debouncer.DueAt() == std::optional { At(10'000ms) });
    CHECK(debouncer.TakeIfDue(At(10'000ms)));
}

namespace
{

/// One debounce the table drives: what was observed, and when it is due.
struct DebounceCase
{
    std::string_view name;                           ///< What the row shows.
    std::vector<std::chrono::milliseconds> observed; ///< Raw notifications, in order.
    std::optional<std::chrono::milliseconds> due;    ///< When the burst is due; nullopt: never.
};

} // namespace

TEST_CASE("The debounce is due at the earlier of the quiet and the ceiling", "[platform][host-events]")
{
    auto const bound = FastCache::NetworkDebounce { .quiet = 2'000ms, .ceiling = 10'000ms };
    auto const cases = std::vector<DebounceCase> {
        { .name = "nothing observed is never due", .observed = {}, .due = std::nullopt },
        { .name = "one notification is due a quiet period after it", .observed = { 0ms }, .due = 2'000ms },
        { .name = "a burst is due a quiet period after its last", .observed = { 0ms, 500ms, 900ms }, .due = 2'900ms },
        { .name = "a burst that never goes quiet is due at the ceiling",
          .observed = { 0ms, 1'900ms, 3'800ms, 5'700ms, 7'600ms, 9'500ms },
          .due = 10'000ms },
        { .name = "a burst whose quiet ends at the ceiling is due then", .observed = { 0ms, 8'000ms }, .due = 10'000ms },
        { .name = "a burst that went quiet before the ceiling is due at the quiet",
          .observed = { 0ms, 7'000ms },
          .due = 9'000ms },
    };
    for (auto const& row: cases)
    {
        INFO(row.name);
        FastCache::NetworkChangeDebouncer debouncer { bound };
        for (auto const offset: row.observed)
            debouncer.Observe(At(offset));
        if (!row.due.has_value())
        {
            CHECK_FALSE(debouncer.DueAt().has_value());
            CHECK_FALSE(debouncer.TakeIfDue(At(1h)));
            continue;
        }
        auto const due = At(FastCache::Testing::Unwrap(row.due));
        CHECK(debouncer.DueAt() == std::optional { due });
        CHECK_FALSE(debouncer.TakeIfDue(due - 1ms));
        CHECK(debouncer.TakeIfDue(due));
        CHECK_FALSE(debouncer.DueAt().has_value());
    }
}

TEST_CASE("A burst after a reported one starts its own ceiling", "[platform][host-events]")
{
    FastCache::NetworkChangeDebouncer debouncer { FastCache::NetworkDebounce { .quiet = 2'000ms, .ceiling = 10'000ms } };
    debouncer.Observe(At(0ms));
    REQUIRE(debouncer.TakeIfDue(At(2'000ms)));

    // The second burst never goes quiet either; its ceiling is measured from ITS first
    // notification, not from the burst already reported.
    for (auto const offset: { 3'000ms, 4'500ms, 6'000ms, 7'500ms, 9'000ms, 10'500ms, 12'000ms })
        debouncer.Observe(At(offset));
    CHECK(debouncer.DueAt() == std::optional { At(13'000ms) });
}

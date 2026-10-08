// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NetworkChangeMessages.hpp>
#include <FastCache/Platform/NetworkChangeWatcher.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ReturnsWithin.hpp>

#if !defined(_WIN32)
    #include <unistd.h>
#endif

using FastCache::HostEvent;
using FastCache::Testing::ReturnsWithin;
using FastCache::Testing::WaitHangGuard;
using FastCache::Testing::WaitOptions;
using FastCache::Testing::WaitUntil;
using FastCache::Testing::WaitUntilOutcome;
using namespace std::chrono_literals;

namespace
{

/// A debounce short enough that the relay's thread looks again every few milliseconds of real
/// time, so a case pays little for a `ManualClock` that decides when anything is due.
constexpr auto ShortDebounce = FastCache::NetworkDebounce { .quiet = 5ms, .ceiling = 50ms };

/// How long a case watches for something that must NOT happen while the clock stands still:
/// many of the relay's looks at `ShortDebounce`, so a relay deciding on real time would be seen.
constexpr auto StillWindow = 200ms;

/// How long destroying a relay may take, whatever its thread was waiting for.
constexpr auto DestroyedWithin = WaitHangGuard;

/// @return A wait that runs for `StillWindow`.
[[nodiscard]] WaitOptions StillWindowWait()
{
    return WaitOptions { .step = {}, .context = {}, .bound = StillWindow, .rest = FastCache::Testing::WaitRest };
}

/// Counts network changes; optionally notifies a relay from inside the first one.
class CountingSink final: public FastCache::IHostEventSink
{
  public:
    void OnHostEvent(HostEvent event) override
    {
        if (event != HostEvent::NetworkChanged)
            others.fetch_add(1);
        if (auto* const relay = notifyFromInside.exchange(nullptr))
            relay->Notify();
        changes.fetch_add(1);
    }

    /// @return What the sink has heard so far, in words.
    [[nodiscard]] std::string Describe() const
    {
        return std::format("changes={} others={}", changes.load(), others.load());
    }

    std::atomic<int> changes { 0 };                                           ///< `NetworkChanged` deliveries.
    std::atomic<int> others { 0 };                                            ///< Any other event: none expected.
    std::atomic<FastCache::NetworkChangeRelay*> notifyFromInside { nullptr }; ///< Notified once, from inside.
};

/// A relay and everything its thread touches, owned together, so a case can leave behind a
/// destruction that never returns without that thread reading the case's freed frame.
struct RelayScene
{
    core::platform::ManualClock clock;                    ///< Decides when a burst is due.
    CountingSink sink;                                    ///< Hears the relay.
    std::unique_ptr<FastCache::NetworkChangeRelay> relay; ///< The relay under test.
};

/// @param bound The relay's debounce.
/// @return A scene whose relay is running.
[[nodiscard]] std::shared_ptr<RelayScene> MakeRelayScene(FastCache::NetworkDebounce bound)
{
    auto scene = std::make_shared<RelayScene>();
    scene->relay = std::make_unique<FastCache::NetworkChangeRelay>(scene->sink, scene->clock, bound);
    return scene;
}

/// Destroy @p scene's relay on a thread of its own.
/// @param scene The scene.
/// @return Whether the destruction returned within `DestroyedWithin`.
[[nodiscard]] bool RelayStops(std::shared_ptr<RelayScene> const& scene)
{
    return ReturnsWithin(DestroyedWithin, [scene] { scene->relay.reset(); });
}

} // namespace

TEST_CASE("The relay reports a burst once, when its clock says it is due", "[platform][host-events]")
{
    auto const scene = MakeRelayScene(ShortDebounce);
    auto& clock = scene->clock;
    auto& sink = scene->sink;
    auto& relay = *scene->relay;

    relay.Notify();
    relay.Notify();
    relay.Notify();
    auto const early = WaitUntilOutcome(
        "a delivery while the clock stands still",
        [&sink] { return sink.changes.load() > 0; },
        [&sink] { return sink.Describe(); },
        StillWindowWait());
    CHECK_FALSE(early.reached);

    clock.advance(ShortDebounce.quiet);
    REQUIRE(WaitUntil(
        "the burst to be delivered", [&sink] { return sink.changes.load() == 1; }, [&sink] { return sink.Describe(); }));

    // Spent: however far the clock moves now, that burst is not reported again.
    clock.advance(1h);
    auto const again = WaitUntilOutcome(
        "a second delivery of one burst",
        [&sink] { return sink.changes.load() > 1; },
        [&sink] { return sink.Describe(); },
        StillWindowWait());
    CHECK_FALSE(again.reached);
    CHECK(sink.others.load() == 0);

    // Idle again, its burst spent: the wait for the next one must end at the stop.
    INFO("destroying the idle relay did not return within DestroyedWithin: its wait for a burst ignored the stop");
    REQUIRE(RelayStops(scene));
}

TEST_CASE("A sink may notify the relay from inside its own delivery", "[platform][host-events]")
{
    // A relay that held its lock across the delivery would deadlock on this Notify.
    auto const scene = MakeRelayScene(ShortDebounce);
    auto& clock = scene->clock;
    auto& sink = scene->sink;
    auto& relay = *scene->relay;
    sink.notifyFromInside.store(&relay);

    relay.Notify();
    clock.advance(ShortDebounce.quiet);
    REQUIRE(WaitUntil(
        "the first burst to be delivered",
        [&sink] { return sink.changes.load() >= 1; },
        [&sink] { return sink.Describe(); }));

    // The notification from inside began a burst of its own.
    clock.advance(ShortDebounce.quiet);
    REQUIRE(WaitUntil(
        "the burst begun inside the delivery to be delivered",
        [&sink] { return sink.changes.load() == 2; },
        [&sink] { return sink.Describe(); }));

    INFO("destroying the idle relay did not return within DestroyedWithin: its wait for a burst ignored the stop");
    REQUIRE(RelayStops(scene));
}

TEST_CASE("Destroying the relay stops it while it waits for a burst far from due", "[platform][host-events]")
{
    // A relay whose wait nothing could cancel would sit in its destructor for the hour. Everything
    // the relay's thread and the destroying call touch is owned by them, so a destruction that
    // never returns can be left behind.
    auto const scene = MakeRelayScene(FastCache::NetworkDebounce { .quiet = 1h, .ceiling = 2h });
    scene->relay->Notify();
    // Time for the relay's thread to take the notification and park until the burst is due, which
    // is the wait this case is about; nothing is delivered meanwhile, the clock standing still.
    auto const delivered = WaitUntilOutcome(
        "a delivery of a burst due in an hour",
        [&sink = scene->sink] { return sink.changes.load() > 0; },
        [&sink = scene->sink] { return sink.Describe(); },
        StillWindowWait());
    CHECK_FALSE(delivered.reached);

    INFO("destroying the relay did not return within DestroyedWithin: its wait for the burst ignored the stop");
    REQUIRE(RelayStops(scene));
    CHECK(scene->sink.changes.load() == 0);
}

TEST_CASE("The network watcher runs where the platform offers one, and is absent elsewhere", "[platform][host-events]")
{
    struct Scene
    {
        core::platform::SteadyClock clock;
        CountingSink sink;
        std::unique_ptr<FastCache::NetworkChangeWatcher> watcher;
    };
    auto const scene = std::make_shared<Scene>();
    auto started = FastCache::StartNetworkChangeWatcher(scene->sink, scene->clock, FastCache::NetworkDebounce {});

    INFO((started.has_value() ? std::string { "started" } : started.error()));
    REQUIRE(started.has_value());
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    CHECK(*started != nullptr);
#else
    // No watcher is not an error: this platform offers nothing this project watches.
    CHECK(*started == nullptr);
#endif
    scene->watcher = std::move(*started);
    INFO("destroying the watcher did not return within DestroyedWithin: its relay's idle wait ignored the stop");
    REQUIRE(ReturnsWithin(DestroyedWithin, [scene] { scene->watcher.reset(); }));
}

#if !defined(_WIN32)

namespace
{

/// A pipe both of whose ends close with it; the read end can be handed away.
struct Pipe
{
    Pipe()
    {
        std::array<int, 2> ends {};
        REQUIRE(::pipe(ends.data()) == 0);
        readEnd = ends[0];
        writeEnd = ends[1];
    }
    ~Pipe()
    {
        CloseWriteEnd();
        if (readEnd >= 0)
            (void) ::close(readEnd);
    }
    Pipe(Pipe const&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe const&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    /// @return The read end, no longer this pipe's to close.
    [[nodiscard]] int TakeReadEnd() noexcept
    {
        return std::exchange(readEnd, -1);
    }

    /// Close the write end, which the reader sees as end of file.
    void CloseWriteEnd() noexcept
    {
        if (writeEnd >= 0)
            (void) ::close(std::exchange(writeEnd, -1));
    }

    /// Write one whole netlink message of @p type, with no payload.
    /// @param type The `nlmsg_type`.
    void WriteNetlink(std::uint16_t type) const
    {
        struct Header
        {
            std::uint32_t length;
            std::uint16_t type;
            std::uint16_t flags;
            std::uint32_t seq;
            std::uint32_t pid;
        };
        auto const bytes = std::bit_cast<std::array<std::byte, sizeof(Header)>>(
            Header { .length = sizeof(Header), .type = type, .flags = 0, .seq = 1, .pid = 0 });
        REQUIRE(::write(writeEnd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }

    int readEnd = -1;  ///< Owned until taken.
    int writeEnd = -1; ///< Owned.
};

/// The netlink types the cases write: one that reports a change, one that does not.
constexpr std::uint16_t NetlinkNewAddr = 20;
constexpr std::uint16_t NetlinkDone = 3;

/// A descriptor watcher and everything its threads touch, owned together.
struct DescriptorScene
{
    core::platform::SteadyClock clock;                        ///< Real time: the debounce is short.
    CountingSink sink;                                        ///< Hears the relay.
    Pipe pipe;                                                ///< What the watcher reads.
    std::unique_ptr<FastCache::NetworkChangeWatcher> watcher; ///< The watcher under test.
};

/// @return A scene whose watcher reads its pipe as netlink, with `ShortDebounce`.
[[nodiscard]] std::shared_ptr<DescriptorScene> MakeDescriptorScene()
{
    auto scene = std::make_shared<DescriptorScene>();
    auto started = FastCache::WatchNetworkChangeDescriptor(
        scene->pipe.TakeReadEnd(), &FastCache::NetlinkReportsChange, scene->sink, scene->clock, ShortDebounce);
    INFO((started.has_value() ? std::string { "started" } : started.error()));
    REQUIRE(started.has_value());
    REQUIRE(*started != nullptr);
    scene->watcher = std::move(*started);
    return scene;
}

} // namespace

TEST_CASE("A descriptor watcher reports what its classifier counts and nothing else", "[platform][host-events]")
{
    auto const scene = MakeDescriptorScene();
    auto& sink = scene->sink;

    scene->pipe.WriteNetlink(NetlinkDone);
    auto const early = WaitUntilOutcome(
        "a delivery for a message that reports no change",
        [&sink] { return sink.changes.load() > 0; },
        [&sink] { return sink.Describe(); },
        StillWindowWait());
    CHECK_FALSE(early.reached);

    scene->pipe.WriteNetlink(NetlinkNewAddr);
    REQUIRE(WaitUntil(
        "the address change to be delivered",
        [&sink] { return sink.changes.load() == 1; },
        [&sink] { return sink.Describe(); }));
    CHECK(sink.others.load() == 0);

    INFO("destroying the watcher did not return within DestroyedWithin: its poll ignored the stop");
    REQUIRE(ReturnsWithin(DestroyedWithin, [scene] { scene->watcher.reset(); }));
}

TEST_CASE("A descriptor watcher whose socket ended is still destroyed promptly", "[platform][host-events]")
{
    auto const scene = MakeDescriptorScene();
    scene->pipe.CloseWriteEnd();
    INFO("destroying the watcher did not return within DestroyedWithin after its descriptor ended");
    REQUIRE(ReturnsWithin(DestroyedWithin, [scene] { scene->watcher.reset(); }));
    CHECK(scene->sink.changes.load() == 0);
}

#endif // !_WIN32

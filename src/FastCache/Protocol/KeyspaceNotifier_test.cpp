// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/PubSubRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using FastCache::IPubSubRegistry;
using FastCache::ISubscriber;
using FastCache::KeyspaceNotifier;
using FastCache::ParseKeyspaceEvents;
using FastCache::PubSubRegistry;
using FastCache::PushMessage;
namespace KeyspaceEvents = FastCache::KeyspaceEvents;

namespace
{

class CapturingSubscriber: public ISubscriber
{
  public:
    void Deliver(PushMessage message) override
    {
        messages.push_back(std::move(message));
    }
    std::vector<PushMessage> messages;
};

[[nodiscard]] std::shared_ptr<CapturingSubscriber> Subscribe(PubSubRegistry& registry, std::string const& channel)
{
    auto sub = std::make_shared<CapturingSubscriber>();
    (void) registry.Subscribe(sub, channel);
    return sub;
}

} // namespace

TEST_CASE("ParseKeyspaceEvents: empty string → no flags", "[protocol][keyspace]")
{
    auto const mask = ParseKeyspaceEvents("");
    REQUIRE(mask.has_value());
    REQUIRE(*mask == KeyspaceEvents::None);
}

TEST_CASE("ParseKeyspaceEvents: AKE expands and ORs to the all-events mask", "[protocol][keyspace]")
{
    auto const mask = ParseKeyspaceEvents("AKE");
    REQUIRE(mask.has_value());
    REQUIRE((*mask & KeyspaceEvents::Keyspace) != 0);
    REQUIRE((*mask & KeyspaceEvents::Keyevent) != 0);
    REQUIRE((*mask & KeyspaceEvents::Generic) != 0);
    REQUIRE((*mask & KeyspaceEvents::String) != 0);
    REQUIRE((*mask & KeyspaceEvents::Expired) != 0);
}

TEST_CASE("ParseKeyspaceEvents: KEg$x mirrors A explicitly", "[protocol][keyspace]")
{
    auto const a = ParseKeyspaceEvents("AKE");
    auto const explicitly = ParseKeyspaceEvents("KEg$x");
    REQUIRE(a.has_value());
    REQUIRE(explicitly.has_value());
    REQUIRE(*a == *explicitly);
}

TEST_CASE("ParseKeyspaceEvents: unknown letter is a ConfigError", "[protocol][keyspace]")
{
    auto const mask = ParseKeyspaceEvents("KZ");
    REQUIRE_FALSE(mask.has_value());
    REQUIRE(mask.error().field == "notify-keyspace-events");
}

TEST_CASE("KeyspaceNotifier: with no channel flag set, publishes nothing", "[protocol][keyspace]")
{
    PubSubRegistry registry;
    auto sub = Subscribe(registry, "__keyspace@0__:foo");

    // Only Generic class enabled — neither K nor E set — so OnEvent is a no-op.
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Generic };
    REQUIRE_FALSE(notifier.IsEnabled());
    notifier.OnEvent(KeyspaceEvents::Generic, "del", "foo");
    REQUIRE(sub->messages.empty());
}

TEST_CASE("KeyspaceNotifier: with K and Generic, fires on __keyspace channel", "[protocol][keyspace]")
{
    PubSubRegistry registry;
    auto sub = Subscribe(registry, "__keyspace@0__:foo");

    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyspace | KeyspaceEvents::Generic };
    REQUIRE(notifier.IsEnabled());
    notifier.OnEvent(KeyspaceEvents::Generic, "del", "foo");
    REQUIRE(sub->messages.size() == 1);
    REQUIRE(sub->messages[0].channel == "__keyspace@0__:foo");
    REQUIRE(sub->messages[0].payload == "del");
}

TEST_CASE("KeyspaceNotifier: with E and Generic, fires on __keyevent channel", "[protocol][keyspace]")
{
    PubSubRegistry registry;
    auto sub = Subscribe(registry, "__keyevent@0__:del");

    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyevent | KeyspaceEvents::Generic };
    notifier.OnEvent(KeyspaceEvents::Generic, "del", "foo");
    REQUIRE(sub->messages.size() == 1);
    REQUIRE(sub->messages[0].channel == "__keyevent@0__:del");
    REQUIRE(sub->messages[0].payload == "foo");
}

TEST_CASE("KeyspaceNotifier: K+E publishes on both channels", "[protocol][keyspace]")
{
    PubSubRegistry registry;
    auto keyspaceSub = Subscribe(registry, "__keyspace@0__:foo");
    auto keyeventSub = Subscribe(registry, "__keyevent@0__:set");

    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyspace | KeyspaceEvents::Keyevent | KeyspaceEvents::String };
    notifier.OnEvent(KeyspaceEvents::String, "set", "foo");
    REQUIRE(keyspaceSub->messages.size() == 1);
    REQUIRE(keyspaceSub->messages[0].payload == "set");
    REQUIRE(keyeventSub->messages.size() == 1);
    REQUIRE(keyeventSub->messages[0].payload == "foo");
}

TEST_CASE("KeyspaceNotifier: class gating drops events whose class is off", "[protocol][keyspace]")
{
    PubSubRegistry registry;
    auto sub = Subscribe(registry, "__keyspace@0__:foo");

    // K + String only: a Generic event (DEL) must be suppressed.
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyspace | KeyspaceEvents::String };
    notifier.OnEvent(KeyspaceEvents::Generic, "del", "foo");
    REQUIRE(sub->messages.empty());
    notifier.OnEvent(KeyspaceEvents::String, "set", "foo");
    REQUIRE(sub->messages.size() == 1);
    REQUIRE(sub->messages[0].payload == "set");
}

TEST_CASE("KeyspaceNotifier: null pub/sub is a safe no-op", "[protocol][keyspace]")
{
    KeyspaceNotifier notifier { nullptr, KeyspaceEvents::Keyspace | KeyspaceEvents::Generic };
    notifier.OnEvent(KeyspaceEvents::Generic, "del", "foo"); // must not crash
}

namespace
{

/// Counting IPubSubRegistry: records every Publish() call so a test can
/// assert "zero publishes" or "n publishes". The subscribe/snapshot
/// surface is unused by the fast-path test, so the methods are stubs.
class CountingPubSub final: public IPubSubRegistry
{
  public:
    std::atomic<std::size_t> publishCount { 0 };
    bool hasSubs { false };

    std::size_t Subscribe(std::shared_ptr<ISubscriber> /*sub*/, std::string_view /*channel*/) override
    {
        return 0;
    }
    std::size_t Unsubscribe(ISubscriber* /*sub*/, std::string_view /*channel*/) override
    {
        return 0;
    }
    std::size_t PSubscribe(std::shared_ptr<ISubscriber> /*sub*/, std::string_view /*pattern*/) override
    {
        return 0;
    }
    std::size_t PUnsubscribe(ISubscriber* /*sub*/, std::string_view /*pattern*/) override
    {
        return 0;
    }
    void UnsubscribeAll(ISubscriber* /*sub*/) override {}
    std::size_t Publish(std::string_view /*channel*/, std::string_view /*message*/) override
    {
        publishCount.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    [[nodiscard]] std::vector<std::string> SnapshotChannels(ISubscriber* /*sub*/) const override
    {
        return {};
    }
    [[nodiscard]] std::vector<std::string> SnapshotPatterns(ISubscriber* /*sub*/) const override
    {
        return {};
    }
    [[nodiscard]] bool HasAnySubscribers() const noexcept override
    {
        return hasSubs;
    }
};

} // namespace

TEST_CASE("KeyspaceNotifier: zero-subscriber short-circuit skips Publish entirely",
          "[protocol][keyspace]")
{
    // The whole point of the fast path: when nothing is subscribed we must
    // not call Publish() at all. Otherwise an operator running with
    // notify-keyspace-events=AKE on a hot write workload would pay the
    // channel-format cost on every SET.
    CountingPubSub pubsub;
    pubsub.hasSubs = false;

    KeyspaceNotifier notifier { &pubsub,
                                KeyspaceEvents::Keyspace | KeyspaceEvents::Keyevent
                                    | KeyspaceEvents::String };
    notifier.OnEvent(KeyspaceEvents::String, "set", "foo");
    REQUIRE(pubsub.publishCount.load() == 0);
}

TEST_CASE("KeyspaceNotifier: with subscribers, Publish is called", "[protocol][keyspace]")
{
    // Symmetric guard: a subscriber present must NOT silence the publish
    // (a regression where HasAnySubscribers always returned false would pass
    // the prior test but break delivery in production).
    CountingPubSub pubsub;
    pubsub.hasSubs = true;

    KeyspaceNotifier notifier { &pubsub,
                                KeyspaceEvents::Keyspace | KeyspaceEvents::Keyevent
                                    | KeyspaceEvents::String };
    notifier.OnEvent(KeyspaceEvents::String, "set", "foo");
    // K + E: one publish per channel (__keyspace and __keyevent).
    REQUIRE(pubsub.publishCount.load() == 2);
}

TEST_CASE("PubSubRegistry: HasAnySubscribers flips as subscriptions come and go",
          "[protocol][keyspace]")
{
    PubSubRegistry registry;
    REQUIRE_FALSE(registry.HasAnySubscribers());

    auto sub = std::make_shared<CapturingSubscriber>();
    (void) registry.Subscribe(sub, "ch");
    REQUIRE(registry.HasAnySubscribers());

    registry.UnsubscribeAll(sub.get());
    REQUIRE_FALSE(registry.HasAnySubscribers());
}

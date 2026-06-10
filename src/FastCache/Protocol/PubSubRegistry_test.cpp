// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/PubSubRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <vector>

using namespace FastCache;

namespace
{

/// Records the messages delivered to it, so a test can assert fan-out without a
/// real connection/socket. Single-threaded use in these tests.
class RecordingSubscriber final: public ISubscriber
{
  public:
    void Deliver(PushMessage message) override
    {
        received.push_back(std::move(message));
    }

    std::vector<PushMessage> received;
};

} // namespace

TEST_CASE("GlobMatch: literals, ?, *, classes, and negation", "[pubsub][glob]")
{
    CHECK(GlobMatch("news", "news"));
    CHECK_FALSE(GlobMatch("news", "newt"));
    CHECK(GlobMatch("news.*", "news.tech"));
    CHECK(GlobMatch("*.tech", "news.tech"));
    CHECK(GlobMatch("news.?ech", "news.tech"));
    CHECK_FALSE(GlobMatch("news.?", "news.tech")); // '?' is exactly one char
    CHECK(GlobMatch("h[ae]llo", "hello"));
    CHECK(GlobMatch("h[ae]llo", "hallo"));
    CHECK_FALSE(GlobMatch("h[ae]llo", "hxllo"));
    CHECK(GlobMatch("h[^x]llo", "hello"));
    CHECK_FALSE(GlobMatch("h[^e]llo", "hello"));
    CHECK(GlobMatch("[a-c]at", "bat"));
    CHECK_FALSE(GlobMatch("[a-c]at", "zat"));
    CHECK(GlobMatch("*", "anything"));
    CHECK(GlobMatch("a*b*c", "axxbyyc"));
}

TEST_CASE("PubSubRegistry: subscribe count and exact-channel delivery", "[pubsub]")
{
    PubSubRegistry registry;
    auto sub = std::make_shared<RecordingSubscriber>();

    CHECK(registry.Subscribe(sub, "news") == 1);
    CHECK(registry.Subscribe(sub, "sports") == 2);

    CHECK(registry.Publish("news", "hello") == 1);
    REQUIRE(sub->received.size() == 1);
    CHECK(sub->received[0].kind == PushMessage::Kind::Message);
    CHECK(sub->received[0].channel == "news");
    CHECK(sub->received[0].payload == "hello");

    // A channel with no subscribers delivers to nobody.
    CHECK(registry.Publish("weather", "sunny") == 0);
    CHECK(sub->received.size() == 1);
}

TEST_CASE("PubSubRegistry: pattern delivery carries the pattern", "[pubsub]")
{
    PubSubRegistry registry;
    auto sub = std::make_shared<RecordingSubscriber>();

    CHECK(registry.PSubscribe(sub, "news.*") == 1);
    CHECK(registry.Publish("news.tech", "x") == 1);
    REQUIRE(sub->received.size() == 1);
    CHECK(sub->received[0].kind == PushMessage::Kind::PMessage);
    CHECK(sub->received[0].pattern == "news.*");
    CHECK(sub->received[0].channel == "news.tech");
}

TEST_CASE("PubSubRegistry: a publish reaches both exact and pattern subscribers", "[pubsub]")
{
    PubSubRegistry registry;
    auto exact = std::make_shared<RecordingSubscriber>();
    auto glob = std::make_shared<RecordingSubscriber>();

    CHECK(registry.Subscribe(exact, "news.tech") == 1);
    CHECK(registry.PSubscribe(glob, "news.*") == 1);
    CHECK(registry.Publish("news.tech", "x") == 2);
    CHECK(exact->received.size() == 1);
    CHECK(glob->received.size() == 1);
}

TEST_CASE("PubSubRegistry: unsubscribe stops delivery and updates the count", "[pubsub]")
{
    PubSubRegistry registry;
    auto sub = std::make_shared<RecordingSubscriber>();

    CHECK(registry.Subscribe(sub, "a") == 1);
    CHECK(registry.Subscribe(sub, "b") == 2);
    CHECK(registry.Unsubscribe(sub.get(), "a") == 1);
    CHECK(registry.Publish("a", "x") == 0);
    CHECK(registry.Publish("b", "y") == 1);
}

TEST_CASE("PubSubRegistry: UnsubscribeAll detaches from every channel and pattern", "[pubsub]")
{
    PubSubRegistry registry;
    auto sub = std::make_shared<RecordingSubscriber>();

    CHECK(registry.Subscribe(sub, "a") == 1);
    CHECK(registry.PSubscribe(sub, "p.*") == 2);
    registry.UnsubscribeAll(sub.get());
    CHECK(registry.Publish("a", "x") == 0);
    CHECK(registry.Publish("p.q", "y") == 0);
}

TEST_CASE("PubSubRegistry: Publish skips subscribers whose lifetime has ended", "[pubsub]")
{
    // Regression: a subscriber that disappears between snapshot and Deliver
    // (the old UAF) must be skipped via the weak_ptr upgrade, not crashed on.
    PubSubRegistry registry;
    auto sub = std::make_shared<RecordingSubscriber>();
    auto* const raw = sub.get();

    CHECK(registry.Subscribe(sub, "a") == 1);
    sub.reset(); // The only strong ref is gone; the weak in the registry expires.
    // Publish must not crash and must report zero live receivers (even though
    // the registry still has the raw-pointer key in its map).
    CHECK(registry.Publish("a", "x") == 0);
    registry.UnsubscribeAll(raw); // safe even with the subscriber destroyed (raw is a key only)
}

TEST_CASE("PubSubRegistry: SnapshotChannels / SnapshotPatterns enumerate live registrations", "[pubsub]")
{
    PubSubRegistry registry;
    auto sub = std::make_shared<RecordingSubscriber>();

    CHECK(registry.Subscribe(sub, "a") == 1);
    CHECK(registry.Subscribe(sub, "b") == 2);
    CHECK(registry.PSubscribe(sub, "p.*") == 3);

    auto const channels = registry.SnapshotChannels(sub.get());
    REQUIRE(channels.size() == 2);

    auto const patterns = registry.SnapshotPatterns(sub.get());
    REQUIRE(patterns.size() == 1);
    CHECK(patterns[0] == "p.*");
}

TEST_CASE("PubSubRegistry: HasAnySubscribers reflects live entry count lock-free", "[pubsub][fast-path]")
{
    // Steady state for a daemon with `notify-keyspace-events` configured but
    // no subscriber yet must be HasAnySubscribers() == false, which lets
    // KeyspaceNotifier::OnEvent skip the channel-format + lookup on every
    // SET. Counter rises on each Subscribe/PSubscribe and falls back to zero
    // through Unsubscribe/PUnsubscribe and UnsubscribeAll.
    PubSubRegistry registry;
    REQUIRE_FALSE(registry.HasAnySubscribers());

    auto sub = std::make_shared<RecordingSubscriber>();
    CHECK(registry.Subscribe(sub, "ch") == 1);
    REQUIRE(registry.HasAnySubscribers());

    CHECK(registry.PSubscribe(sub, "p.*") == 2);
    REQUIRE(registry.HasAnySubscribers());

    // Idempotent re-Subscribe of the same (sub, channel) must NOT double the
    // counter — otherwise the entry-count would drift up over time.
    CHECK(registry.Subscribe(sub, "ch") == 2);
    REQUIRE(registry.HasAnySubscribers());

    CHECK(registry.Unsubscribe(sub.get(), "ch") == 1);
    REQUIRE(registry.HasAnySubscribers()); // pattern still present
    CHECK(registry.PUnsubscribe(sub.get(), "p.*") == 0);
    REQUIRE_FALSE(registry.HasAnySubscribers());

    // UnsubscribeAll on an already-empty registry stays at zero (no underflow).
    registry.UnsubscribeAll(sub.get());
    REQUIRE_FALSE(registry.HasAnySubscribers());
}

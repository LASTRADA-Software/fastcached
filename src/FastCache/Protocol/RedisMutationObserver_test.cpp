// SPDX-License-Identifier: Apache-2.0
//
// What the storage-layer observer publishes, and — just as load-bearing — what
// it deliberately does not. Publishing for a kind the Redis handlers already
// cover would double every subscriber's event stream.
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/PubSubRegistry.hpp>
#include <FastCache/Protocol/RedisMutationObserver.hpp>
#include <FastCache/Protocol/RedisTransaction.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace FastCache;

/// Records the frames published to whichever channel it subscribed to.
class CapturingSubscriber final: public ISubscriber
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
    static_cast<void>(registry.Subscribe(sub, channel));
    return sub;
}

} // namespace

TEST_CASE("The observer publishes expired for a reclaimed TTL", "[protocol][keyspace][mutation-observer]")
{
    PubSubRegistry registry;
    auto const sub = Subscribe(registry, "__keyevent@0__:expired");
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyevent | KeyspaceEvents::All };
    RedisMutationObserver observer { nullptr, &notifier };

    observer.OnMutation(MutationKind::Expire, "lapsed");

    REQUIRE(sub->messages.size() == 1);
    REQUIRE(sub->messages[0].payload == "lapsed");
}

TEST_CASE("The observer publishes evicted for a reclaimed tail", "[protocol][keyspace][mutation-observer]")
{
    PubSubRegistry registry;
    auto const sub = Subscribe(registry, "__keyevent@0__:evicted");
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyevent | KeyspaceEvents::All };
    RedisMutationObserver observer { nullptr, &notifier };

    observer.OnMutation(MutationKind::Evict, "cold");

    REQUIRE(sub->messages.size() == 1);
    REQUIRE(sub->messages[0].payload == "cold");
}

TEST_CASE("The observer publishes nothing for kinds the Redis handlers already cover",
          "[protocol][keyspace][mutation-observer]")
{
    // The reason most rows of EventTable are empty. RedisResp.cpp fires
    // verb-specific events for everything a client asked for; a second publish
    // here would put two __keyevent@0__:* frames on the wire per write, which
    // every existing subscriber would see as a behaviour change.
    PubSubRegistry registry;
    auto const setSub = Subscribe(registry, "__keyevent@0__:set");
    auto const delSub = Subscribe(registry, "__keyevent@0__:del");
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyevent | KeyspaceEvents::All };
    RedisMutationObserver observer { nullptr, &notifier };

    for (auto const kind: { MutationKind::Set,
                            MutationKind::Delete,
                            MutationKind::Append,
                            MutationKind::Prepend,
                            MutationKind::Incr,
                            MutationKind::Touch,
                            MutationKind::Persist,
                            MutationKind::MarkStale,
                            MutationKind::Cas,
                            MutationKind::Update,
                            MutationKind::FlushDb })
        observer.OnMutation(kind, "k");

    REQUIRE(setSub->messages.empty());
    REQUIRE(delSub->messages.empty());
}

TEST_CASE("Only Expire and Evict carry an event name", "[protocol][keyspace][mutation-observer]")
{
    // Asserted over the table itself rather than through OnMutation, so adding
    // a kind that publishes cannot slip past unnoticed.
    for (auto const kind: { MutationKind::Set,
                            MutationKind::Delete,
                            MutationKind::Append,
                            MutationKind::Prepend,
                            MutationKind::Incr,
                            MutationKind::Touch,
                            MutationKind::Persist,
                            MutationKind::MarkStale,
                            MutationKind::Cas,
                            MutationKind::Update,
                            MutationKind::FlushDb })
        REQUIRE(RedisMutationObserver::DescriptorFor(kind).name.empty());

    auto const expire = RedisMutationObserver::DescriptorFor(MutationKind::Expire);
    REQUIRE(expire.name == "expired");
    REQUIRE(expire.classFlag == KeyspaceEvents::Expired);

    auto const evict = RedisMutationObserver::DescriptorFor(MutationKind::Evict);
    REQUIRE(evict.name == "evicted");
    REQUIRE(evict.classFlag == KeyspaceEvents::Evicted);
}

TEST_CASE("An operator who enabled only 'x' gets expired and not evicted", "[protocol][keyspace][mutation-observer]")
{
    PubSubRegistry registry;
    auto const expiredSub = Subscribe(registry, "__keyevent@0__:expired");
    auto const evictedSub = Subscribe(registry, "__keyevent@0__:evicted");
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyevent | KeyspaceEvents::Expired };
    RedisMutationObserver observer { nullptr, &notifier };

    observer.OnMutation(MutationKind::Expire, "a");
    observer.OnMutation(MutationKind::Evict, "b");

    REQUIRE(expiredSub->messages.size() == 1);
    REQUIRE(evictedSub->messages.empty());
}

TEST_CASE("The fast probe answers for the notifier, not only the WATCH registry", "[protocol][keyspace][mutation-observer]")
{
    // The probe gates whether the tiers copy a victim's key at all. An observer
    // that answered only for WATCHers would leave a daemon with subscribers and
    // no WATCHers publishing nothing, because nothing would ever be recorded.
    PubSubRegistry registry;
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::Keyevent | KeyspaceEvents::All };

    RedisMutationObserver const noSinks { nullptr, nullptr };
    REQUIRE_FALSE(noSinks.HasObservers());

    RedisMutationObserver const withNotifier { nullptr, &notifier };
    // No subscriber yet: nothing to publish to, so nothing worth recording.
    REQUIRE_FALSE(withNotifier.HasObservers());

    auto const sub = Subscribe(registry, "__keyevent@0__:expired");
    REQUIRE(withNotifier.HasObservers());
    static_cast<void>(sub);
}

TEST_CASE("A WATCH'd key is dirtied by an expiry as well as by a write", "[protocol][keyspace][mutation-observer]")
{
    // The correctness claim NotifyingStorage.hpp has carried since it landed:
    // a watched key that vanishes under TTL pressure must abort the EXEC.
    WatchRegistry watches;
    auto const handle = std::make_shared<WatchHandle>();
    handle->Remember("watched", CasToken { 7 });
    REQUIRE(watches.Register(handle, "watched"));

    RedisMutationObserver observer { &watches, nullptr };
    REQUIRE_FALSE(handle->IsDirty());

    observer.OnMutation(MutationKind::Expire, "watched");
    REQUIRE(handle->IsDirty());
}

TEST_CASE("A class enabled with neither K nor E does not make the tiers record", "[protocol][keyspace][mutation-observer]")
{
    // `notify-keyspace-events: A` names every class and no channel, so OnEvent
    // publishes nothing. The probe gates a key copy per reclaimed entry taken
    // inside the tier's lock, so answering yes here would buy frames that never
    // go out.
    PubSubRegistry registry;
    auto const sub = Subscribe(registry, "__keyevent@0__:expired");
    KeyspaceNotifier notifier { &registry, KeyspaceEvents::All };
    RedisMutationObserver const observer { nullptr, &notifier };

    REQUIRE_FALSE(observer.HasObservers());
    static_cast<void>(sub);
}

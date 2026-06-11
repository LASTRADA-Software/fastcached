// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/IStreamWaiterRegistry.hpp>
#include <FastCache/Protocol/StreamWaiterRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;

namespace
{

/// Test double counting how many times it has been woken.
struct CountingWaiter final: public IStreamWaiter
{
    int wakes { 0 };
    void Wake() noexcept override
    {
        ++wakes;
    }
};

std::vector<std::string> Keys(std::initializer_list<std::string> ks)
{
    return std::vector<std::string> { ks };
}

} // namespace

TEST_CASE("StreamWaiterRegistry: NotifyAppended wakes waiters on the matching key", "[protocol][stream]")
{
    StreamWaiterRegistry reg;
    auto a = std::make_shared<CountingWaiter>();
    auto b = std::make_shared<CountingWaiter>();
    auto const ka = Keys({ "s1" });
    auto const kb = Keys({ "s2" });
    reg.Register(a, ka);
    reg.Register(b, kb);

    reg.NotifyAppended("s1");
    REQUIRE(a->wakes == 1);
    REQUIRE(b->wakes == 0);

    reg.NotifyAppended("s2");
    REQUIRE(a->wakes == 1);
    REQUIRE(b->wakes == 1);

    // An append on a key with no waiters is a harmless no-op.
    reg.NotifyAppended("unwatched");
    REQUIRE(a->wakes == 1);
    REQUIRE(b->wakes == 1);
}

TEST_CASE("StreamWaiterRegistry: a waiter on several keys wakes on any of them", "[protocol][stream]")
{
    StreamWaiterRegistry reg;
    auto w = std::make_shared<CountingWaiter>();
    auto const keys = Keys({ "a", "b", "c" });
    reg.Register(w, keys);

    reg.NotifyAppended("b");
    REQUIRE(w->wakes == 1);
    // Still registered on the others (the registry does not auto-deregister on
    // wake — the waiter's own resolution decides that).
    reg.NotifyAppended("c");
    REQUIRE(w->wakes == 2);
}

TEST_CASE("StreamWaiterRegistry: Unregister stops future wakes", "[protocol][stream]")
{
    StreamWaiterRegistry reg;
    auto w = std::make_shared<CountingWaiter>();
    auto const keys = Keys({ "s1", "s2" });
    reg.Register(w, keys);

    reg.Unregister(w.get());
    reg.NotifyAppended("s1");
    reg.NotifyAppended("s2");
    REQUIRE(w->wakes == 0);
}

TEST_CASE("StreamWaiterRegistry: an expired waiter is skipped (no use-after-free)", "[protocol][stream]")
{
    StreamWaiterRegistry reg;
    std::weak_ptr<CountingWaiter> weak;
    {
        auto w = std::make_shared<CountingWaiter>();
        weak = w;
        auto const keys = Keys({ "s1" });
        reg.Register(w, keys);
    } // w destroyed here without Unregister — the registry holds only a weak_ptr.
    REQUIRE(weak.expired());

    // Must not crash; the dead weak_ptr is simply pruned.
    reg.NotifyAppended("s1");
    // A second notify confirms the entry was dropped and the map is consistent.
    reg.NotifyAppended("s1");
    SUCCEED("no use-after-free on an expired waiter");
}

TEST_CASE("StreamWaiterRegistry: re-registering the same waiter does not double-wake", "[protocol][stream]")
{
    StreamWaiterRegistry reg;
    auto w = std::make_shared<CountingWaiter>();
    auto const keys = Keys({ "s1" });
    reg.Register(w, keys);
    reg.Register(w, keys); // idempotent per (key, waiter).

    reg.NotifyAppended("s1");
    REQUIRE(w->wakes == 1);
}

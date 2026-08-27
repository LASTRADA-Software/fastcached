// SPDX-License-Identifier: Apache-2.0
//
// What each storage tier reports when it reclaims an entry nobody asked it to
// reclaim. The routing is as load-bearing as the reporting: a tier that reports
// a demotion is worse than one that reports nothing, because the event is a
// claim about a key that is still there.
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/IReclaimLog.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <tests/ScratchPath.hpp>

namespace
{

using namespace FastCache;

/// Observer that is always interested, so `ReclaimLog::IsRecording()` is true
/// and the tiers actually record. What they record is what these tests read.
class AlwaysListening final: public IStorageMutationObserver
{
  public:
    void OnMutation(MutationKind /*kind*/, std::string_view /*key*/) noexcept override {}

    [[nodiscard]] bool HasObservers() const noexcept override
    {
        return true;
    }
};

/// The keys drained from `log`, as `kind:key`, so an assertion reads as the
/// event stream a subscriber would have seen.
[[nodiscard]] std::vector<std::string> DrainNames(ReclaimLog& log)
{
    std::vector<ReclaimedKey> drained;
    log.Drain(drained);
    std::vector<std::string> names;
    names.reserve(drained.size());
    for (auto const& entry: drained)
        names.push_back((entry.kind == MutationKind::Expire ? "expire:" : "evict:") + entry.key);
    return names;
}

/// A value big enough that a byte budget can be expressed in whole entries.
[[nodiscard]] std::vector<std::byte> Value(std::size_t bytes)
{
    return std::vector<std::byte>(bytes);
}

/// A verb that loads a record before it writes, and the name a failure reports.
struct ReclaimingVerb
{
    std::string_view name;                                               ///< For the failure message.
    void (*invoke)(IStorage& tier, std::string_view key, TimePoint now); ///< Runs it, outcome ignored.
};

/// Every such verb. Each must reclaim a record it finds lapsed and say so, on
/// whichever backend an operator configured -- so the table is the list, and a
/// verb added without a reclaim fails here rather than in the field.
auto const ReclaimingVerbs = std::to_array<ReclaimingVerb>({
    { .name = "delete",
      .invoke = [](IStorage& tier, std::string_view key, TimePoint now) { std::ignore = tier.Delete(key, now); } },
    { .name = "add",
      .invoke = [](IStorage& tier,
                   std::string_view key,
                   TimePoint now) { std::ignore = tier.Add(key, Value(8), 0, TimePoint::max(), now); } },
    { .name = "replace",
      .invoke = [](IStorage& tier,
                   std::string_view key,
                   TimePoint now) { std::ignore = tier.Replace(key, Value(8), 0, TimePoint::max(), now); } },
    { .name = "append",
      .invoke =
          [](IStorage& tier, std::string_view key, TimePoint now) {
              auto const suffix = Value(4);
              std::ignore = tier.Append(key, suffix, 0, now);
          } },
    { .name = "prepend",
      .invoke =
          [](IStorage& tier, std::string_view key, TimePoint now) {
              auto const prefix = Value(4);
              std::ignore = tier.Prepend(key, prefix, 0, now);
          } },
    { .name = "cas",
      .invoke = [](IStorage& tier,
                   std::string_view key,
                   TimePoint now) { std::ignore = tier.CompareAndSwap(key, 1, Value(8), 0, TimePoint::max(), now); } },
    { .name = "incr",
      .invoke = [](IStorage& tier,
                   std::string_view key,
                   TimePoint now) { std::ignore = tier.IncrementOrInitialize(key, 1, false, now); } },
    { .name = "touch",
      .invoke = [](IStorage& tier,
                   std::string_view key,
                   TimePoint now) { std::ignore = tier.Touch(key, TimePoint::max(), now); } },
});

} // namespace

TEST_CASE("A tier reports nothing until a log is routed to it", "[cache][reclaim-reporting]")
{
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru { 100 };

    // Budget is 100 bytes; the second Set must evict the first. Nobody has
    // called SetReclaimLog, so nothing is reported.
    REQUIRE(lru.Set("a", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(lru.Set("b", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE_FALSE(log.HasPending());
}

TEST_CASE("The in-memory tier reports the LRU tail it evicted", "[cache][reclaim-reporting]")
{
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru { 100 };
    lru.SetReclaimLog(&log);

    REQUIRE(lru.Set("a", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(DrainNames(log).empty());

    REQUIRE(lru.Set("b", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(DrainNames(log) == std::vector<std::string> { "evict:a" });
}

TEST_CASE("The in-memory tier reports a TTL it finds lapsed", "[cache][reclaim-reporting]")
{
    AlwaysListening obs;
    ReclaimLog log { &obs };
    // Strict mode so a Get takes the lazy-reclaim path rather than the
    // shared-read one, which leaves the expired node in place.
    InMemoryLruStorage lru { 0, 0, LruMode::Strict };
    lru.SetReclaimLog(&log);

    auto const start = TimePoint {};
    auto const expiry = start + std::chrono::seconds { 10 };
    REQUIRE(lru.Set("doomed", Value(8), 0, expiry).has_value());
    REQUIRE(DrainNames(log).empty());

    auto const later = expiry + std::chrono::seconds { 1 };
    auto const got = lru.Get("doomed", later);
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->found);
    REQUIRE(DrainNames(log) == std::vector<std::string> { "expire:doomed" });
}

TEST_CASE("A generation flush is not reported as an expiry", "[cache][reclaim-reporting]")
{
    // FLUSHDB fires its own whole-database event. Naming every key it swept
    // `expired` on top of that reports a TTL that never lapsed — and tells a
    // subscriber the key aged out when an operator wiped it.
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru;
    lru.SetReclaimLog(&log);

    REQUIRE(lru.Set("kept", Value(8), 0, TimePoint::max()).has_value());
    lru.FlushWithGeneration(TimePoint {});
    REQUIRE(lru.PurgeExpired(TimePoint {}, PurgeBudget::Unbounded()).purged == 1);
    REQUIRE(DrainNames(log).empty());
}

TEST_CASE("A sweep reports the entries whose TTL had passed", "[cache][reclaim-reporting]")
{
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru;
    lru.SetReclaimLog(&log);

    auto const start = TimePoint {};
    auto const expiry = start + std::chrono::seconds { 5 };
    REQUIRE(lru.Set("gone", Value(8), 0, expiry).has_value());
    REQUIRE(lru.Set("stays", Value(8), 0, TimePoint::max()).has_value());

    REQUIRE(lru.PurgeExpired(expiry + std::chrono::seconds { 1 }, PurgeBudget::Unbounded()).purged == 1);
    REQUIRE(DrainNames(log) == std::vector<std::string> { "expire:gone" });
}

TEST_CASE("Routing a log to nullptr stops the reporting", "[cache][reclaim-reporting]")
{
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru { 100 };
    lru.SetReclaimLog(&log);
    lru.SetReclaimLog(nullptr);

    REQUIRE(lru.Set("a", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(lru.Set("b", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE_FALSE(log.HasPending());
}

TEST_CASE("ShardedStorage routes the log to every shard", "[cache][reclaim-reporting]")
{
    AlwaysListening obs;
    ReclaimLog log { &obs };

    std::vector<std::unique_ptr<IStorage>> shards;
    shards.push_back(std::make_unique<InMemoryLruStorage>(100));
    shards.push_back(std::make_unique<InMemoryLruStorage>(100));
    ShardedStorage sharded { std::move(shards) };
    sharded.SetReclaimLog(&log);

    // Enough distinct keys that both shards are driven past their budget; the
    // point is that neither shard is silent, whichever key lands where.
    for (auto const& key: { "a", "b", "c", "d", "e", "f", "g", "h" })
        REQUIRE(sharded.Set(key, Value(80), 0, TimePoint::max()).has_value());

    auto const names = DrainNames(log);
    REQUIRE_FALSE(names.empty());
    REQUIRE(std::ranges::all_of(names, [](auto const& n) { return n.starts_with("evict:"); }));
}

TEST_CASE("Every write verb reclaims the lapsed record it finds, on both backends", "[cache][reclaim-reporting]")
{
    // The same client sequence must not produce different events depending on
    // which backend is configured -- so the two tiers are asserted against each
    // other rather than against a hand-written expectation per verb.
    //
    // The disk tier used to reject a lapsed record on every one of these paths
    // and leave it exactly where it was, because the rule was written for the
    // READ paths, where only a shared lock is held and opening a write
    // transaction would break CowTree's single-writer contract. These verbs all
    // hold the exclusive lock, so the rule never applied to them: the cost was
    // dead bytes on disk until a sweep or a DELETE reached them, and an
    // `expired` event a subscriber got in memory and not on disk.
    FastCache::Testing::ScratchDirectory const dir { "reclaim-write-paths" };
    auto const expiry = TimePoint {} + std::chrono::seconds { 5 };
    auto const later = expiry + std::chrono::seconds { 1 };

    for (auto const& verb: ReclaimingVerbs)
    {
        INFO("verb: " << verb.name);
        AlwaysListening obs;

        ReclaimLog memoryLog { &obs };
        InMemoryLruStorage lru;
        lru.SetReclaimLog(&memoryLog);
        REQUIRE(lru.Set("doomed", Value(8), 0, expiry).has_value());
        REQUIRE(DrainNames(memoryLog).empty());
        verb.invoke(lru, "doomed", later);
        CHECK(DrainNames(memoryLog) == std::vector<std::string> { "expire:doomed" });

        CowTreeStorage::Options opts;
        opts.path = dir / (std::string { verb.name } + ".cow");
        auto tier = CowTreeStorage::Open(opts);
        REQUIRE(tier.has_value());
        ReclaimLog diskLog { &obs };
        (*tier)->SetReclaimLog(&diskLog);
        REQUIRE((*tier)->Set("doomed", Value(8), 0, expiry).has_value());
        REQUIRE(DrainNames(diskLog).empty());
        verb.invoke(**tier, "doomed", later);
        CHECK(DrainNames(diskLog) == std::vector<std::string> { "expire:doomed" });

        // Reported is only half of it: a record left behind would fire
        // `expired` again on the next verb that reached it, which is why
        // reporting without erasing would have been worse than neither. The
        // in-memory tier's count is the reference -- `add` leaves the new
        // record it just wrote, everything else leaves nothing.
        CHECK((*tier)->Snapshot().itemCount == lru.Snapshot().itemCount);
    }
}

TEST_CASE("Evicting entries a flush already made invisible reports nothing", "[cache][reclaim-reporting]")
{
    // FLUSHDB leaves the entries in the LRU; the next write evicts them to get
    // back under budget. That eviction is bookkeeping following an event that
    // already fired, not memory pressure taking a live key.
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru { 100 };
    lru.SetReclaimLog(&log);

    REQUIRE(lru.Set("a", Value(80), 0, TimePoint::max()).has_value());
    lru.FlushWithGeneration(TimePoint {});
    REQUIRE(lru.Set("b", Value(80), 0, TimePoint::max()).has_value());

    // The flushed entry really was evicted — otherwise the silence is vacuous.
    REQUIRE(lru.Snapshot().evictions > 0);
    REQUIRE(DrainNames(log).empty());
}

TEST_CASE("The disk tier does not report evicting what a flush already voided", "[cache][reclaim-reporting]")
{
    FastCache::Testing::ScratchDirectory const dir { "reclaim-cow-flush-evict" };

    CowTreeStorage::Options opts;
    opts.path = dir / "tier.cow";
    opts.maxBytes = 100;
    auto tier = CowTreeStorage::Open(opts);
    REQUIRE(tier.has_value());

    AlwaysListening obs;
    ReclaimLog log { &obs };
    (*tier)->SetReclaimLog(&log);

    REQUIRE((*tier)->Set("a", Value(80), 0, TimePoint::max()).has_value());
    (*tier)->FlushWithGeneration(TimePoint {});
    REQUIRE((*tier)->Set("b", Value(80), 0, TimePoint::max()).has_value());

    REQUIRE((*tier)->Snapshot().evictions > 0);
    REQUIRE(DrainNames(log).empty());
}

TEST_CASE("A LayeredStorage does not report an L2 eviction either", "[cache][reclaim-reporting]")
{
    // The mirror image of the demotion case below. `Get` serves an L1 hit
    // without consulting L2, so a key L2 evicted is still retrievable — and L1
    // is exactly where it will be, because L1 absorbing the hits is what left
    // L2's recency stale enough to evict it.
    FastCache::Testing::ScratchDirectory const dir { "reclaim-layered-l2-evict" };

    CowTreeStorage::Options opts;
    opts.path = dir / "tier.cow";
    opts.maxBytes = 100;
    auto l2 = CowTreeStorage::Open(opts);
    REQUIRE(l2.has_value());

    AlwaysListening obs;
    ReclaimLog log { &obs };
    // L1 unbounded, so it keeps everything L2 is forced to drop.
    LayeredStorage layered { std::make_unique<InMemoryLruStorage>(0), std::move(*l2) };
    layered.SetReclaimLog(&log);

    REQUIRE(layered.Set("a", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(layered.Set("b", Value(80), 0, TimePoint::max()).has_value());

    REQUIRE(layered.L2().Snapshot().evictions > 0);
    REQUIRE(DrainNames(log).empty());

    // And the evicted key is still served, which is what made the event wrong.
    auto const got = layered.Get("a", TimePoint {});
    REQUIRE(got.has_value());
    REQUIRE(got->found);
}

TEST_CASE("A LayeredStorage still reports an L2 expiry", "[cache][reclaim-reporting]")
{
    // An expiry IS total: both tiers hold the same TTL, so the entry the mirror
    // still has is one it will refuse for the same reason L2 reclaimed it.
    FastCache::Testing::ScratchDirectory const dir { "reclaim-layered-l2-expire" };

    CowTreeStorage::Options opts;
    opts.path = dir / "tier.cow";
    auto l2 = CowTreeStorage::Open(opts);
    REQUIRE(l2.has_value());

    AlwaysListening obs;
    ReclaimLog log { &obs };
    LayeredStorage layered { std::make_unique<InMemoryLruStorage>(0), std::move(*l2) };
    layered.SetReclaimLog(&log);

    auto const expiry = TimePoint {} + std::chrono::seconds { 5 };
    REQUIRE(layered.Set("gone", Value(8), 0, expiry).has_value());
    REQUIRE(layered.PurgeExpired(expiry + std::chrono::seconds { 1 }, PurgeBudget::Unbounded()).purged == 1);

    REQUIRE(DrainNames(log) == std::vector<std::string> { "expire:gone" });
}

TEST_CASE("The disk tier reports what it evicted to stay under its budget", "[cache][reclaim-reporting]")
{
    FastCache::Testing::ScratchDirectory const dir { "reclaim-cow-evict" };

    CowTreeStorage::Options opts;
    opts.path = dir / "tier.cow";
    opts.maxBytes = 100;
    auto tier = CowTreeStorage::Open(opts);
    REQUIRE(tier.has_value());

    AlwaysListening obs;
    ReclaimLog log { &obs };
    (*tier)->SetReclaimLog(&log);

    REQUIRE((*tier)->Set("a", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(DrainNames(log).empty());

    REQUIRE((*tier)->Set("b", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(DrainNames(log) == std::vector<std::string> { "evict:a" });
}

TEST_CASE("The disk tier's sweep reports only the entries whose TTL had passed", "[cache][reclaim-reporting]")
{
    FastCache::Testing::ScratchDirectory const dir { "reclaim-cow-purge" };

    CowTreeStorage::Options opts;
    opts.path = dir / "tier.cow";
    auto tier = CowTreeStorage::Open(opts);
    REQUIRE(tier.has_value());

    AlwaysListening obs;
    ReclaimLog log { &obs };
    (*tier)->SetReclaimLog(&log);

    auto const expiry = TimePoint {} + std::chrono::seconds { 5 };
    REQUIRE((*tier)->Set("gone", Value(8), 0, expiry).has_value());
    REQUIRE((*tier)->Set("stays", Value(8), 0, TimePoint::max()).has_value());

    REQUIRE((*tier)->PurgeExpired(expiry + std::chrono::seconds { 1 }, PurgeBudget::Unbounded()).purged == 1);
    REQUIRE(DrainNames(log) == std::vector<std::string> { "expire:gone" });
}

TEST_CASE("A LayeredStorage L1 demotion is not reported as an eviction", "[cache][reclaim-reporting]")
{
    // The whole reason LayeredStorage routes the log to L2 only. L1 is a mirror:
    // when it drops an entry to stay under its RAM budget the key is still on
    // disk and the next read serves it. Reporting that publishes `evicted` for a
    // key that never left the cache, and dirties every WATCH on it.
    FastCache::Testing::ScratchDirectory const dir { "reclaim-layered" };

    CowTreeStorage::Options opts;
    opts.path = dir / "tier.cow";
    // L2 unbounded, so nothing is ever evicted from the authoritative tier.
    opts.maxBytes = 0;
    auto l2 = CowTreeStorage::Open(opts);
    REQUIRE(l2.has_value());

    AlwaysListening obs;
    ReclaimLog log { &obs };
    // L1 holds one 80-byte value; the second Set must demote the first.
    LayeredStorage layered { std::make_unique<InMemoryLruStorage>(100), std::move(*l2) };
    layered.SetReclaimLog(&log);

    REQUIRE(layered.Set("a", Value(80), 0, TimePoint::max()).has_value());
    REQUIRE(layered.Set("b", Value(80), 0, TimePoint::max()).has_value());

    // Asserted before the silence is: an empty log proves nothing unless the
    // demotion this test is about actually happened.
    REQUIRE(layered.L1().Snapshot().evictions > 0);
    REQUIRE(DrainNames(log).empty());

    // And the demoted key is genuinely still served, which is what made the
    // event wrong rather than merely noisy.
    auto const got = layered.Get("a", TimePoint {});
    REQUIRE(got.has_value());
    REQUIRE(got->found);
}

TEST_CASE("A plain Set over a lapsed key reclaims nothing, so reports nothing", "[cache][reclaim-reporting]")
{
    // Set overwrites the record without looking the old one up, so there is no
    // reclaim to report — the key is not gone, it has a new value. Worth a test
    // because "touch the key to find out whether it expired" is the obvious
    // advice and it does not work with the most obvious verb;
    // docs/operations/known-limitations.md says so on the strength of this.
    AlwaysListening obs;
    ReclaimLog log { &obs };
    InMemoryLruStorage lru;
    lru.SetReclaimLog(&log);

    auto const expiry = TimePoint {} + std::chrono::seconds { 5 };
    REQUIRE(lru.Set("k", Value(8), 0, expiry).has_value());
    REQUIRE(DrainNames(log).empty());

    REQUIRE(lru.Set("k", Value(8), 0, TimePoint::max()).has_value());
    REQUIRE(DrainNames(log).empty());

    // Whereas a verb that must find the key first does reclaim and report.
    REQUIRE(lru.Set("j", Value(8), 0, expiry).has_value());
    auto const later = expiry + std::chrono::seconds { 1 };
    REQUIRE_FALSE(lru.Touch("j", TimePoint::max(), later).has_value());
    REQUIRE(DrainNames(log) == std::vector<std::string> { "expire:j" });
}

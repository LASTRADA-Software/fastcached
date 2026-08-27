// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IStorageMutationObserver.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// Observer that only answers the fast probe — `ReclaimLog` consults nothing
/// else, so `OnMutation` is never reached from these tests.
class ProbeObserver final: public FastCache::IStorageMutationObserver
{
  public:
    void OnMutation(FastCache::MutationKind /*kind*/, std::string_view /*key*/) noexcept override
    {
        // Unreachable from these tests: the log records and drains, it never
        // publishes. Firing the events is NotifyingStorage's job, and its own
        // tests cover it.
    }

    [[nodiscard]] bool HasObservers() const noexcept override
    {
        return listening.load(std::memory_order_relaxed);
    }

    std::atomic<bool> listening { true };
};

} // namespace

TEST_CASE("ReclaimLog records nothing worth keeping without an observer", "[cache][reclaim-log]")
{
    FastCache::ReclaimLog log { nullptr };
    REQUIRE_FALSE(log.IsRecording());
}

TEST_CASE("ReclaimLog's recording gate tracks the observer", "[cache][reclaim-log]")
{
    ProbeObserver obs;
    FastCache::ReclaimLog log { &obs };
    REQUIRE(log.IsRecording());

    // The gate is what a tier consults on its eviction path, so it has to
    // follow the last WATCHer disconnecting without the tier being rebuilt.
    obs.listening.store(false, std::memory_order_relaxed);
    REQUIRE_FALSE(log.IsRecording());
}

TEST_CASE("ReclaimLog hands back what it recorded, in order", "[cache][reclaim-log]")
{
    ProbeObserver obs;
    FastCache::ReclaimLog log { &obs };
    REQUIRE_FALSE(log.HasPending());

    log.Record(FastCache::MutationKind::Expire, "lapsed");
    log.Record(FastCache::MutationKind::Evict, "cold");
    REQUIRE(log.HasPending());

    std::vector<FastCache::ReclaimedKey> drained;
    log.Drain(drained);

    REQUIRE(drained.size() == 2);
    REQUIRE(drained[0].kind == FastCache::MutationKind::Expire);
    REQUIRE(drained[0].key == "lapsed");
    REQUIRE(drained[1].kind == FastCache::MutationKind::Evict);
    REQUIRE(drained[1].key == "cold");

    // Drained means gone: a second drain must not replay the same events, or
    // every subsequent storage call would re-publish the whole history.
    REQUIRE_FALSE(log.HasPending());
    log.Drain(drained);
    REQUIRE(drained.empty());
}

TEST_CASE("ReclaimLog copies the key it is handed", "[cache][reclaim-log]")
{
    // The caller is mid-erase of the node that owns the key, so the log cannot
    // hold the view it was given.
    ProbeObserver obs;
    FastCache::ReclaimLog log { &obs };

    std::string doomed { "about-to-die" };
    log.Record(FastCache::MutationKind::Expire, doomed);
    doomed.clear();
    doomed.shrink_to_fit();

    std::vector<FastCache::ReclaimedKey> drained;
    log.Drain(drained);
    REQUIRE(drained.size() == 1);
    REQUIRE(drained[0].key == "about-to-die");
}

TEST_CASE("ReclaimLog empties the caller's buffer before filling it", "[cache][reclaim-log]")
{
    // NotifyingStorage drains into one reused buffer; if Drain appended, every
    // call would re-fire every event drained before it.
    ProbeObserver obs;
    FastCache::ReclaimLog log { &obs };

    std::vector<FastCache::ReclaimedKey> reused;
    log.Record(FastCache::MutationKind::Evict, "first");
    log.Drain(reused);
    REQUIRE(reused.size() == 1);

    log.Record(FastCache::MutationKind::Evict, "second");
    log.Drain(reused);
    REQUIRE(reused.size() == 1);
    REQUIRE(reused[0].key == "second");
}

TEST_CASE("ReclaimLog drops past its capacity rather than growing", "[cache][reclaim-log]")
{
    // A Resize() onto a smaller budget evicts until it fits, which on a large
    // cache is one call reclaiming millions of keys. Buffering all of them to
    // publish events would spike memory exactly when the operator asked for
    // less of it.
    ProbeObserver obs;
    FastCache::ReclaimLog log { &obs, nullptr, 3 };

    for (auto const index: std::views::iota(0, 10))
        log.Record(FastCache::MutationKind::Evict, std::format("k{}", index));

    REQUIRE(log.Dropped() == 7);

    std::vector<FastCache::ReclaimedKey> drained;
    log.Drain(drained);
    REQUIRE(drained.size() == 3);
    REQUIRE(drained[0].key == "k0");
    REQUIRE(drained[2].key == "k2");

    // Draining makes room again, and the drop tally is cumulative — it says
    // "events were lost", which a reset would erase.
    log.Record(FastCache::MutationKind::Expire, "after");
    REQUIRE(log.Dropped() == 7);
    log.Drain(drained);
    REQUIRE(drained.size() == 1);
    REQUIRE(drained[0].key == "after");
}

TEST_CASE("ReclaimLog reports its drops to the metrics sink", "[cache][reclaim-log]")
{
    // A count only this class can read is one no operator can. The bounded loss
    // is defensible precisely because it is accountable.
    ProbeObserver obs;
    FastCache::AtomicMetricsSink metrics;
    FastCache::ReclaimLog log { &obs, &metrics, 1 };

    log.Record(FastCache::MutationKind::Evict, "kept");
    log.Record(FastCache::MutationKind::Evict, "lost");
    log.Record(FastCache::MutationKind::Evict, "also-lost");

    REQUIRE(log.Dropped() == 2);
    REQUIRE(metrics.Read(FastCache::IMetricsSink::Counter::KeyspaceReclaimEventsDropped) == 2);
}

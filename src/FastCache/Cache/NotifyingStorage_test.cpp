// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IStorageMutationObserver.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

/// Minimal capturing observer used by the NotifyingStorage tests. Records
/// every (kind, key) pair so assertions can verify the decorator's fan-out
/// shape without depending on the production WATCH / keyspace machinery.
class RecordingObserver final: public FastCache::IStorageMutationObserver
{
  public:
    struct Record
    {
        FastCache::MutationKind kind;
        std::string key;
    };

    void OnMutation(FastCache::MutationKind kind, std::string_view key) noexcept override
    {
        records.push_back(Record { .kind = kind, .key = std::string { key } });
    }

    [[nodiscard]] bool HasObservers() const noexcept override
    {
        return hasObservers.load(std::memory_order_relaxed);
    }

    std::vector<Record> records;
    std::atomic<bool> hasObservers { true };
};

} // namespace

TEST_CASE("NotifyingStorage fires Set on successful Set", "[cache][notifying-storage]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    FastCache::NotifyingStorage notifying { inner, &obs };

    auto const cas = notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());
    REQUIRE(obs.records.size() == 1);
    REQUIRE(obs.records[0].kind == FastCache::MutationKind::Set);
    REQUIRE(obs.records[0].key == "k");
}

TEST_CASE("NotifyingStorage does NOT fire when inner storage returns an error",
          "[cache][notifying-storage]")
{
    // Add against a key that already exists -> StorageError(KeyExists).
    // The observer must NOT see a Set event for a failed mutation.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    FastCache::NotifyingStorage notifying { inner, &obs };

    // Pre-populate via the decorator so the first record is a Set; clear
    // and then attempt the duplicate Add.
    REQUIRE(notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max()).has_value());
    obs.records.clear();

    auto const result = notifying.Add("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(result.has_value());
    REQUIRE(obs.records.empty());
}

TEST_CASE("NotifyingStorage HasObservers fast probe skips OnMutation when nothing is listening",
          "[cache][notifying-storage]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    obs.hasObservers.store(false, std::memory_order_relaxed);
    FastCache::NotifyingStorage notifying { inner, &obs };

    auto const result = notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max());
    REQUIRE(result.has_value());
    // Even though the inner mutation succeeded, the observer's HasObservers
    // probe returned false, so OnMutation was skipped.
    REQUIRE(obs.records.empty());
}

TEST_CASE("NotifyingStorage with a null observer forwards every call to the inner storage",
          "[cache][notifying-storage]")
{
    // Defensive shape: a daemon that never wires an observer (e.g. tests
    // that just want a passthrough) must not crash on Notify.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    FastCache::NotifyingStorage notifying { inner, nullptr };

    REQUIRE(notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max()).has_value());
    REQUIRE(notifying.Delete("k", clock.Now()).has_value());
}

TEST_CASE("NotifyingStorage fires Delete on successful Delete", "[cache][notifying-storage]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    FastCache::NotifyingStorage notifying { inner, &obs };

    REQUIRE(notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max()).has_value());
    obs.records.clear();

    REQUIRE(notifying.Delete("k", clock.Now()).has_value());
    REQUIRE(obs.records.size() == 1);
    REQUIRE(obs.records[0].kind == FastCache::MutationKind::Delete);
    REQUIRE(obs.records[0].key == "k");
}

TEST_CASE("NotifyingStorage fires FlushDb with an empty key on FlushWithGeneration",
          "[cache][notifying-storage][flushdb]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    FastCache::NotifyingStorage notifying { inner, &obs };

    REQUIRE(notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max()).has_value());
    obs.records.clear();

    notifying.FlushWithGeneration(clock.Now());
    REQUIRE(obs.records.size() == 1);
    REQUIRE(obs.records[0].kind == FastCache::MutationKind::FlushDb);
    REQUIRE(obs.records[0].key.empty()); // whole-database event
}

TEST_CASE("NotifyingStorage forwards Peek without firing an event",
          "[cache][notifying-storage]")
{
    // Peek is non-mutating; the observer must not be invoked.
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    FastCache::NotifyingStorage notifying { inner, &obs };
    REQUIRE(notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max()).has_value());
    obs.records.clear();

    auto const peek = notifying.Peek("k", clock.Now());
    REQUIRE(peek.has_value());
    REQUIRE(peek->found);
    REQUIRE(obs.records.empty());
}

TEST_CASE("NotifyingStorage Update fires Update on Store, Delete on Delete, nothing on Unchanged",
          "[cache][notifying-storage]")
{
    FastCache::ManualClock clock;
    FastCache::InMemoryLruStorage inner;
    RecordingObserver obs;
    FastCache::NotifyingStorage notifying { inner, &obs };
    REQUIRE(notifying.Set("k", std::vector<std::byte>(8), 0, FastCache::TimePoint::max()).has_value());
    obs.records.clear();

    // Store outcome -> Update event.
    auto const stored = notifying.Update(
        "k",
        [](FastCache::GetResult const&) {
            return FastCache::IStorage::UpdateOutcome { .value = std::vector<std::byte>(4),
                                                       .flags = 0,
                                                       .action = FastCache::IStorage::UpdateAction::Store };
        },
        clock.Now());
    REQUIRE(stored.has_value());
    REQUIRE(obs.records.size() == 1);
    REQUIRE(obs.records[0].kind == FastCache::MutationKind::Update);

    // Unchanged outcome -> no event.
    obs.records.clear();
    auto const unchanged = notifying.Update(
        "k",
        [](FastCache::GetResult const&) {
            return FastCache::IStorage::UpdateOutcome { .value = {}, .flags = 0,
                .action = FastCache::IStorage::UpdateAction::Unchanged };
        },
        clock.Now());
    REQUIRE(unchanged.has_value());
    REQUIRE(obs.records.empty());

    // Delete outcome -> Delete event.
    auto const deleted = notifying.Update(
        "k",
        [](FastCache::GetResult const&) {
            return FastCache::IStorage::UpdateOutcome { .value = {}, .flags = 0,
                .action = FastCache::IStorage::UpdateAction::Delete };
        },
        clock.Now());
    REQUIRE(deleted.has_value());
    REQUIRE(obs.records.size() == 1);
    REQUIRE(obs.records[0].kind == FastCache::MutationKind::Delete);
}

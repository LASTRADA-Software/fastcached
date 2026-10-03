// SPDX-License-Identifier: Apache-2.0
#include "AtomicFile.hpp"
#include "ReachabilityMemo.hpp"
#include "ReachabilityMemoTestSupport.hpp"
#include "Stats.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache::Cc;
using namespace std::chrono_literals;

namespace
{
/// A fixed instant, so a stamp is a value a case can name.
constexpr std::chrono::system_clock::time_point Noon { std::chrono::seconds { 1'767'225'600 } };
constexpr std::string_view Scheduler = "sched.corp:6674";
} // namespace

TEST_CASE("An unreachable scheduler is remembered for its TTL and not a moment longer", "[memo]")
{
    ReachabilityMemo memo;
    memo.Note(MemoKind::SchedulerUnreached, Scheduler, Noon);
    auto const ttl = MemoKinds[static_cast<std::size_t>(MemoKind::SchedulerUnreached)].ttl;
    CHECK(ttl == 15s);
    CHECK(memo.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon + ttl - 1ms));
    CHECK_FALSE(memo.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon + ttl));
    // Keyed on the endpoint: another scheduler is not remembered as unreachable.
    CHECK_FALSE(memo.Remembers(MemoKind::SchedulerUnreached, "other:6674", Noon));
}

TEST_CASE("A memo stamped before a long sleep, or in the future, keeps nothing off", "[memo]")
{
    ReachabilityMemo memo;
    memo.Note(MemoKind::SchedulerUnreached, Scheduler, Noon);

    // More than an hour asleep: long expired, whatever the power event said or did not.
    CHECK(ReachabilityMemo::AgeOf(memo.Entries().front(), Noon + 61min) == MemoAge::Expired);
    CHECK_FALSE(memo.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon + 61min));

    // A wall clock stepped BACKWARDS makes the stamp lie in the future. Its own outcome,
    // decided where the subtraction happens, and it reads as stale -- never as the
    // freshest entry there is, which is what a negative age would otherwise look like.
    CHECK(ReachabilityMemo::AgeOf(memo.Entries().front(), Noon - 5min) == MemoAge::FromTheFuture);
    CHECK_FALSE(memo.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon - 5min));
}

TEST_CASE("Unreachable workers are capped at the lease's exclusion limit, newest kept", "[memo]")
{
    ReachabilityMemo memo;
    for (auto const i: std::views::iota(0, 20))
        memo.Note(MemoKind::WorkerUnreached, std::format("w{}:6676", i), Noon + std::chrono::seconds { i });
    auto const fresh = memo.Fresh(MemoKind::WorkerUnreached, Noon + 20s);
    REQUIRE(fresh.size() == FastCache::CompileCacheWire::MaxLeaseExclusions);
    CHECK(fresh.front() == "w19:6676"); // newest first
    CHECK(fresh.back() == "w4:6676");

    // Noting an endpoint again moves it, it does not duplicate it.
    memo.Note(MemoKind::WorkerUnreached, "w4:6676", Noon + 21s);
    auto const moved = memo.Fresh(MemoKind::WorkerUnreached, Noon + 21s);
    CHECK(moved.size() == FastCache::CompileCacheWire::MaxLeaseExclusions);
    CHECK(moved.front() == "w4:6676");

    // A worker entry lives about a minute.
    CHECK(memo.Fresh(MemoKind::WorkerUnreached, Noon + 21s + 60s).empty());
}

TEST_CASE("A memo round-trips, and a file it cannot read is an empty memo", "[memo]")
{
    ReachabilityMemo memo;
    memo.Note(MemoKind::SchedulerUnreached, Scheduler, Noon);
    memo.Note(MemoKind::WorkerUnreached, "[fe80::1]:6676", Noon);
    auto const back = ReachabilityMemo::Parse(memo.Serialize());
    CHECK(back.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon));
    CHECK(back.Remembers(MemoKind::WorkerUnreached, "[fe80::1]:6676", Noon));

    // Torn, empty, garbage and a later build's header all read as NOTHING remembered --
    // the direction that dials, which is what the launcher did before the memo existed.
    for (auto const* text:
         { "", "fastcache-cc reach", "\x01\x02 garbage", "fastcache-cc reachability 99\nscheduler-unreached 1 x:1\n" })
        CHECK(ReachabilityMemo::Parse(text).Entries().empty());

    // A line whose kind this build does not know is skipped; its neighbours survive.
    auto const mixed = ReachabilityMemo::Parse(
        std::format("fastcache-cc reachability 1\nfuture-kind 1 x:1\nscheduler-unreached {} {}\n",
                    std::chrono::duration_cast<std::chrono::milliseconds>(Noon.time_since_epoch()).count(),
                    Scheduler));
    CHECK(mixed.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon));
}

TEST_CASE("Only a dial that reached NOTHING at the configured scheduler is remembered", "[memo]")
{
    auto const leaseEndedWith = [](TransportFailure failure, std::string endpoint) {
        DispatchResult result;
        result.status = DispatchStatus::Unavailable;
        result.leaseTransport = failure;
        result.leaseEndpoint = std::move(endpoint);
        return result;
    };

    ReachabilityMemo memo;
    memo.Absorb(leaseEndedWith(TransportFailure::Unreached, std::string { Scheduler }), Scheduler, Noon);
    CHECK(memo.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon));

    // A leader the configured one REDIRECTED to: the configured one answered, so it is
    // not the machine that is down, and nothing may be recorded against it.
    ReachabilityMemo redirected;
    redirected.Absorb(leaseEndedWith(TransportFailure::Unreached, "leader:6674"), Scheduler, Noon);
    CHECK(redirected.Entries().empty());

    // A peer that was reached and then went wrong is not an unreachable one.
    for (auto const failure: { TransportFailure::PeerLost, TransportFailure::Expired, TransportFailure::Silent })
    {
        ReachabilityMemo reached;
        reached.Absorb(leaseEndedWith(failure, std::string { Scheduler }), Scheduler, Noon);
        CHECK(reached.Entries().empty());
    }

    // A lease exchange that completed clears the entry: the scheduler is back.
    memo.Absorb(leaseEndedWith(TransportFailure::None, std::string { Scheduler }), Scheduler, Noon + 1s);
    CHECK_FALSE(memo.Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon + 1s));
    CHECK(memo.Changed());
}

TEST_CASE("A worker nothing reached is remembered, and one that compiled is forgotten", "[memo]")
{
    ReachabilityMemo memo;
    DispatchResult unreached;
    unreached.status = DispatchStatus::Unavailable;
    unreached.unreachedWorker = "laptop.corp:6676";
    memo.Absorb(unreached, Scheduler, Noon);
    CHECK(memo.Remembers(MemoKind::WorkerUnreached, "laptop.corp:6676", Noon));

    DispatchResult compiled;
    compiled.status = DispatchStatus::Compiled;
    compiled.workerEndpoint = "laptop.corp:6676";
    memo.Absorb(compiled, Scheduler, Noon + 1s);
    CHECK_FALSE(memo.Remembers(MemoKind::WorkerUnreached, "laptop.corp:6676", Noon + 1s));
}

TEST_CASE("A memo is saved only when it changed, and the file store round-trips", "[memo]")
{
    Testing::InMemoryMemoStore store;
    auto memo = ReachabilityMemo::Load(store);
    memo.SaveIfChanged(store);
    CHECK(store.Writes() == 0);
    memo.Note(MemoKind::SchedulerUnreached, Scheduler, Noon);
    memo.SaveIfChanged(store);
    CHECK(store.Writes() == 1);
    CHECK(ReachabilityMemo::Load(store).Remembers(MemoKind::SchedulerUnreached, Scheduler, Noon));

    FastCache::Testing::ScratchDirectory const scratch { "fc-cc-memo" };
    auto const disk = MakeDiskFiles();
    FileMemoStore file { scratch / "reachability.memo", *disk };
    CHECK_FALSE(file.Read().has_value()); // absent is nothing, not an error
    REQUIRE(file.Write("fastcache-cc reachability 1\n"));
    CHECK(file.Read() == std::optional<std::string> { "fastcache-cc reachability 1\n" });
}

TEST_CASE("A stamp at either end of its range is never read as fresh, and never overflows", "[memo]")
{
    // The stamp is FILE content, so it can be anything an int64 spells. An age taken by
    // subtracting first overflows for a stamp near the minimum -- undefined behaviour,
    // and on a wrapping build a huge past stamp came out "from the future".
    auto const nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(Noon.time_since_epoch()).count();
    auto const kind = MemoKinds[static_cast<std::size_t>(MemoKind::SchedulerUnreached)].token;

    // A negative stamp is not one this build writes: the line is dropped, and an entry
    // that cannot be read means "dial".
    auto const lowest = ReachabilityMemo::Parse(
        std::format("fastcache-cc reachability 1\n{} {} x:1\n", kind, std::numeric_limits<std::int64_t>::min()));
    CHECK(lowest.Entries().empty());
    CHECK_FALSE(lowest.Remembers(MemoKind::SchedulerUnreached, "x:1", Noon));

    // The furthest future a file can name: from the future, never fresh.
    auto const highest = ReachabilityMemo::Parse(
        std::format("fastcache-cc reachability 1\n{} {} x:1\n", kind, std::numeric_limits<std::int64_t>::max()));
    REQUIRE(highest.Entries().size() == 1);
    CHECK(ReachabilityMemo::AgeOf(highest.Entries().front(), Noon) == MemoAge::FromTheFuture);
    CHECK_FALSE(highest.Remembers(MemoKind::SchedulerUnreached, "x:1", Noon));

    // And AgeOf itself, handed the extremes directly -- the comparison comes before the
    // subtraction, so neither end can overflow whatever reached the entry.
    auto const entryAt = [](std::int64_t stamp) {
        return MemoEntry { .kind = MemoKind::SchedulerUnreached, .stampedUnixMs = stamp, .endpoint = "x:1" };
    };
    CHECK(ReachabilityMemo::AgeOf(entryAt(std::numeric_limits<std::int64_t>::min()), Noon) == MemoAge::Expired);
    CHECK(ReachabilityMemo::AgeOf(entryAt(std::numeric_limits<std::int64_t>::max()), Noon) == MemoAge::FromTheFuture);
    CHECK(ReachabilityMemo::AgeOf(entryAt(0), Noon) == MemoAge::Expired);
    CHECK(ReachabilityMemo::AgeOf(entryAt(nowMs), Noon) == MemoAge::Fresh);
}

TEST_CASE("The file store replaces an existing memo and leaves no temp file behind", "[memo]")
{
    // The atomicity argument rests on the rename REPLACING a file that is already there
    // -- the ordinary case, since every launcher after the first writes over the last one.
    FastCache::Testing::ScratchDirectory const scratch { "fc-cc-memo-replace" };
    auto const disk = MakeDiskFiles();
    FileMemoStore file { scratch / "reachability.memo", *disk };
    REQUIRE(file.Write("fastcache-cc reachability 1\nscheduler-unreached 1 first:1\n"));
    REQUIRE(file.Write("fastcache-cc reachability 1\nscheduler-unreached 2 second:1\n"));
    CHECK(file.Read() == std::optional<std::string> { "fastcache-cc reachability 1\nscheduler-unreached 2 second:1\n" });

    std::vector<std::string> names;
    for (auto const& item: std::filesystem::directory_iterator { scratch.Path() })
        names.push_back(item.path().filename().string());
    CHECK(names == std::vector<std::string> { "reachability.memo" });
}

TEST_CASE("A dispatch skipped on the memo is recorded as the fleet being unreachable", "[memo]")
{
    // Not `Refused`: that one says this machine is at fault, and the reports send an
    // operator to fix the command line. The scheduler being down is `Unreachable`, and the
    // FIXED reason says the launcher remembered it rather than dialling again.
    constexpr auto recording = RememberedUnreachableRecording();
    STATIC_CHECK(recording.outcome == DispatchOutcome::Unreachable);
    CHECK(recording.reason == RememberedUnreachableReason);
    CHECK(ToStringView(recording.outcome) == ToStringView(DispatchOutcome::Unreachable));
}

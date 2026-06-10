// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/StreamCodec.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace FastCache;
using StreamCodec::StreamId;

namespace
{

/// Fixture wiring a CacheEngine over an in-memory store with manual clocks, so
/// stream entry-ID timestamps and PEL idle times are fully deterministic.
struct StreamEngineFixture
{
    InMemoryLruStorage storage;
    ManualClock clock;
    /// Start the wall clock at 1000ms past the epoch so generated IDs read as
    /// `1000-0`, `1000-1`, ... — small, readable, and stable across runs.
    ManualWallClock wallClock { std::chrono::system_clock::time_point { std::chrono::milliseconds { 1000 } } };
    CacheEngine engine { storage, clock, wallClock };

    /// Advance the wall clock by `ms` milliseconds.
    void AdvanceMs(std::uint64_t ms)
    {
        wallClock.Advance(std::chrono::milliseconds { ms });
    }
};

using Field = std::pair<std::string, std::string>;

[[nodiscard]] std::vector<Field> Fields(std::string_view k, std::string_view v)
{
    return { Field { std::string { k }, std::string { v } } };
}

} // namespace

TEST_CASE("CacheEngine stream: XADD auto-ID uses wall-clock ms and sequences within a ms", "[cache][stream]")
{
    StreamEngineFixture fix;
    auto const fields = Fields("f", "v");

    auto const id1 = fix.engine.StreamAdd("s", std::nullopt, false, fields, std::nullopt, false);
    REQUIRE(id1.has_value());
    REQUIRE(*id1 == StreamId { .ms = 1000, .seq = 0 });

    // Same millisecond -> sequence advances.
    auto const id2 = fix.engine.StreamAdd("s", std::nullopt, false, fields, std::nullopt, false);
    REQUIRE(id2.has_value());
    REQUIRE(*id2 == StreamId { .ms = 1000, .seq = 1 });

    // New millisecond -> sequence resets.
    fix.AdvanceMs(5);
    auto const id3 = fix.engine.StreamAdd("s", std::nullopt, false, fields, std::nullopt, false);
    REQUIRE(id3.has_value());
    REQUIRE(*id3 == StreamId { .ms = 1005, .seq = 0 });

    auto const len = fix.engine.StreamLen("s");
    REQUIRE(len.has_value());
    REQUIRE(*len == 3);
}

TEST_CASE("CacheEngine stream: explicit IDs must be strictly increasing", "[cache][stream]")
{
    StreamEngineFixture fix;
    auto const fields = Fields("f", "v");

    REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = 5, .seq = 5 }, false, fields, std::nullopt, false).has_value());

    // Equal or lower ID is rejected.
    auto const dup = fix.engine.StreamAdd("s", StreamId { .ms = 5, .seq = 5 }, false, fields, std::nullopt, false);
    REQUIRE_FALSE(dup.has_value());
    REQUIRE(dup.error().code == StorageErrorCode::InvalidArgument);

    // 0-0 is always illegal.
    auto const zero = fix.engine.StreamAdd("z", StreamId::Min(), false, fields, std::nullopt, false);
    REQUIRE_FALSE(zero.has_value());

    // A strictly greater ID is accepted.
    REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = 5, .seq = 6 }, false, fields, std::nullopt, false).has_value());
}

TEST_CASE("CacheEngine stream: NOMKSTREAM does not create an absent stream", "[cache][stream]")
{
    StreamEngineFixture fix;
    auto const r = fix.engine.StreamAdd("nope", std::nullopt, false, Fields("f", "v"), std::nullopt, /*noMkStream*/ true);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == StorageErrorCode::KeyNotFound);
    REQUIRE(fix.engine.StreamLen("nope").value() == 0);
}

TEST_CASE("CacheEngine stream: WRONGTYPE against a string key", "[cache][stream]")
{
    StreamEngineFixture fix;
    REQUIRE(fix.engine.Set("k", {}, 0, 0).has_value());
    auto const r = fix.engine.StreamAdd("k", std::nullopt, false, Fields("f", "v"), std::nullopt, false);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == StorageErrorCode::WrongType);
    REQUIRE_FALSE(fix.engine.StreamLen("k").has_value());
}

TEST_CASE("CacheEngine stream: XRANGE / XREVRANGE honour bounds and count", "[cache][stream]")
{
    StreamEngineFixture fix;
    for (auto const ms: { 10U, 20U, 30U, 40U })
        REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = ms, .seq = 0 }, false, Fields("f", "v"), std::nullopt, false)
                    .has_value());

    auto const all = fix.engine.StreamRange("s", StreamId::Min(), StreamId::Max(), 0, false);
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 4);
    REQUIRE(all->front().id == StreamId { .ms = 10, .seq = 0 });

    auto const mid = fix.engine.StreamRange("s", StreamId { .ms = 20, .seq = 0 }, StreamId { .ms = 30, .seq = 0 }, 0, false);
    REQUIRE(mid->size() == 2);

    auto const capped = fix.engine.StreamRange("s", StreamId::Min(), StreamId::Max(), 2, false);
    REQUIRE(capped->size() == 2);
    REQUIRE(capped->front().id == StreamId { .ms = 10, .seq = 0 });

    auto const rev = fix.engine.StreamRange("s", StreamId::Min(), StreamId::Max(), 2, true);
    REQUIRE(rev->size() == 2);
    REQUIRE(rev->front().id == StreamId { .ms = 40, .seq = 0 });
}

TEST_CASE("CacheEngine stream: XREAD returns entries strictly after the cursor", "[cache][stream]")
{
    StreamEngineFixture fix;
    for (auto const ms: { 10U, 20U, 30U })
        REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = ms, .seq = 0 }, false, Fields("f", "v"), std::nullopt, false)
                    .has_value());

    auto const after10 = fix.engine.StreamRead("s", StreamId { .ms = 10, .seq = 0 }, 0);
    REQUIRE(after10.has_value());
    REQUIRE(after10->size() == 2);
    REQUIRE(after10->front().id == StreamId { .ms = 20, .seq = 0 });

    auto const after30 = fix.engine.StreamRead("s", StreamId { .ms = 30, .seq = 0 }, 0);
    REQUIRE(after30->empty());
}

TEST_CASE("CacheEngine stream: XDEL and XTRIM evict entries and track max-deleted", "[cache][stream]")
{
    StreamEngineFixture fix;
    for (auto const ms: { 10U, 20U, 30U, 40U })
        REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = ms, .seq = 0 }, false, Fields("f", "v"), std::nullopt, false)
                    .has_value());

    std::vector<StreamId> const toDel { StreamId { .ms = 20, .seq = 0 } };
    auto const deleted = fix.engine.StreamDelete("s", toDel);
    REQUIRE(deleted.has_value());
    REQUIRE(*deleted == 1);
    REQUIRE(fix.engine.StreamLen("s").value() == 3);

    auto const trimmed = fix.engine.StreamTrimTo(
        "s", CacheEngine::StreamTrim { .strategy = CacheEngine::StreamTrim::Strategy::MaxLen, .threshold = 1 });
    REQUIRE(trimmed.has_value());
    REQUIRE(*trimmed == 2); // keeps only the newest (40-0)
    auto const remaining = fix.engine.StreamRange("s", StreamId::Min(), StreamId::Max(), 0, false);
    REQUIRE(remaining->size() == 1);
    REQUIRE(remaining->front().id == StreamId { .ms = 40, .seq = 0 });
}

TEST_CASE("CacheEngine stream: consumer group read advances the cursor and fills the PEL", "[cache][stream]")
{
    StreamEngineFixture fix;
    auto const fields = Fields("f", "v");
    for (auto const ms: { 10U, 20U, 30U })
        REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = ms, .seq = 0 }, false, fields, std::nullopt, false).has_value());

    REQUIRE(fix.engine.StreamGroupCreate("s", "g1", CacheEngine::GroupStart::Beginning, {}, false).has_value());

    // First `>` read delivers the whole backlog into the PEL.
    auto const read1 = fix.engine.StreamReadGroup("s", "g1", "c1", std::nullopt, 0, false);
    REQUIRE(read1.has_value());
    REQUIRE(read1->size() == 3);

    auto const pending = fix.engine.StreamPendingSummary("s", "g1");
    REQUIRE(pending.has_value());
    REQUIRE(pending->count == 3);
    REQUIRE(pending->minId == StreamId { .ms = 10, .seq = 0 });
    REQUIRE(pending->maxId == StreamId { .ms = 30, .seq = 0 });

    // A second `>` read sees nothing new.
    auto const read2 = fix.engine.StreamReadGroup("s", "g1", "c1", std::nullopt, 0, false);
    REQUIRE(read2->empty());

    // ACK two; one remains pending.
    std::vector<StreamId> const ack { StreamId { .ms = 10, .seq = 0 }, StreamId { .ms = 20, .seq = 0 } };
    auto const acked = fix.engine.StreamAck("s", "g1", ack);
    REQUIRE(acked.has_value());
    REQUIRE(*acked == 2);
    REQUIRE(fix.engine.StreamPendingSummary("s", "g1").value().count == 1);
}

TEST_CASE("CacheEngine stream: reading an unknown group is NOGROUP (KeyNotFound)", "[cache][stream]")
{
    StreamEngineFixture fix;
    REQUIRE(fix.engine.StreamAdd("s", std::nullopt, false, Fields("f", "v"), std::nullopt, false).has_value());
    auto const r = fix.engine.StreamReadGroup("s", "absent", "c1", std::nullopt, 0, false);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == StorageErrorCode::KeyNotFound);
}

TEST_CASE("CacheEngine stream: XCLAIM transfers PEL ownership after the idle threshold", "[cache][stream]")
{
    StreamEngineFixture fix;
    auto const fields = Fields("f", "v");
    REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = 10, .seq = 0 }, false, fields, std::nullopt, false).has_value());
    REQUIRE(fix.engine.StreamGroupCreate("s", "g1", CacheEngine::GroupStart::Beginning, {}, false).has_value());
    REQUIRE(fix.engine.StreamReadGroup("s", "g1", "c1", std::nullopt, 0, false).has_value());

    // Let the entry idle for 5s.
    fix.AdvanceMs(5000);
    std::vector<StreamId> const ids { StreamId { .ms = 10, .seq = 0 } };

    // Below the idle threshold -> not claimed.
    auto const tooSoon = fix.engine.StreamClaim("s", "g1", "c2", /*minIdleMs*/ 10000, ids, false);
    REQUIRE(tooSoon.has_value());
    REQUIRE(tooSoon->ids.empty());

    // Past the threshold -> claimed by c2.
    auto const claimed = fix.engine.StreamClaim("s", "g1", "c2", /*minIdleMs*/ 1000, ids, false);
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->ids.size() == 1);
    REQUIRE(claimed->entries.size() == 1);

    auto const pend =
        fix.engine.StreamPendingRange("s", "g1", StreamId::Min(), StreamId::Max(), 0, std::string_view { "c2" }, 0);
    REQUIRE(pend.has_value());
    REQUIRE(pend->size() == 1);
    REQUIRE(pend->front().consumer == "c2");
}

TEST_CASE("CacheEngine stream: XINFO reports length, watermarks and groups", "[cache][stream]")
{
    StreamEngineFixture fix;
    auto const fields = Fields("f", "v");
    REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = 10, .seq = 0 }, false, fields, std::nullopt, false).has_value());
    REQUIRE(fix.engine.StreamAdd("s", StreamId { .ms = 20, .seq = 0 }, false, fields, std::nullopt, false).has_value());
    REQUIRE(fix.engine.StreamGroupCreate("s", "g1", CacheEngine::GroupStart::End, {}, false).has_value());

    auto const infoResult = fix.engine.StreamInfoOf("s");
    REQUIRE(infoResult.has_value());
    auto const& info = infoResult.value();
    REQUIRE(info.length == 2);
    REQUIRE(info.lastId == StreamId { .ms = 20, .seq = 0 });
    REQUIRE(info.entriesAdded == 2);
    REQUIRE(info.groupCount == 1);
    REQUIRE(info.first.has_value());
    REQUIRE(info.last.has_value());
    if (info.first.has_value() && info.last.has_value())
    {
        REQUIRE(info.first->id == StreamId { .ms = 10, .seq = 0 });
        REQUIRE(info.last->id == StreamId { .ms = 20, .seq = 0 });
    }

    auto const groups = fix.engine.StreamGroupInfo("s");
    REQUIRE(groups.has_value());
    REQUIRE(groups->size() == 1);
    REQUIRE(groups->front().name == "g1");
    REQUIRE(groups->front().lastDelivered == StreamId { .ms = 20, .seq = 0 }); // `$` start
}

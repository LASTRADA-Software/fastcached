// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

using FastCache::Testing::MakeBytes;

namespace
{

bool ContainsSubstr(std::string const& s, std::string_view needle) noexcept
{
    return s.contains(needle);
}

} // namespace

TEST_CASE("TracingStorage emits one Trace line per Get with HIT outcome", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    REQUIRE(inner.Set("foo", MakeBytes("bar"), 0, FastCache::TimePoint::max()).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const got = tracer.Get("foo", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].level == FastCache::LogLevel::Trace);
    REQUIRE(ContainsSubstr(records[0].message, "storage: GET"));
    REQUIRE(ContainsSubstr(records[0].message, "key=foo"));
    REQUIRE(ContainsSubstr(records[0].message, "result=HIT"));
    REQUIRE(ContainsSubstr(records[0].message, "bytes=3"));
}

TEST_CASE("TracingStorage prefixes the line with the published source tag", "[tracing][logsource]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;
    FastCache::TracingStorage tracer { inner, logger, clock };

    // A handler publishes the client tag immediately before the (synchronous)
    // storage call; the trace line carries it. Restore to empty afterwards.
    FastCache::Detail::StorageSourceTag = "[203.0.113.7]";
    auto const got = tracer.Get("foo", clock.Now());
    FastCache::Detail::StorageSourceTag = {};
    REQUIRE(got.has_value());

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].message.starts_with("[203.0.113.7] storage: GET key=foo"));
}

TEST_CASE("TracingStorage omits the prefix when no source tag is published", "[tracing][logsource]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;
    FastCache::TracingStorage tracer { inner, logger, clock };

    FastCache::Detail::StorageSourceTag = {}; // no source
    REQUIRE(tracer.Get("foo", clock.Now()).has_value());

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].message.starts_with("storage: GET key=foo"));
}

TEST_CASE("TracingStorage emits MISS for missing key", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const got = tracer.Get("absent", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->found);

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "result=MISS"));
}

TEST_CASE("TracingStorage emits STORED for Set", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const cas = tracer.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
    REQUIRE(cas.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: SET"));
    REQUIRE(ContainsSubstr(records[0].message, "result=STORED"));
    REQUIRE(ContainsSubstr(records[0].message, "bytes=1"));
}

TEST_CASE("TracingStorage emits NOT_STORED for Add of existing key", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    REQUIRE(inner.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const r = tracer.Add("k", MakeBytes("x"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: ADD"));
    REQUIRE(ContainsSubstr(records[0].message, "result=NOT_STORED"));
}

TEST_CASE("TracingStorage emits no records when MinLevel above Trace", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Info };
    FastCache::ManualClock clock;

    FastCache::TracingStorage tracer { inner, logger, clock };
    (void) tracer.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max());
    (void) tracer.Get("k", clock.Now());

    REQUIRE(logger.Snapshot().empty());
}

TEST_CASE("TracingStorage forwards values unchanged (pass-through)", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const setCas = tracer.Set("k", MakeBytes("hello"), 42, FastCache::TimePoint::max());
    REQUIRE(setCas.has_value());

    auto const got = tracer.Get("k", clock.Now());
    REQUIRE(got.has_value());
    REQUIRE(got->found);
    REQUIRE(got->entry.flags == 42U);
    REQUIRE(got->entry.cas == *setCas);
}

TEST_CASE("TracingStorage emits DELETED / NOT_FOUND for Delete", "[tracing]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    REQUIRE(inner.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    REQUIRE(tracer.Delete("k", clock.Now()).has_value());
    auto const second = tracer.Delete("k", clock.Now());
    REQUIRE_FALSE(second.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(ContainsSubstr(records[0].message, "result=DELETED"));
    REQUIRE(ContainsSubstr(records[1].message, "result=NOT_FOUND"));
}

TEST_CASE("TracingStorage emits VALUE_TOO_LARGE with bytes for Set", "[tracing]")
{
    FastCache::InMemoryLruStorage inner { 0, 4 };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const r = tracer.Set("k", MakeBytes("hello"), 0, FastCache::TimePoint::max());
    REQUIRE_FALSE(r.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: SET"));
    REQUIRE(ContainsSubstr(records[0].message, "result=VALUE_TOO_LARGE bytes=5"));
}

TEST_CASE("TracingStorage emits VALUE_TOO_LARGE with bytes for Add", "[tracing]")
{
    FastCache::InMemoryLruStorage inner { 0, 4 };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const r = tracer.Add("k", MakeBytes("hello"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: ADD"));
    REQUIRE(ContainsSubstr(records[0].message, "result=VALUE_TOO_LARGE bytes=5"));
}

TEST_CASE("TracingStorage emits VALUE_TOO_LARGE with bytes for CAS", "[tracing]")
{
    FastCache::InMemoryLruStorage inner { 0, 4 };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    REQUIRE(inner.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());
    auto const casToken = inner.Get("k", clock.Now())->entry.cas;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const r = tracer.CompareAndSwap("k", casToken, MakeBytes("hello"), 0, FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(r.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: CAS"));
    REQUIRE(ContainsSubstr(records[0].message, "result=VALUE_TOO_LARGE bytes=5"));
}

TEST_CASE("TracingStorage emits VALUE_TOO_LARGE with bytes for Append", "[tracing]")
{
    FastCache::InMemoryLruStorage inner { 0, 4 };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    REQUIRE(inner.Set("k", MakeBytes("ab"), 0, FastCache::TimePoint::max()).has_value());
    auto const casToken = inner.Get("k", clock.Now())->entry.cas;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const suffix = MakeBytes("cde");
    auto const r = tracer.Append("k", suffix, casToken, clock.Now());
    REQUIRE_FALSE(r.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: APPEND"));
    REQUIRE(ContainsSubstr(records[0].message, "result=VALUE_TOO_LARGE bytes=3"));
}

TEST_CASE("TracingStorage emits VALUE_TOO_LARGE with bytes for Prepend", "[tracing]")
{
    FastCache::InMemoryLruStorage inner { 0, 4 };
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    REQUIRE(inner.Set("k", MakeBytes("ab"), 0, FastCache::TimePoint::max()).has_value());
    auto const casToken = inner.Get("k", clock.Now())->entry.cas;

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const prefix = MakeBytes("cde");
    auto const r = tracer.Prepend("k", prefix, casToken, clock.Now());
    REQUIRE_FALSE(r.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: PREPEND"));
    REQUIRE(ContainsSubstr(records[0].message, "result=VALUE_TOO_LARGE bytes=3"));
}

TEST_CASE("TracingStorage forwards Touch and emits TOUCHED / NOT_FOUND", "[tracing][touch]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;
    REQUIRE(inner.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const touched = tracer.Touch("k", FastCache::TimePoint::max(), clock.Now());
    REQUIRE(touched.has_value());
    auto const missed = tracer.Touch("nope", FastCache::TimePoint::max(), clock.Now());
    REQUIRE_FALSE(missed.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(ContainsSubstr(records[0].message, "storage: TOUCH"));
    REQUIRE(ContainsSubstr(records[0].message, "result=TOUCHED"));
    REQUIRE(ContainsSubstr(records[1].message, "result=NOT_FOUND"));
}

TEST_CASE("TracingStorage::PeekExpiry emits its own verb (not PEEK)", "[tracing][peek_expiry]")
{
    // Pre-Phase-1 the inherited default of PeekExpiry would re-enter
    // Peek on the decorator, emitting a misleading "storage: PEEK"
    // line for every TTL/PTTL poll. The override emits PEEK_EXPIRY,
    // distinguishing TTL probe traffic from value Peek traffic.
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;

    auto const deadline = clock.Now() + std::chrono::seconds { 30 };
    REQUIRE(inner.Set("k", MakeBytes("v"), 0, deadline).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const exp = tracer.PeekExpiry("k", clock.Now());
    REQUIRE(exp.has_value());
    REQUIRE(exp->has_value());
    if (exp.has_value() && exp->has_value())
        REQUIRE(**exp == deadline);

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: PEEK_EXPIRY"));
    REQUIRE(ContainsSubstr(records[0].message, "result=HIT"));
    // Specifically must NOT misattribute as plain Peek.
    REQUIRE_FALSE(ContainsSubstr(records[0].message, "storage: PEEK key"));
}

TEST_CASE("TracingStorage::PeekExpiry emits NO_TTL for keys without expiry", "[tracing][peek_expiry]")
{
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;
    REQUIRE(inner.Set("k", MakeBytes("v"), 0, FastCache::TimePoint::max()).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const exp = tracer.PeekExpiry("k", clock.Now());
    REQUIRE(exp.has_value());

    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "result=NO_TTL"));
}

TEST_CASE("TracingStorage::Update forwards to the inner atomic implementation and emits UPDATE", "[tracing][update]")
{
    // The inherited default would have decomposed Update into a Peek
    // followed by a Set on the decorator, losing the inner's atomic
    // RMW guarantee and emitting two trace lines under the wrong
    // verbs. With the override, the inner storage's Update is called
    // directly and exactly one UPDATE trace line is emitted.
    FastCache::InMemoryLruStorage inner;
    FastCache::CapturingLogger logger { FastCache::LogLevel::Trace };
    FastCache::ManualClock clock;
    auto const deadline = clock.Now() + std::chrono::seconds { 60 };
    REQUIRE(inner.Set("k", MakeBytes("0"), 0, deadline).has_value());

    FastCache::TracingStorage tracer { inner, logger, clock };
    auto const upd = tracer.Update(
        "k",
        [](FastCache::GetResult const&) -> std::expected<FastCache::IStorage::UpdateOutcome, FastCache::StorageError> {
            return FastCache::IStorage::UpdateOutcome {
                .value = MakeBytes("1"),
                .flags = 0,
                .action = FastCache::IStorage::UpdateAction::Store,
            };
        },
        clock.Now());
    REQUIRE(upd.has_value());

    // Exactly one trace line, under the UPDATE verb.
    auto records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(ContainsSubstr(records[0].message, "storage: UPDATE"));
    REQUIRE(ContainsSubstr(records[0].message, "result=STORED"));

    // And the inner storage preserved the TTL (Phase 1 contract holds
    // through the decorator).
    auto const after = inner.Peek("k", clock.Now());
    REQUIRE(after.has_value());
    REQUIRE(after->found);
    REQUIRE(after->entry.expiry == deadline);
}

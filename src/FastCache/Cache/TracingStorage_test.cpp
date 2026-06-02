// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

using FastCache::Testing::MakeBytes;

namespace
{

bool ContainsSubstr(std::string const& s, std::string_view needle) noexcept
{
    return s.find(needle) != std::string::npos;
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

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/WindowsEventLogger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using FastCache::EventLogSeverity;
using FastCache::EventLogSeverityFor;
using FastCache::LogLevel;

TEST_CASE("WindowsEventLogger: six levels fold onto the three severities a log has", "[platform][eventlog]")
{
    // The whole point of writing to the event log rather than to a file (#179) is
    // that the severity survives: a file flattens a level into four characters
    // inside a line, while the log records it as something an operator filters on.
    // So the fold is behaviour, and it is asserted here rather than left inside an
    // anonymous namespace where nothing could reach it.
    STATIC_REQUIRE(EventLogSeverityFor(LogLevel::Trace) == EventLogSeverity::Information);
    STATIC_REQUIRE(EventLogSeverityFor(LogLevel::Debug) == EventLogSeverity::Information);
    STATIC_REQUIRE(EventLogSeverityFor(LogLevel::Info) == EventLogSeverity::Information);

    // Warn is the boundary, and it is the assertion that would catch a fold quietly
    // sliding one level: a warning recorded as information is invisible to the
    // filter an operator actually uses.
    STATIC_REQUIRE(EventLogSeverityFor(LogLevel::Warn) == EventLogSeverity::Warning);

    // Fatal is an Error, because the log has no fourth severity to promote it to --
    // and reporting it as a Warning would put the last thing a dying service said
    // below the threshold most people watch.
    STATIC_REQUIRE(EventLogSeverityFor(LogLevel::Error) == EventLogSeverity::Error);
    STATIC_REQUIRE(EventLogSeverityFor(LogLevel::Fatal) == EventLogSeverity::Error);
}

TEST_CASE("WindowsEventLogger: absent where there is no event log, and says so", "[platform][eventlog]")
{
    // Null is the contract, not a failure: it is what lets one expression in `main`
    // choose a sink without a platform branch around it, and what keeps a foreground
    // run on any platform pointed at its terminal.
    auto const logger = FastCache::MakeWindowsEventLogger("FastCacheEventLogCase", LogLevel::Info);

#if defined(_WIN32)
    REQUIRE(logger != nullptr);

    // The level is carried, not defaulted -- a service started at `--log-level=warn`
    // whose sink logged everything would be a flag that silently stopped working.
    REQUIRE(logger->MinLevel() == LogLevel::Info);
    logger->SetMinLevel(LogLevel::Error);
    REQUIRE(logger->MinLevel() == LogLevel::Error);
#else
    REQUIRE(logger == nullptr);
#endif
}

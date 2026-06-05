// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <sstream>
#include <string>

TEST_CASE("ConsoleLogger writes formatted lines to its sink", "[logger]")
{
    std::ostringstream sink;
    FastCache::ConsoleLogger logger { sink, FastCache::LogLevel::Trace };

    logger.Log(FastCache::LogLevel::Info, "hello");
    logger.Log(FastCache::LogLevel::Warn, "watch out");

    auto const output = sink.str();
    REQUIRE(output == "[INFO] hello\n[WARN] watch out\n");
}

TEST_CASE("ConsoleLogger respects MinLevel and SetMinLevel", "[logger]")
{
    std::ostringstream sink;
    FastCache::ConsoleLogger logger { sink, FastCache::LogLevel::Warn };

    logger.Log(FastCache::LogLevel::Debug, "dropped");
    logger.Log(FastCache::LogLevel::Info, "also dropped");
    logger.Log(FastCache::LogLevel::Warn, "kept");

    REQUIRE(sink.str() == "[WARN] kept\n");

    sink.str("");
    logger.SetMinLevel(FastCache::LogLevel::Debug);
    logger.Log(FastCache::LogLevel::Debug, "now kept");
    REQUIRE(sink.str() == "[DEBUG] now kept\n");
}

TEST_CASE("NullLogger drops every record", "[logger]")
{
    FastCache::NullLogger logger;
    logger.Log(FastCache::LogLevel::Fatal, "ignored");
    // No assertion needed — surviving without UB is the contract.
    REQUIRE(logger.MinLevel() == FastCache::LogLevel::Fatal);
}

TEST_CASE("CapturingLogger snapshots emitted records in order", "[logger]")
{
    FastCache::CapturingLogger logger;
    logger.Log(FastCache::LogLevel::Info, "first");
    logger.Log(FastCache::LogLevel::Error, "second");

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].level == FastCache::LogLevel::Info);
    REQUIRE(records[0].message == "first");
    REQUIRE(records[1].level == FastCache::LogLevel::Error);
    REQUIRE(records[1].message == "second");
}

TEST_CASE("CapturingLogger Clear removes all records", "[logger]")
{
    FastCache::CapturingLogger logger;
    logger.Log(FastCache::LogLevel::Info, "one");
    logger.Log(FastCache::LogLevel::Info, "two");
    logger.Clear();
    REQUIRE(logger.Snapshot().empty());
}

TEST_CASE("ConsoleLogger prefixes lines with ISO 8601 UTC timestamp when enabled", "[logger]")
{
    std::ostringstream sink;
    FastCache::ConsoleLogger logger { sink, FastCache::LogLevel::Trace, /*timestamps=*/true };

    logger.Log(FastCache::LogLevel::Info, "hello");

    auto const output = sink.str();
    static std::regex const pattern { R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z \[INFO\] hello\n)" };
    REQUIRE(std::regex_match(output, pattern));
}

TEST_CASE("ILogger::Logf only formats when the level passes the filter", "[logger]")
{
    FastCache::CapturingLogger logger { FastCache::LogLevel::Warn };
    logger.Logf(FastCache::LogLevel::Debug, "dropped {}", 1);
    logger.Logf(FastCache::LogLevel::Error, "kept {}", 42);

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].message == "kept 42");
}

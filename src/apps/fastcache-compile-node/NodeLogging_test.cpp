// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeLogging.hpp"

#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <sstream>
#include <string>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// One line, emitted through the factory for @p cfg.
///
/// The whole case in one helper, deliberately: what is under test is the path from a
/// CONFIGURATION to the bytes on the sink, so every case drives the same two ends and
/// differs only in the configuration it sets.
/// @param cfg What an operator asked for.
/// @return What the logger wrote.
[[nodiscard]] std::string LineFor(NodeConfig const& cfg)
{
    std::ostringstream sink;
    auto const logger = MakeNodeConsoleLogger(sink, cfg);
    REQUIRE(logger != nullptr);
    logger->Log(LogLevel::Error, "a message");
    return sink.str();
}

/// Whether @p line begins with an ISO 8601 UTC instant.
///
/// Matched by SHAPE rather than compared to a formatted `now()`: a case that built its
/// own expected string would pass against a logger that stamped a constant, and one
/// that compared against a second reading of the clock is a race whenever the two land
/// either side of a microsecond.
/// @param line A rendered log line.
/// @return True when a timestamp prefix is present.
[[nodiscard]] bool StartsWithTimestamp(std::string const& line)
{
    static std::regex const iso8601 { R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z )" };
    return std::regex_search(line, iso8601);
}

} // namespace

TEST_CASE("The node's log-timestamps setting reaches the logger", "[node][logging]")
{
    // **Issue #485, and the case is about the WIRING rather than about `ConsoleLogger`.**
    // The node constructed its logger with two of the constructor's three arguments,
    // against a third that defaults to `false`. So the logger was correct, every test
    // of it passed, and the node could not emit a time or be asked for one. What was
    // missing was never a behaviour -- it was an argument, and only a case that drives
    // a CONFIGURATION in and reads the bytes out can see that.
    //
    // Driven through `MakeNodeConsoleLogger` because that is the one path production
    // takes. `ctest -R node-logger-single-path` is what keeps that true; without it
    // this case could go green over a function `main` had stopped calling.
    NodeConfig cfg;

    // The default, stated rather than assumed. Off, matching `fastcached`, chosen with
    // the supervisor cases in front of it -- see `NodeConfig::logTimestamps`.
    REQUIRE_FALSE(cfg.logTimestamps);
    auto const bare = LineFor(cfg);
    CHECK(bare == "[ERROR] a message\n");
    CHECK_FALSE(StartsWithTimestamp(bare));

    // And asking for it changes what is written. This is the assertion the defect
    // would have failed: before #485 there was no field to set.
    cfg.logTimestamps = true;
    auto const stamped = LineFor(cfg);
    CHECK(StartsWithTimestamp(stamped));

    // The rest of the line is unchanged, so the timestamp is a PREFIX and not a
    // different rendering -- an operator's `grep '\[ERROR\]'` keeps working.
    CHECK(stamped.ends_with("[ERROR] a message\n"));
}

TEST_CASE("The node's log level reaches the logger too", "[node][logging]")
{
    // The sibling setting, and it is here because the two are one concern reaching one
    // object: a factory that carried the timestamp and dropped the level would be the
    // same defect with the fields exchanged, and nothing else asserts this either.
    NodeConfig cfg;
    cfg.logLevel = LogLevel::Error;

    std::ostringstream sink;
    auto const logger = MakeNodeConsoleLogger(sink, cfg);
    REQUIRE(logger != nullptr);
    CHECK(logger->MinLevel() == LogLevel::Error);

    // Filtered, which is the level being spent rather than merely stored.
    logger->Log(LogLevel::Info, "below the threshold");
    CHECK(sink.str().empty());
    logger->Log(LogLevel::Error, "at the threshold");
    CHECK(sink.str().contains("at the threshold"));
}

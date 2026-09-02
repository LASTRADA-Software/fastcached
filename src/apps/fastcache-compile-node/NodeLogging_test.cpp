// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeLogging.hpp"

#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

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
    MakeNodeConsoleLogger(sink, cfg)->Log(LogLevel::Error, "a message");
    return sink.str();
}

} // namespace

TEST_CASE("The node's log-timestamps setting reaches the logger", "[node][logging]")
{
    // **Issue #485, and the case is about the WIRING rather than about `ConsoleLogger`.**
    // The node constructed its logger with two of the constructor's three arguments,
    // against a third that defaulted to `false`. So the logger was correct, every test
    // of it passed, and the node could not emit a time or be asked for one.
    //
    // The missing ARGUMENT is now a compile error -- `ConsoleLogger` takes
    // `LogTimestamps` with no default -- so what is left for a case to cover is the
    // half a signature cannot: that the factory passes what the CONFIGURATION says
    // rather than a hardcoded `LogTimestamps::No`, which would compile perfectly.
    //
    // What a timestamp looks like is `Logger_test`'s question and is asserted there.
    // Restating its format here would be a second, weaker copy that drifts; this
    // compares the two renderings against each other instead, which answers "did the
    // setting travel" without owning a format.
    NodeConfig cfg;

    // BOTH settings are driven explicitly. The default is platform-dependent since
    // #496, and a case that leans on it cannot see the setting travel on the platform
    // where the default already IS what the case set -- the two renderings would be
    // compared against a factory that was never asked to change anything. What the
    // default is belongs to `DefaultLogTimestamps` and is asserted beside it.
    cfg.logTimestamps = false;
    auto const bare = LineFor(cfg);
    CHECK(bare == "[ERROR] a message\n");

    cfg.logTimestamps = true;
    auto const stamped = LineFor(cfg);

    // A PREFIX and nothing else: the same line, with something in front of it. An
    // operator's `grep '\[ERROR\]'` keeps working, and a factory that ignored the
    // setting would produce two identical strings.
    CHECK(stamped.ends_with(bare));
    CHECK(stamped.size() > bare.size());
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
    CHECK(logger->MinLevel() == LogLevel::Error);

    // Filtered, which is the level being spent rather than merely stored.
    logger->Log(LogLevel::Info, "below the threshold");
    CHECK(sink.str().empty());
    logger->Log(LogLevel::Error, "at the threshold");
    CHECK(sink.str().contains("at the threshold"));
}

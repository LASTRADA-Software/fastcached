// SPDX-License-Identifier: Apache-2.0
#include "ExchangeLog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A reply frame whose first byte is `status`.
[[nodiscard]] std::vector<std::byte> ReplyWith(Wire::Status status, std::size_t extra = 0)
{
    std::vector<std::byte> bytes { static_cast<std::byte>(status) };
    bytes.resize(1 + extra);
    return bytes;
}

} // namespace

TEST_CASE("ExchangeLog: the high-rate verbs are Debug and the scheduling verbs are Info", "[node][logging]")
{
    // The WHOLE POINT of the table, so it is asserted directly rather than left to
    // follow from the rows: if `fetch` were Info a single build would bury the
    // journal, and if `lease` were Debug the default level would show nothing about
    // scheduling -- which is the state this table was written to end.
    CHECK(LogLevelForOp(static_cast<std::uint8_t>(Wire::Op::Fetch)) == LogLevel::Debug);
    CHECK(LogLevelForOp(static_cast<std::uint8_t>(Wire::Op::Store)) == LogLevel::Debug);
    // Trace, not Debug: the heartbeat is the only verb that arrives whether or
    // not anybody is building, so at Debug it is the one line that makes an IDLE
    // node's journal grow -- and a reader who turns Debug on wants the cache
    // traffic, not the pulse.
    CHECK(LogLevelForOp(static_cast<std::uint8_t>(Wire::Op::Heartbeat)) == LogLevel::Trace);

    CHECK(LogLevelForOp(static_cast<std::uint8_t>(Wire::Op::Lease)) == LogLevel::Info);
    CHECK(LogLevelForOp(static_cast<std::uint8_t>(Wire::Op::Compile)) == LogLevel::Info);
    CHECK(LogLevelForOp(static_cast<std::uint8_t>(Wire::Op::Register)) == LogLevel::Info);
}

TEST_CASE("ExchangeLog: an operator at the default level sees scheduling and no cache traffic", "[node][logging]")
{
    // The property an operator actually depends on, stated as a property rather than
    // as a list of rows -- a new high-rate verb added at Info would pass every
    // per-row check above and still destroy the journal.
    //
    // Asserted as a PARTITION: every Cache-family verb is Debug and every
    // Compile-family verb is Info. Scheduler is deliberately mixed (heartbeat is
    // periodic, a lease is an event), so it is not asserted wholesale -- a claim
    // that would be false is worse than no claim.
    for (auto const& row: Wire::OpTable)
    {
        auto const level = LogLevelForOp(static_cast<std::uint8_t>(row.code));
        if (row.family == Wire::VerbFamily::Cache)
            CHECK(level == LogLevel::Debug);
        if (row.family == Wire::VerbFamily::Compile)
            CHECK(level == LogLevel::Info);
    }
}

TEST_CASE("ExchangeLog: an unimplemented opcode is named by its byte, not as a word", "[node][logging]")
{
    // Two opcodes this build does not serve must not render identically: one client
    // probing and one client a version ahead are different diagnoses, and a shared
    // "unknown" makes them one.
    auto const a = ExchangeVerbName(0xEE);
    auto const b = ExchangeVerbName(0xEF);
    CHECK(a != b);
    CHECK(a == "opcode-0xee");

    // And it must not be able to fill a disk at Info.
    CHECK(LogLevelForOp(0xEE) == LogLevel::Debug);
}

TEST_CASE("ExchangeLog: a reply that was never written is its own outcome", "[node][logging]")
{
    // Four states -- ok, miss, error, none -- and the fourth is the one a bool or a
    // status byte alone cannot carry. A node that declined to write must not read as
    // one that answered `error`, because those are fixed in different places.
    auto const none = FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", 64, {}, 3ms);
    auto const err =
        FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", 64, ReplyWith(Wire::Status::Error), 3ms);
    CHECK(none.contains("no-reply"));
    CHECK(err.contains("error"));
    CHECK(none != err);
}

TEST_CASE("ExchangeLog: a miss and an ok are distinguishable in the line", "[node][logging]")
{
    // `fetch -> miss` versus `fetch -> ok` is the single most useful thing this line
    // carries: it is the difference between a cache that is working and one that is
    // reachable and empty, which is exactly what an operator cannot see from the
    // journal today.
    auto const hit =
        FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", 64, ReplyWith(Wire::Status::Ok, 4096), 12ms);
    auto const miss =
        FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", 64, ReplyWith(Wire::Status::Miss), 1ms);
    CHECK(hit.contains("-> ok"));
    CHECK(miss.contains("-> miss"));

    // The sizes are in the line, because "the cache served 4 KiB" and "the cache
    // served nothing" is the same `ok` otherwise.
    CHECK(hit.contains("4097 B out"));
}

TEST_CASE("ExchangeLog: a peer the kernel could not name is said so, not left blank", "[node][logging]")
{
    auto const line = FormatExchange(static_cast<std::uint8_t>(Wire::Op::Lease), "", 32, ReplyWith(Wire::Status::Ok), 0ms);
    CHECK(line.contains("<unknown peer>"));
}

TEST_CASE("ExchangeLog: the line names the verb, the peer and the elapsed time", "[node][logging]")
{
    auto const line = FormatExchange(
        static_cast<std::uint8_t>(Wire::Op::Compile), "192.168.1.7", 1024, ReplyWith(Wire::Status::Ok, 8192), 4210ms);
    CHECK(line.contains("compile"));
    CHECK(line.contains("192.168.1.7"));
    CHECK(line.contains("4210 ms"));
    CHECK(line.contains("1024 B in"));
}

TEST_CASE("ExchangeLog: an unrecognised status byte renders as its byte", "[node][logging]")
{
    std::vector<std::byte> const weird { static_cast<std::byte>(0x7F) };
    auto const line = FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "h", 8, weird, 0ms);
    CHECK(line.contains("status-0x7f"));
}

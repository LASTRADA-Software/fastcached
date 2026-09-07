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

namespace
{

/// A logger that keeps what it was told, so a case can assert on the LEVEL as well
/// as the text -- which is the whole point here: the exchange line already says a
/// refusal happened, at the verb's own level, and what #181 is about is that nobody
/// sees it there.
class CapturingLines final: public FastCache::ILogger
{
  public:
    void Log(FastCache::LogLevel level, std::string_view message) override
    {
        lines.emplace_back(level, std::string { message });
    }
    [[nodiscard]] FastCache::LogLevel MinLevel() const noexcept override
    {
        return FastCache::LogLevel::Trace;
    }
    void SetMinLevel(FastCache::LogLevel /*level*/) noexcept override {}

    [[nodiscard]] std::size_t WarnCount() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [](auto const& l) { return l.first == FastCache::LogLevel::Warn; }));
    }

    std::vector<std::pair<FastCache::LogLevel, std::string>> lines;
};

/// An `Error` reply carrying `code`, framed the way the wire frames one.
[[nodiscard]] std::vector<std::byte> ErrorReply(Wire::ErrorCode code, std::string_view message)
{
    return Wire::EncodeErrorReply(code, message);
}

} // namespace

TEST_CASE("NoteVersionRefusal: a version refusal is Warn, not the verb's level", "[node][logging][refusal]")
{
    // The defect: the exchange line follows the VERB, so a refused `fetch` is `Debug`
    // and an operator at the default level sees a node refusing every request with no
    // line saying so. #815 is what that costs -- weeks of building through a cache
    // answering "no".
    CapturingLines log;
    RefusalThrottle throttle;
    auto const t0 = std::chrono::steady_clock::time_point {};

    NoteVersionRefusal(log, throttle, "10.0.0.4", ErrorReply(Wire::ErrorCode::UnsupportedVersion, "supported 5..5"), t0);

    REQUIRE(log.WarnCount() == 1);
    CHECK(log.lines.front().second.contains("10.0.0.4"));
    // The daemon's own words carry the range, which the category alone cannot.
    CHECK(log.lines.front().second.contains("supported 5..5"));
    // And it says what it MEANS for that client, not only what happened.
    CHECK(log.lines.front().second.contains("version pair"));
}

TEST_CASE("NoteVersionRefusal: any other refusal is left to the exchange line", "[node][logging][refusal]")
{
    // Narrow on purpose. A malformed frame or an unauthenticated peer is about ONE
    // request; only a version mismatch says the client cannot work with this node at
    // all. Escalating the others would make the level meaningless.
    CapturingLines log;
    RefusalThrottle throttle;
    auto const t0 = std::chrono::steady_clock::time_point {};

    NoteVersionRefusal(log, throttle, "10.0.0.4", ErrorReply(Wire::ErrorCode::MalformedFrame, "bad"), t0);
    NoteVersionRefusal(log, throttle, "10.0.0.4", ErrorReply(Wire::ErrorCode::Unauthenticated, "no token"), t0);
    // And a perfectly good reply must not be mistaken for one.
    NoteVersionRefusal(log, throttle, "10.0.0.4", Wire::EncodeReply(Wire::Status::Ok, {}), t0);

    CHECK(log.WarnCount() == 0);
}

TEST_CASE("NoteVersionRefusal: a build's every translation unit produces one line", "[node][logging][refusal]")
{
    // A build opens a connection per translation unit, so an unthrottled line would be
    // one per compile -- which buries the journal and is the failure mode #993 records
    // one layer up.
    CapturingLines log;
    RefusalThrottle throttle;
    auto const t0 = std::chrono::steady_clock::time_point {};

    for (auto i = 0; i < 1000; ++i)
        NoteVersionRefusal(log, throttle, "10.0.0.4", ErrorReply(Wire::ErrorCode::UnsupportedVersion, "supported 5..5"), t0);
    CHECK(log.WarnCount() == 1);

    // A DIFFERENT machine is a different fact and must not be suppressed by the first:
    // during a rollout the operator needs to know which machines are behind.
    NoteVersionRefusal(log, throttle, "10.0.0.9", ErrorReply(Wire::ErrorCode::UnsupportedVersion, "supported 5..5"), t0);
    CHECK(log.WarnCount() == 2);

    // ...and it speaks again once the interval passes, or somebody who fixes one node
    // never learns the rest are still behind.
    NoteVersionRefusal(log,
                       throttle,
                       "10.0.0.4",
                       ErrorReply(Wire::ErrorCode::UnsupportedVersion, "supported 5..5"),
                       t0 + std::chrono::seconds { 61 });
    CHECK(log.WarnCount() == 3);
}

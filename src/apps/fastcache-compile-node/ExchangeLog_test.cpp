// SPDX-License-Identifier: Apache-2.0
#include "ExchangeLog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A reply frame whose first byte is `status`.
/// A request frame of @p bytes, for the size the line reports.
///
/// The renderer takes the byte count FROM the span now rather than beside it, so a
/// case cannot state a length that disagrees with the bytes it passed. These frames
/// carry no valid header on purpose: every case here is about the generic line, and
/// the toolchain clause -- which needs a decodable compile payload -- has its own
/// cases below.
[[nodiscard]] std::vector<std::byte> FrameOf(std::size_t bytes)
{
    return std::vector<std::byte>(bytes, std::byte { 0 });
}

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
    auto const none = FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", FrameOf(64), {}, 3ms);
    auto const err = FormatExchange(
        static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", FrameOf(64), ReplyWith(Wire::Status::Error), 3ms);
    CHECK(none.contains("no-reply"));
    CHECK(err.contains("error"));
    CHECK(none != err);
}

TEST_CASE("ExchangeLog: a stream that pushed counts its pushes and is not a missing reply", "[node][logging]")
{
    // WHAT DISTINGUISHES: the same empty terminal is `no-reply (0 B out)` for a one-reply verb and `28 pushes,
    // then closed` with the pushes' bytes for a subscription its watcher left -- the line lane-enroll read on a
    // node that had streamed for half a minute. And a stream that ends with a terminal names that terminal.
    auto const subscribe = static_cast<std::uint8_t>(Wire::Op::Subscribe);
    auto const left =
        FormatExchange(subscribe, "127.0.0.1", FrameOf(24), {}, 27502ms, {}, StreamTally { .pushes = 28, .bytes = 49000 });
    CHECK(left.contains("-> 28 pushes, then closed (24 B in, 49000 B out, 27502 ms)"));
    CHECK_FALSE(left.contains("no-reply"));

    auto const ended = FormatExchange(subscribe,
                                      "127.0.0.1",
                                      FrameOf(24),
                                      ReplyWith(Wire::Status::Ok),
                                      900ms,
                                      {},
                                      StreamTally { .pushes = 2, .bytes = 100 });
    CHECK(ended.contains("-> 2 pushes, then ok (24 B in, 101 B out, 900 ms)"));

    // The control: without a tally the same empty reply is still its own outcome.
    CHECK(FormatExchange(subscribe, "127.0.0.1", FrameOf(24), {}, 1ms).contains("-> no-reply (24 B in, 0 B out"));
}

TEST_CASE("ExchangeLog: a miss and an ok are distinguishable in the line", "[node][logging]")
{
    // `fetch -> miss` versus `fetch -> ok` is the single most useful thing this line
    // carries: it is the difference between a cache that is working and one that is
    // reachable and empty, which is exactly what an operator cannot see from the
    // journal today.
    auto const hit = FormatExchange(
        static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", FrameOf(64), ReplyWith(Wire::Status::Ok, 4096), 12ms);
    auto const miss = FormatExchange(
        static_cast<std::uint8_t>(Wire::Op::Fetch), "10.0.0.4", FrameOf(64), ReplyWith(Wire::Status::Miss), 1ms);
    CHECK(hit.contains("-> ok"));
    CHECK(miss.contains("-> miss"));

    // The sizes are in the line, because "the cache served 4 KiB" and "the cache
    // served nothing" is the same `ok` otherwise.
    CHECK(hit.contains("4097 B out"));
}

TEST_CASE("ExchangeLog: a peer the kernel could not name is said so, not left blank", "[node][logging]")
{
    auto const line =
        FormatExchange(static_cast<std::uint8_t>(Wire::Op::Lease), "", FrameOf(32), ReplyWith(Wire::Status::Ok), 0ms);
    CHECK(line.contains("<unknown peer>"));
}

TEST_CASE("ExchangeLog: the line names the verb, the peer and the elapsed time", "[node][logging]")
{
    auto const line = FormatExchange(static_cast<std::uint8_t>(Wire::Op::Compile),
                                     "192.168.1.7",
                                     FrameOf(1024),
                                     ReplyWith(Wire::Status::Ok, 8192),
                                     4210ms);
    CHECK(line.contains("compile"));
    CHECK(line.contains("192.168.1.7"));
    CHECK(line.contains("4210 ms"));
    CHECK(line.contains("1024 B in"));
}

TEST_CASE("ExchangeLog: an unrecognised status byte renders as its byte", "[node][logging]")
{
    std::vector<std::byte> const weird { static_cast<std::byte>(0x7F) };
    auto const line = FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch), "h", FrameOf(8), weird, 0ms);
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
        return minLevel;
    }
    void SetMinLevel(FastCache::LogLevel level) noexcept override
    {
        minLevel = level;
    }

    [[nodiscard]] std::size_t WarnCount() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(lines, [](auto const& l) { return l.first == FastCache::LogLevel::Warn; }));
    }

    std::vector<std::pair<FastCache::LogLevel, std::string>> lines;
    FastCache::LogLevel minLevel { FastCache::LogLevel::Trace }; ///< What `LogExchange` is told this logger hears.
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

    for ([[maybe_unused]] auto const i: std::views::iota(0, 1000))
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

// --- the toolchain clause (#994) --------------------------------------------

namespace
{
/// A real, decodable compile frame naming @p fingerprint.
///
/// Encoded through `Wire::EncodeCompile` rather than assembled by hand, for the
/// reason the renderer decodes rather than being told: the clause is only correct if
/// it reads what a real client sends, and a hand-built frame would be this test
/// agreeing with itself about an offset.
[[nodiscard]] std::vector<std::byte> CompileFrameFor(std::string_view fingerprint)
{
    std::array<std::byte, 4> const source { std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 4 } };
    // EVERY field named, the empty ones included. clang treats
    // `missing-designated-field-initializers` as an error under `-Werror` while GCC
    // accepts the partial form, so the short spelling builds on a GCC lane and fails
    // the clang-debug gate leg. It did, which is what "one configuration is not the
    // gate" means when it happens to you.
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "a-token",
                                                      .fingerprint = fingerprint,
                                                      .args = {},
                                                      .source = std::span<std::byte const> { source },
                                                      .acceptedCodecs = {},
                                                      .sourceName = {},
                                                      .compileDir = {},
                                                      .compileDirReplacement = {},
                                                      .sourceRoot = {},
                                                      .sourceRootReplacement = {} });
}
} // namespace

TEST_CASE("ExchangeLog: a compile names the toolchain it used", "[node][logging][toolchain]")
{
    // #994. The line already said WHO and HOW LONG; this is the half an operator needs
    // to answer "is the fleet using the compiler I think it is".
    auto const frame = CompileFrameFor("631c2ecd");
    auto const line = Node::FormatExchange(static_cast<std::uint8_t>(Wire::Op::Compile),
                                           "192.168.1.7",
                                           frame,
                                           ReplyWith(Wire::Status::Ok, 8192),
                                           4210ms,
                                           [](std::string_view fp) {
                                               CHECK(fp == "631c2ecd");
                                               return std::string { "/usr/bin/gcc" };
                                           });

    // BOTH halves. A fingerprint an operator cannot resolve is close to useless on its
    // own -- it correlates against the node's startup lines and nothing else -- and a
    // compiler path without the fingerprint cannot be matched against what a client
    // keyed on. The ticket calls one without the other half a fix.
    CHECK(line.contains("toolchain=631c2ecd"));
    CHECK(line.contains("/usr/bin/gcc"));

    // And it is still the same line, not a second one. `ServeConnection` calls this
    // "the one place this node says anything about a client".
    CHECK(line.contains("192.168.1.7"));
    CHECK(line.contains("4210 ms"));
}

TEST_CASE("ExchangeLog: a fingerprint this node does not serve is NAMED, not dropped", "[node][logging][toolchain]")
{
    // **The interesting case, and the one a silent fallback would hide.** A client
    // keying against a toolchain this node has stopped serving is #238's symptom seen
    // from the other side -- and if the clause simply vanished, the line would be
    // indistinguishable from a verb that carries no toolchain at all.
    auto const frame = CompileFrameFor("deadbeef");
    auto const line = Node::FormatExchange(static_cast<std::uint8_t>(Wire::Op::Compile),
                                           "10.0.0.9",
                                           frame,
                                           ReplyWith(Wire::Status::Error),
                                           7ms,
                                           [](std::string_view) { return std::string {}; });

    CHECK(line.contains("toolchain=deadbeef"));
    CHECK(line.contains("unserved"));
}

TEST_CASE("ExchangeLog: a verb that is not a compile carries no toolchain clause", "[node][logging][toolchain]")
{
    // The discriminator, and getting the FIXTURE right is the whole of it.
    //
    // **The obvious version of this case cannot fail.** Written with `FrameOf(64)` --
    // sixty-four junk bytes -- it passed under a renderer with the verb check DELETED,
    // measured: junk has no decodable header, so the clause is empty for that reason
    // and the verb test is never reached. The assertion was right and the case tested
    // nothing, which is `.agent/rules/testing.md`'s "assert what DISTINGUISHES" landing
    // on the test written to distinguish.
    //
    // So the payload is held CONSTANT and only the verb varies: these are the same
    // bytes as the compile cases above, labelled `fetch`. Everything except the verb
    // check would now produce a clause, so the absence below can only be the verb
    // check doing its job. Re-checked by deleting that check: this case then fails.
    auto const frame = CompileFrameFor("631c2ecd");
    auto const line = Node::FormatExchange(static_cast<std::uint8_t>(Wire::Op::Fetch),
                                           "10.0.0.4",
                                           frame,
                                           ReplyWith(Wire::Status::Ok, 4096),
                                           12ms,
                                           [](std::string_view) { return std::string { "/usr/bin/gcc" }; });

    CHECK_FALSE(line.contains("toolchain="));
    CHECK_FALSE(line.contains("/usr/bin/gcc"));
    // Still a complete line, so the absence above is a missing clause and not a
    // renderer that gave up.
    CHECK(line.contains("fetch"));
    CHECK(line.contains("10.0.0.4"));
}

TEST_CASE("ExchangeLog: an undecodable compile frame drops the clause rather than guessing", "[node][logging][toolchain]")
{
    // A truncated or malformed compile payload has no fingerprint to name. It must not
    // print an empty one, and it must not stop the line being written -- the exchange
    // still happened and the peer, the size and the elapsed time are all still true.
    auto const line = Node::FormatExchange(static_cast<std::uint8_t>(Wire::Op::Compile),
                                           "10.0.0.4",
                                           FrameOf(64),
                                           ReplyWith(Wire::Status::Error),
                                           3ms,
                                           [](std::string_view) { return std::string { "/usr/bin/gcc" }; });

    CHECK_FALSE(line.contains("toolchain="));
    CHECK(line.contains("compile"));
    CHECK(line.contains("10.0.0.4"));
}

TEST_CASE("ExchangeLog: with no namer a compile still names its fingerprint", "[node][logging][toolchain]")
{
    // Every surface without a toolchain map -- a cache tier, a scheduler, every
    // fixture -- passes no namer at all. The clause must degrade to the half it can
    // still answer rather than disappearing: the fingerprint is decoded from the
    // frame and needs nobody's help.
    auto const frame = CompileFrameFor("631c2ecd");
    auto const line = Node::FormatExchange(
        static_cast<std::uint8_t>(Wire::Op::Compile), "10.0.0.4", frame, ReplyWith(Wire::Status::Ok), 5ms);

    CHECK(line.contains("toolchain=631c2ecd"));
    CHECK(line.contains("unserved"));
}

// --- cache-drop (#1276) --------------------------------------------------------

TEST_CASE("ExchangeLog: a removal is narrated at Info and the drop exchange stays Debug", "[node][logging][cache-drop]")
{
    // WHAT DISTINGUISHES: the same verb, three outcomes, and only the one that removed an
    // object says anything at the default level. The partition case above is left untouched
    // and still holds for this verb -- its exchange line is Debug -- which is what keeps that
    // guard exact rather than carrying a local exception.
    auto const drop = static_cast<std::uint8_t>(Wire::Op::CacheDrop);
    auto const frame = Wire::EncodeCacheDrop("0123abcd");
    CHECK(LogLevelForOp(drop) == LogLevel::Debug);

    auto const infoLines = [&](std::vector<std::byte> const& reply) {
        CapturingLines log;
        LogExchange(log, drop, "127.0.0.1", frame, reply, 1ms);
        std::vector<std::string> info;
        for (auto const& [level, text]: log.lines)
            if (level == LogLevel::Info)
                info.push_back(text);
        auto const debug = std::ranges::count_if(log.lines, [](auto const& l) { return l.first == LogLevel::Debug; });
        CHECK(debug == 1); // the exchange line, on every outcome
        return info;
    };

    SECTION("a hit removed an object, and says which, from whom")
    {
        auto const info = infoLines(ReplyWith(Wire::Status::Ok));
        REQUIRE(info.size() == 1);
        CHECK(info.front().contains("127.0.0.1"));
        CHECK(info.front().contains("removed key 0123abcd"));
    }
    SECTION("a miss removed nothing and says nothing above Debug")
    {
        CHECK(infoLines(ReplyWith(Wire::Status::Miss)).empty());
    }
    SECTION("a refusal removed nothing and says nothing above Debug")
    {
        CHECK(infoLines(ErrorReply(Wire::ErrorCode::NotAMember, "not this machine")).empty());
    }
    SECTION("a node that wrote no reply removed nothing it can vouch for")
    {
        CHECK(infoLines({}).empty());
    }
}

TEST_CASE("ExchangeLog: a removal is heard at the default level, which the exchange line is not",
          "[node][logging][cache-drop]")
{
    // The ordering bug this guards: the removal nested inside the exchange line's own level
    // test would be silenced at exactly Info, the level it exists to be seen at.
    CapturingLines log;
    log.SetMinLevel(LogLevel::Info);
    LogExchange(log,
                static_cast<std::uint8_t>(Wire::Op::CacheDrop),
                "127.0.0.1",
                Wire::EncodeCacheDrop("k"),
                ReplyWith(Wire::Status::Ok),
                1ms);
    REQUIRE(log.lines.size() == 1);
    CHECK(log.lines.front().first == LogLevel::Info);
}

TEST_CASE("ExchangeLog: a removed key a peer chose cannot forge a journal line", "[node][logging][cache-drop]")
{
    // A key is bytes a client picked. Unescaped, a newline in it would begin a second record
    // that says whatever the client liked, and an ESC is a sequence an operator's terminal obeys.
    auto const removal = FormatCacheRemoval(static_cast<std::uint8_t>(Wire::Op::CacheDrop),
                                            "127.0.0.1",
                                            Wire::EncodeCacheDrop("evil\nfake line\x1b[2J"),
                                            ReplyWith(Wire::Status::Ok));
    REQUIRE(removal.has_value());
    CHECK_FALSE(Unwrap(removal).contains('\n'));
    CHECK_FALSE(Unwrap(removal).contains('\x1b'));
    CHECK(Unwrap(removal).contains(R"(\x1b)"));
}

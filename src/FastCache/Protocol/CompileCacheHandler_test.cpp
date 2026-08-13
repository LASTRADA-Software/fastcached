// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastCache;
using PathCanon::Grammar;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Fixture: a paired in-memory socket, an engine over in-memory storage, and
/// the compile-cache handler driving the server end.
struct CcFixture
{
    ManualClock clock;
    InMemoryLruStorage storage { 0 };
    CacheEngine engine { storage, clock };
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
};

/// @param payload Bytes to write. Taken by value: a coroutine must not hold a
///                reference parameter across a suspend point.
Task<bool> WriteBytes(ISocket* socket, std::vector<std::byte> payload)
{
    auto const r = co_await socket->Write(std::span<std::byte const> { payload.data(), payload.size() });
    co_return r.has_value();
}

Task<std::vector<std::byte>> ReadAvailable(ISocket* socket)
{
    std::vector<std::byte> out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const r = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*r));
        if (*r < chunk.size())
            break;
    }
    co_return out;
}

/// Read exactly `count` reply bytes.
///
/// ReadAvailable stops as soon as a Read returns less than its chunk size,
/// which cannot express "the reply is an exact multiple of the chunk" — there
/// it would loop and park on a Read that nothing will ever satisfy. When the
/// expected length is known up front (a FETCH hit is 1 + 4 + payload), asking
/// for exactly that many bytes sidesteps the guesswork entirely.
/// @param socket Socket to read from.
/// @param count Exact byte count expected.
/// @return The bytes read; shorter than `count` if the peer stopped early.
Task<std::vector<std::byte>> ReadExactlyN(ISocket* socket, std::size_t count)
{
    std::vector<std::byte> out;
    out.reserve(count);
    while (out.size() < count)
    {
        std::vector<std::byte> chunk(count - out.size());
        auto const r = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!r.has_value() || *r == 0)
            break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*r));
    }
    co_return out;
}

/// Drive request bytes through the handler and return the raw reply bytes.
/// Half-closes the client write side so the handler sees EOF.
/// @param fix The fixture.
/// @param request The request bytes; may contain several concatenated frames.
/// @param session Session knobs (payload cap, logger).
/// @return Everything the handler wrote back.
std::vector<std::byte> ExchangeWith(CcFixture& fix, std::vector<std::byte> const& request, SessionContext session)
{
    REQUIRE(SyncRun(WriteBytes(fix.pair.client.get(), request)));
    fix.pair.client->ShutdownWrite();
    SyncRun(fix.handler.Run(fix.pair.server.get(), &fix.engine, /*primingBytes*/ {}, session));
    // Close the server side before draining, as Connection does once a handler
    // returns. Without it a rejection that closes WITHOUT replying — a foreign
    // magic, the one case that still cannot be answered — leaves the drain
    // parked on a read nothing will ever satisfy.
    fix.pair.server->ShutdownWrite();
    return SyncRun(ReadAvailable(fix.pair.client.get()));
}

/// Drive one batch of request bytes through the handler with default session
/// settings.
std::vector<std::byte> Exchange(CcFixture& fix, std::vector<std::byte> const& request)
{
    return ExchangeWith(fix, request, SessionContext {});
}

/// Concatenate several request frames into one stream, so a single handler run
/// can be asked to answer more than one command — the only way to observe
/// "reply and continue" given that one Run consumes EOF and returns.
std::vector<std::byte> Concat(std::initializer_list<std::vector<std::byte>> frames)
{
    std::vector<std::byte> out;
    for (auto const& frame: frames)
        out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

/// One decoded reply.
struct ReplyFrame
{
    Wire::Status status;
    std::vector<std::byte> payload;
};

/// Split a raw reply stream into frames using each frame's *declared* length.
///
/// This is the client half of the drainability contract, and asserting through
/// it is what makes the contract real: a reader that knows nothing but the
/// header can step over any reply, whatever command produced it and whether it
/// succeeded, missed or was refused. The pre-version format could not do this —
/// a miss was a bare byte and an error carried an undeclared trailing message.
/// @param bytes The raw stream.
/// @return One entry per complete frame found.
std::vector<ReplyFrame> SplitReplies(std::span<std::byte const> bytes)
{
    std::vector<ReplyFrame> frames;
    std::size_t offset = 0;
    while (offset + Wire::ReplyHeaderSize <= bytes.size())
    {
        auto const header = Wire::DecodeReplyHeader(bytes.subspan(offset, Wire::ReplyHeaderSize));
        if (!header.has_value())
            break;
        offset += Wire::ReplyHeaderSize;
        if (bytes.size() - offset < header->payloadLength)
            break;
        auto const payload = bytes.subspan(offset, header->payloadLength);
        frames.push_back(ReplyFrame { .status = header->status, .payload = { payload.begin(), payload.end() } });
        offset += header->payloadLength;
    }
    return frames;
}

/// Decode the single reply expected from an exchange.
std::optional<ReplyFrame> SoleReply(std::span<std::byte const> bytes)
{
    auto const frames = SplitReplies(bytes);
    if (frames.size() != 1)
        return std::nullopt;
    return frames.front();
}

/// The four string fields of a STORE frame. Named rather than positional so the
/// same-typed fields cannot be transposed at a call site.
struct StoreFields
{
    std::string_view key;
    std::string_view cohort;
    std::string_view srcRoot;
    std::string_view buildTree;
};

/// Build a STORE frame through the shared wire module, so these tests exercise
/// the same encoder the launcher ships rather than a second implementation that
/// could agree with the handler while both disagree with the client.
std::vector<std::byte> StoreFrame(StoreFields const& fields,
                                  CompileValue const& value,
                                  Wire::WireVersion version = Wire::CurrentVersion)
{
    auto const encoded = EncodeCompileValue(value);
    return Wire::EncodeStore(Wire::StoreRequest { .key = fields.key,
                                                  .cohort = fields.cohort,
                                                  .srcRoot = fields.srcRoot,
                                                  .buildTree = fields.buildTree,
                                                  .value = std::span<std::byte const> { encoded } },
                             version);
}

/// Build a FETCH frame.
std::vector<std::byte> FetchFrame(std::string_view key, Wire::WireVersion version = Wire::CurrentVersion)
{
    return Wire::EncodeFetch(key, version);
}

/// Decode a FETCH hit reply into a CompileValue.
std::optional<CompileValue> DecodeFetchHit(std::span<std::byte const> reply)
{
    auto const frame = SoleReply(reply);
    if (!frame.has_value() || frame->status != Wire::Status::Ok)
        return std::nullopt;
    auto decoded = DecodeCompileValue(frame->payload);
    if (!decoded.has_value())
        return std::nullopt;
    return *decoded;
}

/// The error code and message carried by an Error reply.
std::optional<std::pair<Wire::ErrorCode, std::string>> ErrorOf(ReplyFrame const& frame)
{
    if (frame.status != Wire::Status::Error)
        return std::nullopt;
    auto const decoded = Wire::DecodeErrorPayload(frame.payload);
    if (!decoded.has_value())
        return std::nullopt;
    return std::pair { decoded->first, std::string { decoded->second } };
}

} // namespace

TEST_CASE("STORE canonicalizes showIncludes; FETCH returns the canonical form", "[compile-cache][handler]")
{
    CcFixture fix;

    CompileValue v;
    v.objectBlob = { std::byte { 0xDE }, std::byte { 0xAD } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\ci\deep\src\a.h)"
                                       "\r\n" });

    // STORE from a deep "CI" layout.
    auto const storeReply = Exchange(
        fix,
        StoreFrame(
            { .key = "obj-hash", .cohort = "envCI", .srcRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" },
            v));
    auto const storeFrame = SoleReply(storeReply);
    REQUIRE(storeFrame.has_value());
    CHECK(storeFrame->status == Wire::Status::Ok);

    // FETCH: the value must be canonical (no machine path), object blob intact.
    // The first handler consumed EOF and returned, so drive a fresh handler
    // over the SAME engine (same storage) for the fetch.
    InMemorySocketPair pair2 = InMemorySocketPair::Create();
    CompileCacheHandler handler2;
    REQUIRE(SyncRun(WriteBytes(pair2.client.get(), FetchFrame("obj-hash"))));
    pair2.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler2.Run(pair2.server.get(), &fix.engine, {}, session));
    auto const fetchReply = SyncRun(ReadAvailable(pair2.client.get()));

    // value_or keeps the access unguarded-safe: static analysis cannot see a
    // has_value() guard through Catch2's REQUIRE macro.
    auto const decoded = DecodeFetchHit(fetchReply);
    REQUIRE(decoded.has_value());
    auto const fetched = decoded.value_or(CompileValue {});
    REQUIRE(fetched.textRegions.size() == 1);
    CHECK(fetched.textRegions[0].bytes.contains("<SRCROOT>/a.h"));
    CHECK_FALSE(fetched.textRegions[0].bytes.contains(R"(ci\deep)"));
    CHECK(fetched.objectBlob == std::vector<std::byte> { std::byte { 0xDE }, std::byte { 0xAD } });
}

// Every other handler test uses a two-byte object blob, so nothing here ever
// exercised a reply big enough to matter. A FETCH reply declares its length up
// front, which means a transport that delivers fewer bytes than declared leaves
// the client blocked forever rather than failing — the failure mode that took
// down a real build. This pins the protocol end of that contract: what STORE
// accepted is exactly what FETCH hands back, at a size well past any buffer.
TEST_CASE("STORE/FETCH round-trips an object blob larger than 1 MiB", "[compile-cache][handler][large]")
{
    CcFixture fix;

    // Deliberately not a round number: a reply whose length is an exact
    // multiple of a reader's chunk size is its own (separate) trap, and the
    // odd size also makes an off-by-one in the framing visible.
    constexpr std::size_t BlobByteCount = (1024U * 1024U) + 4099U;
    CompileValue v;
    v.objectBlob.resize(BlobByteCount);
    for (std::size_t i = 0; i < BlobByteCount; ++i)
        v.objectBlob[i] = static_cast<std::byte>((i * 31U) & 0xFF);
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\ci\deep\src\big.h)"
                                       "\r\n" });

    auto const storeReply = Exchange(
        fix,
        StoreFrame(
            { .key = "big-obj", .cohort = "envCI", .srcRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" }, v));
    auto const storeFrame = SoleReply(storeReply);
    REQUIRE(storeFrame.has_value());
    REQUIRE(storeFrame->status == Wire::Status::Ok);

    InMemorySocketPair pair2 = InMemorySocketPair::Create();
    CompileCacheHandler handler2;
    REQUIRE(SyncRun(WriteBytes(pair2.client.get(), FetchFrame("big-obj"))));
    pair2.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler2.Run(pair2.server.get(), &fix.engine, {}, session));

    // Read the 5-byte header first so the declared length drives the rest: that
    // is exactly what a real client does, so a truncated reply shows up here as
    // a short read instead of as a hang.
    auto const headerBytes = SyncRun(ReadExactlyN(pair2.client.get(), Wire::ReplyHeaderSize));
    REQUIRE(headerBytes.size() == Wire::ReplyHeaderSize);
    auto const header = Wire::DecodeReplyHeader(headerBytes);
    REQUIRE(header.has_value());
    REQUIRE(header->status == Wire::Status::Ok);
    auto const declared = header->payloadLength;
    REQUIRE(declared > 1024U * 1024U);

    auto const payload = SyncRun(ReadExactlyN(pair2.client.get(), declared));
    REQUIRE(payload.size() == declared);

    auto const decoded = DecodeCompileValue(std::span<std::byte const> { payload });
    REQUIRE(decoded.has_value());
    auto const fetched = decoded.value_or(CompileValue {});
    REQUIRE(fetched.objectBlob.size() == BlobByteCount);
    CHECK(fetched.objectBlob == v.objectBlob);
    REQUIRE(fetched.textRegions.size() == 1);
    CHECK(fetched.textRegions[0].bytes.contains("<SRCROOT>/big.h"));
}

TEST_CASE("FETCH of an absent key replies miss", "[compile-cache][handler]")
{
    CcFixture fix;
    auto const reply = Exchange(fix, FetchFrame("nope"));
    auto const frame = SoleReply(reply);
    REQUIRE(frame.has_value());
    CHECK(frame->status == Wire::Status::Miss);
    CHECK(frame->payload.empty());
}

// --- versioning and rejection ----------------------------------------------
//
// The point of the versioned header is that a peer this build cannot serve gets
// a diagnosable ANSWER. A silent close, which is what the pre-version format
// could only manage, is indistinguishable from a dead connection — so a
// mismatched install looked exactly like a flaky network and a cache that never
// warmed up.

TEST_CASE("A request at an unsupported wire version is rejected, not dropped", "[compile-cache][handler][version]")
{
    CcFixture fix;
    constexpr auto TooNew = static_cast<Wire::WireVersion>(Wire::CurrentVersion + 1);

    auto const reply = Exchange(fix, FetchFrame("k", TooNew));
    auto const frame = SoleReply(reply);
    REQUIRE(frame.has_value());

    auto const error = ErrorOf(*frame);
    REQUIRE(error.has_value());
    CHECK(error->first == Wire::ErrorCode::UnsupportedVersion);

    // A rejection that does not say what WOULD work cannot be acted on: this
    // message is the only thing an operator with a mismatched install sees.
    CHECK(error->second.contains(std::to_string(static_cast<unsigned>(TooNew))));
    CHECK(error->second.contains(std::to_string(static_cast<unsigned>(Wire::CurrentVersion))));
}

TEST_CASE("An unknown opcode is rejected but the connection survives", "[compile-cache][handler][version]")
{
    // The load-bearing test for the declared payload length: the server can only
    // answer the second command if it stepped over the first by exactly the
    // length that frame declared. This is what makes adding a verb in a later
    // version a non-breaking change.
    CcFixture fix;

    CompileValue value;
    value.objectBlob = { std::byte { 0x42 } };
    REQUIRE(fix.engine.Set("k", EncodeCompileValue(value), /*flags=*/0, /*exptime=*/0).has_value());

    // A frame with a verb this build does not know, carrying a real payload.
    auto unknown = FetchFrame("payload-the-server-must-skip");
    unknown[2] = std::byte { 0xEE };

    auto const replies = SplitReplies(ExchangeWith(fix, Concat({ unknown, FetchFrame("k") }), SessionContext {}));

    REQUIRE(replies.size() == 2);

    auto const error = ErrorOf(replies[0]);
    REQUIRE(error.has_value());
    CHECK(error->first == Wire::ErrorCode::UnknownOpcode);

    CHECK(replies[1].status == Wire::Status::Ok);
    CHECK_FALSE(replies[1].payload.empty());
}

TEST_CASE("A FETCH miss and a rejected FETCH are distinguishable", "[compile-cache][handler][version]")
{
    // Both were the byte 0x00 before the format carried a status space, so a
    // version-rejected client saw an endlessly cold cache and no diagnostic.
    CcFixture missFixture;
    auto const miss = SoleReply(Exchange(missFixture, FetchFrame("absent")));
    REQUIRE(miss.has_value());
    CHECK(miss->status == Wire::Status::Miss);

    CcFixture rejectedFixture;
    auto const rejected =
        SoleReply(Exchange(rejectedFixture, FetchFrame("absent", static_cast<Wire::WireVersion>(Wire::CurrentVersion + 1))));
    REQUIRE(rejected.has_value());
    CHECK(rejected->status == Wire::Status::Error);

    CHECK(miss->status != rejected->status);
}

TEST_CASE("A refused STORE is drainable without knowing the command", "[compile-cache][handler][version]")
{
    // The error payload declares its length, so a client that reads only the
    // reply header can skip it and stay in sync. Without that, the trailing
    // message desynchronised any connection that carried a second command.
    CcFixture fix;

    // A STORE whose value is not a decodable compile-value.
    auto const junk = std::vector<std::byte> { std::byte { 0xEE }, std::byte { 0xEE } };
    auto const badStore = Wire::EncodeStore(Wire::StoreRequest {
        .key = "k", .cohort = "", .srcRoot = "", .buildTree = "", .value = std::span<std::byte const> { junk } });

    auto const replies = SplitReplies(ExchangeWith(fix, Concat({ badStore, FetchFrame("anything") }), SessionContext {}));

    REQUIRE(replies.size() == 2);
    auto const error = ErrorOf(replies[0]);
    REQUIRE(error.has_value());
    CHECK(error->first == Wire::ErrorCode::MalformedValue);
    CHECK(replies[1].status == Wire::Status::Miss);
}

TEST_CASE("A wire version change mid-connection is rejected", "[compile-cache][handler][version]")
{
    CcFixture fix;
    auto const first = FetchFrame("a", Wire::CurrentVersion);
    auto const second = FetchFrame("b", static_cast<Wire::WireVersion>(Wire::CurrentVersion + 1));

    auto const replies = SplitReplies(ExchangeWith(fix, Concat({ first, second }), SessionContext {}));

    REQUIRE(replies.size() == 2);
    CHECK(replies[0].status == Wire::Status::Miss);
    auto const error = ErrorOf(replies[1]);
    REQUIRE(error.has_value());
    CHECK(error->first == Wire::ErrorCode::UnsupportedVersion);
}

TEST_CASE("An oversize declared payload is rejected before it is read", "[compile-cache][handler][version]")
{
    // The declared total is checked against the cap up front. The pre-version
    // format could only discover this field by field, after the reader had
    // already taken the memory.
    CcFixture fix;
    SessionContext tinySession {};
    tinySession.maxPayloadBytes = 8;

    auto const reply = ExchangeWith(fix, FetchFrame("a-key-longer-than-the-cap"), tinySession);
    auto const frame = SoleReply(reply);
    REQUIRE(frame.has_value());

    auto const error = ErrorOf(*frame);
    REQUIRE(error.has_value());
    CHECK(error->first == Wire::ErrorCode::PayloadTooLarge);
}

TEST_CASE("A frame whose fields do not fill its declared payload is rejected", "[compile-cache][handler][version]")
{
    // The declared total and the per-field lengths are redundant by design;
    // disagreement is a typed refusal rather than a silent reinterpretation.
    auto const good = FetchFrame("ab");

    SECTION("declared payload longer than the fields supply")
    {
        CcFixture fix;
        auto frame = good;
        frame.push_back(std::byte { 0x00 });                                    // one trailing byte...
        frame[6] = static_cast<std::byte>(static_cast<unsigned>(frame[6]) + 1); // ...and a length that admits it

        auto const reply = SoleReply(Exchange(fix, frame));
        REQUIRE(reply.has_value());
        auto const error = ErrorOf(*reply);
        REQUIRE(error.has_value());
        CHECK(error->first == Wire::ErrorCode::MalformedFrame);
    }

    SECTION("field length overruns the declared payload")
    {
        CcFixture fix;
        auto frame = good;
        frame[10] = static_cast<std::byte>(0xFF); // field claims 255 bytes, payload holds 2

        auto const reply = SoleReply(Exchange(fix, frame));
        REQUIRE(reply.has_value());
        auto const error = ErrorOf(*reply);
        REQUIRE(error.has_value());
        CHECK(error->first == Wire::ErrorCode::MalformedFrame);
    }
}

TEST_CASE("Every op in the table is dispatched", "[compile-cache][handler][version]")
{
    // Dispatch is a switch (one per protocol, per the project's rule), so this
    // walks the table and requires each row to reach a handler — the same
    // table-versus-code drift guard used for the CLI flag table.
    for (auto const& row: Wire::OpTable)
    {
        CcFixture fix;

        // A minimal well-formed frame for this op: the right number of empty fields.
        std::vector<std::byte> payload;
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, row.fieldCount))
            payload.insert(payload.end(), 4, std::byte { 0x00 });

        std::vector<std::byte> frame { Wire::Magic,
                                       static_cast<std::byte>(Wire::CurrentVersion),
                                       static_cast<std::byte>(row.code) };
        std::array<std::byte, sizeof(std::uint32_t)> lengthBytes {};
        WriteBigEndian<std::uint32_t>(lengthBytes, static_cast<std::uint32_t>(payload.size()));
        frame.insert(frame.end(), lengthBytes.begin(), lengthBytes.end());
        frame.insert(frame.end(), payload.begin(), payload.end());

        auto const reply = SoleReply(Exchange(fix, frame));
        REQUIRE(reply.has_value());

        // Whatever it answers, it must not be "I do not know this verb".
        if (auto const error = ErrorOf(*reply); error.has_value())
            CHECK(error->first != Wire::ErrorCode::UnknownOpcode);
    }
}

TEST_CASE("A dropped frame is logged", "[compile-cache][handler][version]")
{
    // Every other handler reports a frame drop through the session logger; this
    // one reported nothing at all, so a rejection was invisible to the operator
    // as well as to the client.
    CcFixture fix;
    CapturingLogger logger { LogLevel::Trace };
    SessionContext session {};
    session.logger = &logger;

    auto frame = FetchFrame("k");
    frame[0] = std::byte { 0x80 }; // a magic that is not ours

    (void) ExchangeWith(fix, frame, session);

    auto const records = logger.Snapshot();
    CHECK(std::ranges::any_of(records, [](auto const& record) { return record.message.contains("compile-cache"); }));
}

TEST_CASE("Cross-depth: value stored from a deep layout localizes for a shallow consumer",
          "[compile-cache][handler][crossdepth]")
{
    CcFixture fix;

    CompileValue v;
    v.objectBlob = { std::byte { 0x01 } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\ci\deep\runner\src\inc\x.h)"
                                       "\r\n" });

    // Produced at a deep path (CI runner nested deeper).
    auto const storeReply = Exchange(fix,
                                     StoreFrame({ .key = "k",
                                                  .cohort = "envCI",
                                                  .srcRoot = R"(C:\ci\deep\runner\src)",
                                                  .buildTree = R"(C:\ci\deep\runner\build)" },
                                                v));
    auto const storeFrame = SoleReply(storeReply);
    REQUIRE(storeFrame.has_value());
    REQUIRE(storeFrame->status == Wire::Status::Ok);

    // Fetch the canonical value and localize it as a SHALLOW consumer would.
    InMemorySocketPair pair2 = InMemorySocketPair::Create();
    CompileCacheHandler handler2;
    REQUIRE(SyncRun(WriteBytes(pair2.client.get(), FetchFrame("k"))));
    pair2.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler2.Run(pair2.server.get(), &fix.engine, {}, session));
    auto const reply = SyncRun(ReadAvailable(pair2.client.get()));

    auto const decoded = DecodeFetchHit(reply);
    REQUIRE(decoded.has_value());
    auto const fetched = decoded.value_or(CompileValue {});
    REQUIRE(fetched.textRegions.size() == 1);

    PathCanon::Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    auto const localized = PathCanon::LocalizeRegion(fetched.textRegions[0].bytes, Grammar::ShowIncludes, consumer);
    REQUIRE(localized.has_value());
    CHECK(localized->contains(R"(D:\project\inc\x.h)"));
    CHECK_FALSE(localized->contains(R"(ci\deep)"));
}

namespace
{

/// A single valid compile value for cohort tests.
CompileValue SampleValue()
{
    CompileValue v;
    v.objectBlob = { std::byte { 0x7F } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes,
                              .bytes = "Note: including file: "
                                       R"(C:\src\h.h)"
                                       "\r\n" });
    return v;
}

/// Store `key` under `cohort` via a fresh handler over `engine`.
void StoreVia(CacheEngine& engine, std::string_view key, std::string_view cohort)
{
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
    auto const frame =
        StoreFrame({ .key = key, .cohort = cohort, .srcRoot = R"(C:\src)", .buildTree = R"(C:\build)" }, SampleValue());
    REQUIRE(SyncRun(WriteBytes(pair.client.get(), frame)));
    pair.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler.Run(pair.server.get(), &engine, {}, session));
    (void) SyncRun(ReadAvailable(pair.client.get()));
}

} // namespace

TEST_CASE("FETCH of a cohort member warms the rest of the cohort into L1", "[compile-cache][handler][prefetch]")
{
    ManualClock clock;
    auto l1 = std::make_unique<InMemoryLruStorage>(0);
    auto l2 = std::make_unique<InMemoryLruStorage>(0);
    LayeredStorage layered { std::move(l1), std::move(l2) };
    CacheEngine engine { layered, clock };

    // Store three cohort members.
    StoreVia(engine, "k1", "envCI");
    StoreVia(engine, "k2", "envCI");
    StoreVia(engine, "k3", "envCI");

    // Evict the L1 mirror so members live only in L2 (cold L1).
    layered.L1().EraseIfPresent("k1");
    layered.L1().EraseIfPresent("k2");
    layered.L1().EraseIfPresent("k3");
    REQUIRE_FALSE(layered.L1().Peek("k2", clock.Now())->found);
    REQUIRE_FALSE(layered.L1().Peek("k3", clock.Now())->found);

    // Fetch the leading key: triggers a cohort prefetch of k2/k3 into L1.
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
    REQUIRE(SyncRun(WriteBytes(pair.client.get(), FetchFrame("k1"))));
    pair.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler.Run(pair.server.get(), &engine, {}, session));
    (void) SyncRun(ReadAvailable(pair.client.get()));

    // k2 and k3 are now warm in L1 (a direct L1 Peek finds them).
    CHECK(layered.L1().Peek("k2", clock.Now())->found);
    CHECK(layered.L1().Peek("k3", clock.Now())->found);
}

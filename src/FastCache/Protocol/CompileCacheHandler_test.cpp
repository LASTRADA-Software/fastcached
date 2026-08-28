// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseTable.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;
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
///
/// Carries its own validity rather than being wrapped in an optional. Catch2's
/// REQUIRE is a macro that clang-tidy's optional analysis cannot see through, so
/// every `REQUIRE(x.has_value())` followed by `x->…` reads as an unchecked
/// access; a plain flag keeps these helpers assertable without a `value_or`
/// dance at each of two dozen call sites.
struct ReplyFrame
{
    bool present { false };                     ///< False when no such reply was decoded.
    Wire::Status status { Wire::Status::Miss }; ///< Meaningful only when `present`.
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
        frames.push_back(
            ReplyFrame { .present = true, .status = header->status, .payload = { payload.begin(), payload.end() } });
        offset += header->payloadLength;
    }
    return frames;
}

/// Decode the single reply expected from an exchange.
/// @param bytes The raw stream.
/// @return The reply; `present` is false unless there was exactly one.
ReplyFrame SoleReply(std::span<std::byte const> bytes)
{
    auto frames = SplitReplies(bytes);
    if (frames.size() != 1)
        return ReplyFrame {};
    return std::move(frames.front());
}

/// The four string fields of a STORE frame. Named rather than positional so the
/// same-typed fields cannot be transposed at a call site.
struct StoreFields
{
    std::string_view key;
    std::string_view prefetchGroup;
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
                                                  .prefetchGroup = fields.prefetchGroup,
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
/// @param reply The raw reply stream.
/// @return The value; `present` is false unless the reply was a decodable hit.
struct FetchedValue
{
    bool present { false };
    CompileValue value;
};

FetchedValue DecodeFetchHit(std::span<std::byte const> reply)
{
    auto const frame = SoleReply(reply);
    if (!frame.present || frame.status != Wire::Status::Ok)
        return {};
    auto decoded = DecodeCompileValue(frame.payload);
    if (!decoded.has_value())
        return {};
    return FetchedValue { .present = true, .value = *std::move(decoded) };
}

/// The code and message carried by an Error reply.
struct DecodedError
{
    bool present { false };
    Wire::ErrorCode code { Wire::ErrorCode::MalformedFrame };
    std::string message;
};

DecodedError ErrorOf(ReplyFrame const& frame)
{
    if (!frame.present || frame.status != Wire::Status::Error)
        return {};
    auto const decoded = Wire::DecodeErrorPayload(frame.payload);
    if (!decoded.has_value())
        return {};
    return DecodedError { .present = true, .code = decoded->first, .message = std::string { decoded->second } };
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
    auto const storeReply = Exchange(fix,
                                     StoreFrame({ .key = "obj-hash",
                                                  .prefetchGroup = "envCI",
                                                  .srcRoot = R"(C:\ci\deep\src)",
                                                  .buildTree = R"(C:\ci\deep\build)" },
                                                v));
    auto const storeFrame = SoleReply(storeReply);
    REQUIRE(storeFrame.present);
    CHECK(storeFrame.status == Wire::Status::Ok);

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

    auto const decoded = DecodeFetchHit(fetchReply);
    REQUIRE(decoded.present);
    auto const& fetched = decoded.value;
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

    auto const storeReply = Exchange(fix,
                                     StoreFrame({ .key = "big-obj",
                                                  .prefetchGroup = "envCI",
                                                  .srcRoot = R"(C:\ci\deep\src)",
                                                  .buildTree = R"(C:\ci\deep\build)" },
                                                v));
    auto const storeFrame = SoleReply(storeReply);
    REQUIRE(storeFrame.present);
    REQUIRE(storeFrame.status == Wire::Status::Ok);

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
    REQUIRE(Unwrap(header).status == Wire::Status::Ok);
    auto const declared = Unwrap(header).payloadLength;
    REQUIRE(declared > 1024U * 1024U);

    auto const payload = SyncRun(ReadExactlyN(pair2.client.get(), declared));
    REQUIRE(payload.size() == declared);

    // value_or keeps the access unguarded-safe: static analysis cannot see a
    // has_value() guard through Catch2's REQUIRE macro.
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
    REQUIRE(frame.present);
    CHECK(frame.status == Wire::Status::Miss);
    CHECK(frame.payload.empty());
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
    REQUIRE(frame.present);

    auto const error = ErrorOf(frame);
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::UnsupportedVersion);

    // A rejection that does not say what WOULD work cannot be acted on: this
    // message is the only thing an operator with a mismatched install sees.
    CHECK(error.message.contains(std::to_string(static_cast<unsigned>(TooNew))));
    CHECK(error.message.contains(std::to_string(static_cast<unsigned>(Wire::CurrentVersion))));
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

    auto const error = ErrorOf(replies.at(0));
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::UnknownOpcode);

    CHECK(replies.at(1).status == Wire::Status::Ok);
    CHECK_FALSE(replies.at(1).payload.empty());
}

TEST_CASE("A FETCH miss and a rejected FETCH are distinguishable", "[compile-cache][handler][version]")
{
    // Both were the byte 0x00 before the format carried a status space, so a
    // version-rejected client saw an endlessly cold cache and no diagnostic.
    CcFixture missFixture;
    auto const miss = SoleReply(Exchange(missFixture, FetchFrame("absent")));
    REQUIRE(miss.present);
    CHECK(miss.status == Wire::Status::Miss);

    CcFixture rejectedFixture;
    auto const rejected =
        SoleReply(Exchange(rejectedFixture, FetchFrame("absent", static_cast<Wire::WireVersion>(Wire::CurrentVersion + 1))));
    REQUIRE(rejected.present);
    CHECK(rejected.status == Wire::Status::Error);

    CHECK(miss.status != rejected.status);
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
        .key = "k", .prefetchGroup = "", .srcRoot = "", .buildTree = "", .value = std::span<std::byte const> { junk } });

    auto const replies = SplitReplies(ExchangeWith(fix, Concat({ badStore, FetchFrame("anything") }), SessionContext {}));

    REQUIRE(replies.size() == 2);
    auto const error = ErrorOf(replies.at(0));
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::MalformedValue);
    CHECK(replies.at(1).status == Wire::Status::Miss);
}

TEST_CASE("A wire version change mid-connection is rejected", "[compile-cache][handler][version]")
{
    CcFixture fix;
    auto const first = FetchFrame("a", Wire::CurrentVersion);
    auto const second = FetchFrame("b", static_cast<Wire::WireVersion>(Wire::CurrentVersion + 1));

    auto const replies = SplitReplies(ExchangeWith(fix, Concat({ first, second }), SessionContext {}));

    REQUIRE(replies.size() == 2);
    CHECK(replies.at(0).status == Wire::Status::Miss);
    auto const error = ErrorOf(replies.at(1));
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::UnsupportedVersion);
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
    REQUIRE(frame.present);

    auto const error = ErrorOf(frame);
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::PayloadTooLarge);
    // Naming both numbers is the point of the message: it is the only thing that
    // tells an operator which way to move --storage-max-value.
    CHECK(error.message.contains("exceeds cap 8"));
}

TEST_CASE("An oversize frame is refused but the connection survives", "[compile-cache][handler][version]")
{
    // The half that broke a build. Refusing an over-cap frame *without* reading
    // its body leaves the sender writing into a socket the server has stopped
    // reading and then closed, so it never sees the refusal it was answered
    // with -- it sees its own write fail, and before issue #68 died of SIGPIPE
    // doing so. Stepping over the body is what turns a dropped connection into
    // an answer, exactly as it does for an unknown opcode.
    CcFixture fix;
    SessionContext tinySession {};
    tinySession.maxPayloadBytes = 8;

    auto const replies =
        SplitReplies(ExchangeWith(fix, Concat({ FetchFrame("a-key-longer-than-the-cap"), FetchFrame("k") }), tinySession));
    REQUIRE(replies.size() == 2);

    auto const error = ErrorOf(replies[0]);
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::PayloadTooLarge);
    // The second command was read from the right offset, which is only possible
    // if the first frame's body was consumed in full.
    CHECK(replies[1].status == Wire::Status::Miss);
}

TEST_CASE("A frame too large even to discard is still answered, then closed", "[compile-cache][handler][version]")
{
    // The drain is bounded, so past the bound the server goes back to replying
    // and closing. The reply still goes out first: a sender that has already
    // finished writing can read it, and one that has not loses nothing it would
    // have had before.
    // Same cap as the case above, and a key long enough that its frame lands on
    // the far side of the drain allowance -- which is what makes the bound
    // itself observable rather than merely written down. The cap cannot go
    // lower to get there: a cap under the 7-byte request header would fail the
    // header read instead, before there is a frame to have an opinion about.
    CcFixture fix;
    SessionContext tinySession {};
    tinySession.maxPayloadBytes = 8; // drain allowance is a small multiple of this

    auto const replies = SplitReplies(
        ExchangeWith(fix, Concat({ FetchFrame("a-key-far-past-the-drain-bound"), FetchFrame("k") }), tinySession));
    REQUIRE(replies.size() == 1);

    auto const error = ErrorOf(replies[0]);
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::PayloadTooLarge);
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
        REQUIRE(reply.present);
        auto const error = ErrorOf(reply);
        REQUIRE(error.present);
        CHECK(error.code == Wire::ErrorCode::MalformedFrame);
    }

    SECTION("field length overruns the declared payload")
    {
        CcFixture fix;
        auto frame = good;
        frame[10] = static_cast<std::byte>(0xFF); // field claims 255 bytes, payload holds 2

        auto const reply = SoleReply(Exchange(fix, frame));
        REQUIRE(reply.present);
        auto const error = ErrorOf(reply);
        REQUIRE(error.present);
        CHECK(error.code == Wire::ErrorCode::MalformedFrame);
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
        REQUIRE(reply.present);

        // Whatever it answers, it must not be "I do not know this verb".
        if (auto const error = ErrorOf(reply); error.present)
            CHECK(error.code != Wire::ErrorCode::UnknownOpcode);
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
                                                  .prefetchGroup = "envCI",
                                                  .srcRoot = R"(C:\ci\deep\runner\src)",
                                                  .buildTree = R"(C:\ci\deep\runner\build)" },
                                                v));
    auto const storeFrame = SoleReply(storeReply);
    REQUIRE(storeFrame.present);
    REQUIRE(storeFrame.status == Wire::Status::Ok);

    // Fetch the canonical value and localize it as a SHALLOW consumer would.
    InMemorySocketPair pair2 = InMemorySocketPair::Create();
    CompileCacheHandler handler2;
    REQUIRE(SyncRun(WriteBytes(pair2.client.get(), FetchFrame("k"))));
    pair2.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler2.Run(pair2.server.get(), &fix.engine, {}, session));
    auto const reply = SyncRun(ReadAvailable(pair2.client.get()));

    auto const decoded = DecodeFetchHit(reply);
    REQUIRE(decoded.present);
    auto const& fetched = decoded.value;
    REQUIRE(fetched.textRegions.size() == 1);

    PathCanon::Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    auto const localized = PathCanon::LocalizeRegion(fetched.textRegions[0].bytes, Grammar::ShowIncludes, consumer);
    CHECK(localized.contains(R"(D:\project\inc\x.h)"));
    CHECK_FALSE(localized.contains(R"(ci\deep)"));
}

namespace
{

/// A single valid compile value for prefetch group tests.
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

/// Store `key` under `prefetch group` via a fresh handler over `engine`.
void StoreVia(CacheEngine& engine, std::string_view key, std::string_view prefetchGroup)
{
    InMemorySocketPair pair = InMemorySocketPair::Create();
    CompileCacheHandler handler;
    auto const frame = StoreFrame(
        { .key = key, .prefetchGroup = prefetchGroup, .srcRoot = R"(C:\src)", .buildTree = R"(C:\build)" }, SampleValue());
    REQUIRE(SyncRun(WriteBytes(pair.client.get(), frame)));
    pair.client->ShutdownWrite();
    SessionContext session {};
    SyncRun(handler.Run(pair.server.get(), &engine, {}, session));
    (void) SyncRun(ReadAvailable(pair.client.get()));
}

} // namespace

TEST_CASE("FETCH of a prefetch group member warms the rest of the prefetch group into L1",
          "[compile-cache][handler][prefetch]")
{
    ManualClock clock;
    auto l1 = std::make_unique<InMemoryLruStorage>(0);
    auto l2 = std::make_unique<InMemoryLruStorage>(0);
    LayeredStorage layered { std::move(l1), std::move(l2) };
    CacheEngine engine { layered, clock };

    // Store three prefetch group members.
    StoreVia(engine, "k1", "envCI");
    StoreVia(engine, "k2", "envCI");
    StoreVia(engine, "k3", "envCI");

    // Evict the L1 mirror so members live only in L2 (cold L1).
    layered.L1().EraseIfPresent("k1");
    layered.L1().EraseIfPresent("k2");
    layered.L1().EraseIfPresent("k3");
    REQUIRE_FALSE(layered.L1().Peek("k2", clock.Now())->found);
    REQUIRE_FALSE(layered.L1().Peek("k3", clock.Now())->found);

    // Fetch the leading key: triggers a group prefetch of k2/k3 into L1.
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

// --- authentication ---------------------------------------------------------
//
// This handler was the only one in the tree that never consulted
// SessionContext::CurrentAuth(): a daemon started with --requirepass gated
// memcached text, memcached binary and RESP, and served the compile cache to
// anyone. These cases are what keep that from being reintroduced by anybody who
// adds a verb without noticing the gate.

namespace
{

/// A session requiring `secret`, with the policy reachable through a live source
/// so the handler exercises the same rotation-capable path the daemon uses.
///
/// Returned as a pair because `SessionContext` holds a borrowed `IAuthSource*`:
/// the source has to outlive the session, and handing back both makes that the
/// caller's visible obligation rather than a dangling pointer waiting to happen.
struct AuthedSession
{
    std::unique_ptr<SharedAuthSource> source;
    SessionContext session;
};

[[nodiscard]] AuthedSession RequireSecret(std::string_view username, std::string_view secret)
{
    auto source = std::make_unique<SharedAuthSource>(
        std::make_shared<AuthPolicy const>(std::string { username }, std::string { secret }));
    SessionContext session {};
    session.authSource = source.get();
    return AuthedSession { .source = std::move(source), .session = session };
}

/// Build an AUTH frame.
[[nodiscard]] std::vector<std::byte> AuthFrame(std::string_view username,
                                               std::string_view secret,
                                               Wire::WireVersion version = Wire::CurrentVersion)
{
    return Wire::EncodeAuth(Wire::AuthRequest { .username = username, .secret = secret }, version);
}

} // namespace

TEST_CASE("Every gated verb is refused before AUTH, and the connection survives", "[compile-cache][handler][auth]")
{
    // Both gated verbs in one stream: the point is not that FETCH is refused but
    // that the refusal is a *reply* the peer can read and step over, so a second
    // command still gets an answer. A gate that closed the connection would pass
    // a single-command assertion and fail this one.
    CcFixture fix;
    auto authed = RequireSecret("", "s3cret");

    CompileValue v;
    v.objectBlob = { std::byte { 0x01 } };
    auto const reply = ExchangeWith(
        fix,
        Concat({ FetchFrame("k"),
                 StoreFrame({ .key = "k", .prefetchGroup = "g", .srcRoot = "/src", .buildTree = "/build" }, v) }),
        authed.session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);
    for (auto const& frame: frames)
    {
        auto const error = ErrorOf(frame);
        REQUIRE(error.present);
        CHECK(error.code == Wire::ErrorCode::Unauthenticated);
    }
}

TEST_CASE("An unauthenticated FETCH stores nothing and leaks nothing", "[compile-cache][handler][auth]")
{
    // The refusal must be a refusal, not a slow serve: assert on the engine, not
    // only on the reply. A gate that replied Unauthenticated *after* doing the
    // work would satisfy the case above.
    CcFixture fix;
    auto authed = RequireSecret("", "s3cret");

    CompileValue v;
    v.objectBlob = { std::byte { 0xAB }, std::byte { 0xCD } };
    (void) ExchangeWith(fix,
                        StoreFrame({ .key = "gated", .prefetchGroup = "g", .srcRoot = "/src", .buildTree = "/build" }, v),
                        authed.session);

    auto const got = fix.engine.Get("gated");
    REQUIRE(got.has_value());
    CHECK_FALSE(got->found);
}

TEST_CASE("A correct AUTH opens the connection for the commands pipelined behind it", "[compile-cache][handler][auth]")
{
    // Written as ONE stream rather than an exchange per frame, because that is
    // exactly how the launcher sends it: AUTH and the real command go out in a
    // single write and both replies are read afterwards. If the handler ever
    // required a round trip between them, this case is what fails.
    CcFixture fix;
    auto authed = RequireSecret("", "s3cret");

    CompileValue v;
    v.objectBlob = { std::byte { 0x11 }, std::byte { 0x22 } };
    auto const reply =
        ExchangeWith(fix,
                     Concat({ AuthFrame("", "s3cret"),
                              StoreFrame({ .key = "k", .prefetchGroup = "g", .srcRoot = "/src", .buildTree = "/build" }, v),
                              FetchFrame("k") }),
                     authed.session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].status == Wire::Status::Ok); // AUTH accepted
    CHECK(frames[1].status == Wire::Status::Ok); // STORE landed
    REQUIRE(frames[2].status == Wire::Status::Ok);

    auto const decoded = DecodeCompileValue(frames[2].payload);
    REQUIRE(decoded.has_value());
    CHECK(decoded->objectBlob == v.objectBlob);
}

TEST_CASE("A wrong secret is refused and does not open the connection", "[compile-cache][handler][auth]")
{
    CcFixture fix;
    auto authed = RequireSecret("", "s3cret");

    auto const reply = ExchangeWith(fix, Concat({ AuthFrame("", "wrong"), FetchFrame("k") }), authed.session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);

    auto const authError = ErrorOf(frames[0]);
    REQUIRE(authError.present);
    CHECK(authError.code == Wire::ErrorCode::Unauthenticated);

    // The command behind it is still gated — a failed attempt must not count as
    // an attempt made.
    auto const fetchError = ErrorOf(frames[1]);
    REQUIRE(fetchError.present);
    CHECK(fetchError.code == Wire::ErrorCode::Unauthenticated);
}

TEST_CASE("A named user authenticates with its username, and the secret alone does not", "[compile-cache][handler][auth]")
{
    SECTION("username and secret together are accepted")
    {
        CcFixture fix;
        auto authed = RequireSecret("builder", "s3cret");
        auto const reply = ExchangeWith(fix, AuthFrame("builder", "s3cret"), authed.session);
        CHECK(SoleReply(reply).status == Wire::Status::Ok);
    }
    SECTION("a wrong username with the right secret is refused")
    {
        CcFixture fix;
        auto authed = RequireSecret("builder", "s3cret");
        auto const reply = ExchangeWith(fix, AuthFrame("intruder", "s3cret"), authed.session);
        CHECK(ErrorOf(SoleReply(reply)).code == Wire::ErrorCode::Unauthenticated);
    }
}

TEST_CASE("With auth disabled a credential is accepted and ignored", "[compile-cache][handler][auth]")
{
    // A launcher configured with FASTCACHE_TOKEN must keep working against a
    // daemon that requires none — otherwise setting a token becomes a breaking
    // change against every unauthenticated daemon in a mixed fleet, which is
    // precisely the migration this feature has to survive.
    CcFixture fix;
    auto const reply = Exchange(fix, Concat({ AuthFrame("", "any-token"), FetchFrame("absent") }));

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].status == Wire::Status::Ok);
    CHECK(frames[1].status == Wire::Status::Miss);
}

TEST_CASE("Enabling auth by reload gates a connection that is already open", "[compile-cache][handler][auth]")
{
    // The policy is resolved per command from the live source, so a SIGHUP that
    // *enables* auth reaches connections that are already open. Getting this
    // wrong is silent: seeding a per-connection "authenticated" flag from the
    // policy at connect time -- the obvious spelling -- leaves every connection
    // that predates the reload exempt for its whole life, which on a long-lived
    // pool is indistinguishable from auth simply not working.
    //
    // Driven through a source that arms itself after its first read. That leans
    // on the loop reading the policy exactly once per command, which is a stated
    // property of the handler rather than an accident: the gate and the verify
    // must see the same policy, so they share one read.
    struct ArmingSource final: IAuthSource
    {
        std::shared_ptr<AuthPolicy const> policy;
        mutable int reads { 0 };

        [[nodiscard]] std::shared_ptr<AuthPolicy const> Current() const noexcept override
        {
            ++reads;
            return reads >= 2 ? policy : std::shared_ptr<AuthPolicy const> {};
        }
    };

    CcFixture fix;
    ArmingSource source;
    source.policy = std::make_shared<AuthPolicy const>(std::string {}, std::string { "s3cret" });
    SessionContext session {};
    session.authSource = &source;

    auto const reply = ExchangeWith(fix, Concat({ FetchFrame("k"), FetchFrame("k") }), session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);
    // Before the reload: served normally (a miss, since nothing is stored).
    CHECK(frames[0].status == Wire::Status::Miss);
    // After it: the same command on the same connection is now gated.
    auto const error = ErrorOf(frames[1]);
    REQUIRE(error.present);
    CHECK(error.code == Wire::ErrorCode::Unauthenticated);
}

TEST_CASE("A verified credential survives a secret rotation", "[compile-cache][handler][auth]")
{
    // The other direction, and deliberately the opposite answer: a peer that
    // proved the credential current when it connected keeps its access when the
    // secret is rotated under it. Re-gating on rotation would fail every
    // in-flight build the moment an operator rotates, which is what makes
    // rotation something nobody dares do. Redis behaves the same way.
    struct RotatingSource final: IAuthSource
    {
        std::shared_ptr<AuthPolicy const> first;
        std::shared_ptr<AuthPolicy const> second;
        mutable int reads { 0 };

        [[nodiscard]] std::shared_ptr<AuthPolicy const> Current() const noexcept override
        {
            ++reads;
            return reads <= 1 ? first : second;
        }
    };

    CcFixture fix;
    RotatingSource source;
    source.first = std::make_shared<AuthPolicy const>(std::string {}, std::string { "old-secret" });
    source.second = std::make_shared<AuthPolicy const>(std::string {}, std::string { "new-secret" });
    SessionContext session {};
    session.authSource = &source;

    auto const reply = ExchangeWith(fix, Concat({ AuthFrame("", "old-secret"), FetchFrame("k") }), session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].status == Wire::Status::Ok);   // proved against the old secret
    CHECK(frames[1].status == Wire::Status::Miss); // still served after rotation
}

TEST_CASE("A gated frame is drained by its declared length, not buffered", "[compile-cache][handler][auth]")
{
    // The gate runs before the payload is read: an unauthenticated peer could
    // otherwise pipeline frames declaring the full payload cap and make the
    // server allocate all of it per frame before being told to authenticate.
    // Observable as framing rather than as footprint — a handler that failed to
    // step over the refused body would find the next header inside it and drop
    // the connection instead of answering the second command.
    CcFixture fix;
    auto authed = RequireSecret("", "s3cret");

    CompileValue big;
    big.objectBlob.assign(64 * 1024, std::byte { 0x5A });
    auto const reply = ExchangeWith(
        fix,
        Concat({ StoreFrame({ .key = "big", .prefetchGroup = "g", .srcRoot = "/src", .buildTree = "/build" }, big),
                 FetchFrame("after") }),
        authed.session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);
    CHECK(ErrorOf(frames[0]).code == Wire::ErrorCode::Unauthenticated);
    CHECK(ErrorOf(frames[1]).code == Wire::ErrorCode::Unauthenticated);
}

TEST_CASE("An oversize AUTH is refused on its own ceiling, not the session's", "[compile-cache][handler][auth]")
{
    // AUTH is the one verb deliberately reachable before authentication, which
    // makes it the one hole in the pre-auth allocation gate. Without a ceiling of
    // its own it is read with `ReadExactly(payloadLength)` bounded only by
    // `maxPayloadBytes` -- 256 MiB by default -- so an unauthenticated peer gets
    // exactly the allocation the gate exists to deny, through the door the gate
    // holds open. A credential does not need 256 MiB.
    //
    // Two frames, because the refusal must also be recoverable: the oversize body
    // is drained by its declared length, so the command behind it still gets an
    // answer rather than the connection being dropped.
    CcFixture fix;
    auto authed = RequireSecret("", "s3cret");

    std::string const huge(Wire::MaxAuthPayload + 1, 'x');
    auto const reply = ExchangeWith(fix, Concat({ AuthFrame("", huge), FetchFrame("k") }), authed.session);

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);

    auto const authError = ErrorOf(frames[0]);
    REQUIRE(authError.present);
    CHECK(authError.code == Wire::ErrorCode::PayloadTooLarge);
    // The message must name the verb whose ceiling was hit, not the session cap:
    // an operator reading "exceeds cap 268435456" for a 4 KiB limit learns nothing.
    CHECK(authError.message.contains("auth"));

    // Still gated, and still answering.
    CHECK(ErrorOf(frames[1]).code == Wire::ErrorCode::Unauthenticated);
}

TEST_CASE("A credential right up against the ceiling is still accepted", "[compile-cache][handler][auth]")
{
    // The bound must be a bound, not an off-by-one that quietly rejects the
    // largest legal credential.
    CcFixture fix;
    std::string const secret(Wire::MaxAuthPayload - (2 * sizeof(std::uint32_t)), 'x');
    auto authed = RequireSecret("", secret);

    auto const reply = ExchangeWith(fix, AuthFrame("", secret), authed.session);
    CHECK(SoleReply(reply).status == Wire::Status::Ok);
}

TEST_CASE("A gated verb keeps the operator's cap, not the AUTH ceiling", "[compile-cache][handler][auth]")
{
    // The per-op ceiling must not leak onto verbs that legitimately carry an
    // object file. A STORE larger than MaxAuthPayload is ordinary traffic, and a
    // shared ceiling would cap every cached object at 4 KiB.
    CcFixture fix;

    CompileValue v;
    v.objectBlob.assign(Wire::MaxAuthPayload * 4, std::byte { 0x7E });
    auto const reply =
        Exchange(fix, StoreFrame({ .key = "big", .prefetchGroup = "g", .srcRoot = "/src", .buildTree = "/build" }, v));

    CHECK(SoleReply(reply).status == Wire::Status::Ok);
    auto const got = fix.engine.Get("big");
    REQUIRE(got.has_value());
    CHECK(got->found);
}

// --- distributed execution: answered, never served ---------------------------

TEST_CASE("The cache refuses every scheduling verb, and says where they went", "[compile-cache][handler][distributed]")
{
    // `fastcached` is a cache and nothing else. The fleet's scheduler moved to
    // `fastcache-compile-node --listen-scheduler`, because handing out capacity is a
    // decision only one node may make at a time and nothing here can establish which
    // node that is.
    //
    // The verbs keep their place in `OpTable` and keep getting a TYPED refusal rather
    // than being dropped, and both halves matter. A client built against an older
    // daemon has to learn why its scheduling stopped: a closed connection is
    // indistinguishable from a dead host, and an unknown opcode would say this daemon
    // is too old when it is in fact too new.
    CcFixture fix;

    auto const reply = ExchangeWith(
        fix,
        Concat({ Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc", .key = "k", .acceptedCodecs = {} }),
                 Wire::EncodeRegister(
                     Wire::RegisterRequest { .fingerprint = "gcc", .endpoint = "h:1", .slots = 1, .acceptedCodecs = {} }),
                 Wire::EncodeHeartbeat("w1", 0),
                 Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = "l1", .key = "k" }) }),
        SessionContext {});

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 4);
    for (auto const& frame: frames)
    {
        CHECK(ErrorOf(frame).code == Wire::ErrorCode::DispatchNotPermitted);
        // A refusal that cannot say what would have worked cannot be acted on.
        CHECK(ErrorOf(frame).message.contains("fastcache-compile-node"));
    }
}

TEST_CASE("A compile sent to the cache is refused with its own message", "[compile-cache][handler][distributed]")
{
    // Distinct wording, because "you sent the job to the cache instead of to the
    // worker the lease named" is a different client bug from "this is not a
    // scheduler", and both would otherwise present as one unexplained refusal.
    CcFixture fix;

    auto const reply = ExchangeWith(fix,
                                    Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "t",
                                                                               .fingerprint = "gcc",
                                                                               .args = {},
                                                                               .source = {},
                                                                               .acceptedCodecs = {},
                                                                               .sourceName = "t.cpp" }),
                                    SessionContext {});

    auto const refusal = ErrorOf(SoleReply(reply));
    CHECK(refusal.code == Wire::ErrorCode::DispatchNotPermitted);
    CHECK(refusal.message.contains("does not execute compiles"));
}

TEST_CASE("An oversize control frame is refused on the control ceiling", "[compile-cache][handler][distributed]")
{
    // Unchanged by the scheduler's departure, and deliberately kept: the ceiling is a
    // property of the FRAMING rather than of who answers the verb, and the payload is
    // still drained so the connection stays usable for whatever was pipelined behind
    // it. A refusal that left bytes on the socket would desynchronize the stream,
    // which is the defect the declared-length header exists to prevent.
    CcFixture fix;

    std::string const huge(Wire::MaxControlPayload + 1, 'k');
    auto const reply = ExchangeWith(
        fix,
        Concat({ Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-13", .key = huge, .acceptedCodecs = {} }),
                 Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-13", .key = "k", .acceptedCodecs = {} }) }),
        SessionContext {});

    auto const frames = SplitReplies(reply);
    REQUIRE(frames.size() == 2);
    CHECK(ErrorOf(frames[0]).code == Wire::ErrorCode::PayloadTooLarge);
    CHECK(ErrorOf(frames[0]).message.contains("lease"));
    // Still answering afterwards, which is the point of draining rather than closing.
    CHECK(ErrorOf(frames[1]).code == Wire::ErrorCode::DispatchNotPermitted);
}

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::CompileCacheWire;
using FastCache::Testing::Unwrap;

namespace
{

/// Build a byte vector from an initializer list of integer literals, so an
/// expected wire layout can be written out as the hex it actually is.
[[nodiscard]] std::vector<std::byte> Bytes(std::initializer_list<int> values)
{
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (auto const value: values)
        out.push_back(static_cast<std::byte>(value));
    return out;
}

} // namespace

// --- layout pins -----------------------------------------------------------
//
// These are the tests that make the format a contract rather than whatever the
// current code happens to emit. A change to any byte position below is a wire
// break and must be a deliberate version bump, not an accident.

TEST_CASE("The wire constants have their specified byte values", "[wire]")
{
    CHECK(static_cast<std::uint8_t>(Magic) == 0xFC);
    // Version 12 puts enrollment on keys (#178 PR 4): ENROLL carries a role and the joiner's
    // identity key, two more fields of an exact arity, and its approved reply carries the
    // cluster's ROSTER where it carried the cluster key. The floor moves with it, because a
    // leader that still read a version-11 ENROLL would have to keep a key-handing path alive
    // for it.
    //
    // Version 11 gives CLUSTER-ADMIT and CLUSTER-ADMIT-LEARNER a member's identity key, as a
    // third field of the request and a third field of the receipt (#178): two exact arities
    // moved, which no reader can step over.
    //
    // Version 10 makes the NODE-METRICS reply a `StatsReading` (#1406): the body of an existing
    // verb changed shape, which no reader can step over.
    //
    // Version 9 adds `Status::Push` and `Op::Subscribe`, the live-stats stream (#1399):
    // a new reply status is a new grammar for every reader, so the floor moves with it.
    //
    // Version 8 gives the CLUSTER-ADMIT reply a receipt, so the id and endpoint the
    // leader recorded come back as bytes an operator can read against the machine being
    // admitted (#1296). Version 7 gave the NODE-STATUS reply a nested runtime record
    // (#1294, #1295); version 6 put the lease's lifetime on the LEASE grant (#522);
    // version 5 added COMPILE's source-root pair (#883).
    //
    // Both move together, and version 8's reason is the INVERSE of version 7's --
    // `MinSupportedVersion` carries both arguments in full. In short: 7's older client
    // fails loudly and cannot be served, while 8's older client SUCCEEDS quietly, reads
    // no reply body for this verb, and drops the one string the change exists to put on
    // the screen. A missing string does not announce itself as missing, so leaving the
    // floor at 7 would manufacture a quiet failure inside the fix for one.
    //
    // Version 13 (#178) gives NODE-ANNOUNCE a fourth field -- a voter's roster endorsement --
    // and its `Ok` a body, the certified roster. The arity is exact, so an older peer cannot
    // read the request at all, and both move for that reason.
    //
    // Version 14 (#178) replaces the shared-key node proof with a signed handshake and SEALS
    // every frame after it. The challenge now carries the caller's nonce and ephemeral key, and
    // a version-13 peer's empty `NodeChallenge` is a frame this build refuses; a version-13
    // peer reading a sealed reply would take its tag for the next frame's header. Both move.
    CHECK(CurrentVersion == 14);
    CHECK(MinSupportedVersion == 14);
    CHECK(RequestHeaderSize == 7);
    CHECK(ReplyHeaderSize == 5);

    CHECK(static_cast<std::uint8_t>(Op::Store) == 0x01);
    CHECK(static_cast<std::uint8_t>(Op::Fetch) == 0x02);

    CHECK(static_cast<std::uint8_t>(Status::Miss) == 0x00);
    CHECK(static_cast<std::uint8_t>(Status::Ok) == 0x01);
    CHECK(static_cast<std::uint8_t>(Status::Error) == 0x02);
    CHECK(static_cast<std::uint8_t>(Status::Progress) == 0x03);
    CHECK(static_cast<std::uint8_t>(Status::Push) == 0x04);
    CHECK(static_cast<std::uint8_t>(Op::Subscribe) == 0x12);
    // Taken past the bytes parallel branches already held (#1303's cordon, the fleet text verb), so
    // `Op` has a gap until they land. `EveryOpcodeIsDistinct` is what stops two branches that each
    // took one byte from merging into a table where one verb is served as the other.
    CHECK(static_cast<std::uint8_t>(Op::CacheDrop) == 0x15);

    // The BYTE, not the symbol. A constant on this wire carries two facts -- its
    // name and its value -- and a peer built from another revision of this header
    // agrees about only the second. Every in-tree caller spells the enumerator, so a
    // consistent renumbering leaves the whole suite green while every deployed
    // launcher reads a different refusal, and nobody here can recompile them. Pinned
    // for the newest codes because those are the ones an author is about to move; the
    // older neighbours are pinned by `RetiredErrorCodes` and by the peers already
    // running.
    CHECK(static_cast<std::uint8_t>(ErrorCode::RequestDeadlineExceeded) == 0x1D);
    CHECK(static_cast<std::uint8_t>(ErrorCode::ForeignValueGeneration) == 0x1E);

    // `MalformedValue` is pinned here as the one exception to "older neighbours are
    // pinned by the peers already running", because #544 made this PAIR load-bearing
    // rather than the codes individually: a foreign generation and bytes that are not
    // a compile value are now different facts with different operator remedies, and
    // the only thing separating them for a peer built elsewhere is that these two
    // bytes differ. Asserting the codes differ would not catch a renumbering that
    // moved both.
    CHECK(static_cast<std::uint8_t>(ErrorCode::MalformedValue) == 0x05);
}

TEST_CASE("EncodeFetch emits the specified bytes exactly", "[wire]")
{
    auto const frame = EncodeFetch("ab");

    // clang-format off: the grid IS the specification -- one wire field per row.
    auto const expected = Bytes({
        0xFC,                   // magic
        0x0E,                   // version
        0x02,                   // op = Fetch
        0x00, 0x00, 0x00, 0x06, // payloadLength = 6
        0x00, 0x00, 0x00, 0x02, // field[0] length = 2
        0x61, 0x62,             // "ab"
    });
    // clang-format on

    CHECK(frame == expected);
}

TEST_CASE("EncodeStore emits the specified bytes exactly", "[wire]")
{
    auto const value = Bytes({ 0xAA, 0xBB });
    auto const frame = EncodeStore(StoreRequest {
        .key = "k", .prefetchGroup = "", .srcRoot = "s", .buildTree = "b", .value = std::span<std::byte const> { value } });

    auto const expected = Bytes({
        0xFC,                               // magic
        0x0E,                               // version
        0x01,                               // op = Store
        0x00, 0x00, 0x00, 0x19,             // payloadLength = 25 = (4+1) + (4+0) + (4+1) + (4+1) + (4+2)
        0x00, 0x00, 0x00, 0x01, 0x6B,       // key           = "k"
        0x00, 0x00, 0x00, 0x00,             // prefetchGroup = "" (empty, still length-prefixed)
        0x00, 0x00, 0x00, 0x01, 0x73,       // srcRoot       = "s"
        0x00, 0x00, 0x00, 0x01, 0x62,       // buildTree     = "b"
        0x00, 0x00, 0x00, 0x02, 0xAA, 0xBB, // value
    });

    CHECK(frame == expected);
}

TEST_CASE("EncodeErrorReply emits the specified bytes exactly", "[wire]")
{
    auto const reply = EncodeErrorReply(ErrorCode::UnsupportedVersion, "x");

    // clang-format off: the grid IS the specification -- one wire field per row.
    auto const expected = Bytes({
        0x02,                   // status = Error
        0x00, 0x00, 0x00, 0x02, // payloadLength = 2
        0x01,                   // ErrorCode::UnsupportedVersion
        0x78,                   // "x"
    });
    // clang-format on

    CHECK(reply == expected);
}

TEST_CASE("A miss reply is a zero-length payload, not an absent one", "[wire]")
{
    // The pre-version format answered a miss with a bare 0x00 and no length,
    // which is why an error and a miss could not be told apart and why no reply
    // could be drained without knowing which command produced it.
    auto const reply = EncodeReply(Status::Miss, {});
    CHECK(reply == Bytes({ 0x00, 0x00, 0x00, 0x00, 0x00 }));
    CHECK(reply.size() == ReplyHeaderSize);
}

// --- header round-trips ----------------------------------------------------

TEST_CASE("DecodeRequestHeader reads back what EncodeFetch wrote", "[wire]")
{
    auto const frame = EncodeFetch("ab");
    auto const header = DecodeRequestHeader(std::span<std::byte const> { frame }.first(RequestHeaderSize));

    REQUIRE(header.has_value());
    CHECK(Unwrap(header).version == CurrentVersion);
    CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Op::Fetch));
    CHECK(Unwrap(header).payloadLength == 6);
}

TEST_CASE("DecodeRequestHeader rejects a foreign magic but keeps an unknown opcode", "[wire]")
{
    auto frame = EncodeFetch("ab");

    SECTION("a wrong magic is not this protocol at all")
    {
        frame[0] = std::byte { 0x80 };
        CHECK_FALSE(DecodeRequestHeader(std::span<std::byte const> { frame }.first(RequestHeaderSize)).has_value());
    }

    SECTION("an unknown opcode decodes, so the caller can answer and resynchronize")
    {
        // Deliberately NOT a decode failure: the whole point of the declared
        // payload length is that an unrecognised verb can be skipped and
        // answered rather than dropping the connection.
        frame[2] = std::byte { 0xEE };
        auto const header = DecodeRequestHeader(std::span<std::byte const> { frame }.first(RequestHeaderSize));
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).opRaw == 0xEE);
        CHECK(FindOp(Unwrap(header).opRaw) == nullptr);
    }

    SECTION("a short header is rejected")
    {
        CHECK_FALSE(DecodeRequestHeader(std::span<std::byte const> { frame }.first(RequestHeaderSize - 1)).has_value());
    }
}

TEST_CASE("DecodeReplyHeader round-trips and rejects an unknown status", "[wire]")
{
    auto const payload = Bytes({ 0x01, 0x02, 0x03 });
    auto reply = EncodeReply(Status::Ok, payload);

    auto const header = DecodeReplyHeader(std::span<std::byte const> { reply }.first(ReplyHeaderSize));
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Status::Ok);
    CHECK(Unwrap(header).payloadLength == 3);

    reply[0] = std::byte { 0x7F };
    CHECK_FALSE(DecodeReplyHeader(std::span<std::byte const> { reply }.first(ReplyHeaderSize)).has_value());
}

TEST_CASE("EncodeProgressReply emits the specified bytes exactly", "[wire]")
{
    // clang-format off: the grid IS the specification -- one wire field per row.
    auto const expected = Bytes({
        0x03,                   // status = Progress
        0x00, 0x00, 0x00, 0x00, // payloadLength = 0
    });
    // clang-format on

    CHECK(EncodeProgressReply() == expected);

    // The EMPTY payload is the contract, not an argument somebody chose (#245): a
    // liveness pulse that carried partial diagnostics would be a second, racy channel
    // for output the result frame already carries whole. Asserted here as well as
    // enforced by the encoder taking no argument, because the assertion is what states
    // it and the signature is only what makes it hard to break.
    auto const header = DecodeReplyHeader(EncodeProgressReply());
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).payloadLength == 0);
}

TEST_CASE("A progress frame is a known status and is never a terminal one", "[wire]")
{
    // Two facts a client reads separately, and collapsing them is the whole defect a
    // pre-#245 launcher has: `DecodeReplyHeader` refused `0x03` outright, so a
    // progress frame arrived as a decode failure and the compile was abandoned several
    // minutes in, as a transport failure naming nothing.
    auto const pulse = EncodeProgressReply();
    auto const header = DecodeReplyHeader(pulse);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Status::Progress);
    CHECK_FALSE(IsTerminalStatus(Unwrap(header).status));

    CHECK(IsTerminalStatus(Status::Ok));
    CHECK(IsTerminalStatus(Status::Miss));
    CHECK(IsTerminalStatus(Status::Error));

    // And the byte after it is still unknown, so the enum is a closed set rather than
    // "anything small is a status".
    CHECK(IsKnownStatus(0x03));
    CHECK(IsKnownStatus(0x04));
    CHECK_FALSE(IsKnownStatus(0x05));
}

TEST_CASE("Only COMPILE may be answered with a progress pulse", "[wire]")
{
    // A pulse turns one verb's reply into a STREAM, and every client reading that verb
    // then has to loop for its answer. That is a decision per verb, so the table says
    // which -- and `ProgressIsCompileOnly` asserts it at compile time. This is the
    // runtime half, which is what fails when a row's mask is widened by hand.
    for (auto const& row: OpTable)
    {
        INFO("verb " << row.name);
        auto const pulses = (row.legalStatuses & StatusBit(Status::Progress)) != 0;
        CHECK(pulses == (row.code == Op::Compile));
    }
}

TEST_CASE("The pulse cadence and the client's patience are one pair of numbers", "[wire]")
{
    // They bound ONE silence from opposite sides: the worker's cadence is how often it
    // says something, the client's bound is how long it waits without hearing. Neither
    // end can pick its own without describing a fleet the other is not running, which
    // is why both live here -- the only header both binaries include -- exactly as
    // `DefaultCompileLeaseTimeout` does.
    //
    // The RELATION is the load-bearing part: a client that gives up on the first missed
    // pulse refuses a perfectly healthy worker over the ordinary jitter of its own
    // reactor, which reads as a fleet that has stopped working.
    CHECK(DefaultCompileIdleTimeout >= 3 * DefaultProgressInterval);
    CHECK(DefaultCompileIdleTimeout < DefaultCompileLeaseTimeout);
    CHECK(DefaultProgressInterval > std::chrono::milliseconds::zero());
}

// --- payload splitting -----------------------------------------------------

TEST_CASE("DecodeStorePayload round-trips every field, including an empty one", "[wire]")
{
    // An empty prefetch group is the routine case, not an edge one: the launcher stores
    // with no prefetch group whenever grouping is off, and the handler branches on it.
    auto const value = Bytes({ 0xDE, 0xAD, 0xBE, 0xEF });
    auto const frame = EncodeStore(StoreRequest { .key = "the-key",
                                                  .prefetchGroup = "",
                                                  .srcRoot = "/src",
                                                  .buildTree = "/build",
                                                  .value = std::span<std::byte const> { value } });

    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const view = DecodeStorePayload(payload);

    REQUIRE(view.has_value());
    CHECK(AsStringView(Unwrap(view).key) == "the-key");
    CHECK(Unwrap(view).prefetchGroup.empty());
    CHECK(AsStringView(Unwrap(view).srcRoot) == "/src");
    CHECK(AsStringView(Unwrap(view).buildTree) == "/build");
    CHECK(std::ranges::equal(Unwrap(view).value, value));
}

TEST_CASE("EncodeCacheDrop emits the specified bytes exactly", "[wire][cache-drop]")
{
    auto const frame = EncodeCacheDrop("ab");

    // clang-format off: the grid IS the specification -- one wire field per row.
    auto const expected = Bytes({
        0xFC,                   // magic
        0x0E,                   // version
        0x15,                   // op = CacheDrop
        0x00, 0x00, 0x00, 0x06, // payloadLength = 6
        0x00, 0x00, 0x00, 0x02, // field[0] length = 2
        0x61, 0x62,             // "ab"
    });
    // clang-format on

    CHECK(frame == expected);
}

TEST_CASE("DecodeCacheDropPayload round-trips the key and refuses a second field", "[wire][cache-drop]")
{
    auto const frame = EncodeCacheDrop("the-key");
    auto const key = DecodeCacheDropPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(key.has_value());
    CHECK(AsStringView(Unwrap(key)) == "the-key");

    // A STORE-shaped payload is five fields, not one: refused rather than read as its key.
    auto const value = Bytes({ 0xAA });
    auto const store = EncodeStore(StoreRequest {
        .key = "k", .prefetchGroup = "", .srcRoot = "s", .buildTree = "b", .value = std::span<std::byte const> { value } });
    CHECK_FALSE(DecodeCacheDropPayload(std::span<std::byte const> { store }.subspan(RequestHeaderSize)).has_value());
}

TEST_CASE("A cache drop is a Cache verb that may miss, needs the credential, and is bounded", "[wire][cache-drop]")
{
    // Each column decides something a surface does with the verb, so each is asserted
    // rather than left to follow from the row. `Cache` routes it to the tier and its
    // locality gate; `Miss` is the answer a repair's second run gets; `RequiresAuth` is the
    // gate a daemon's writes get; and the bound is what a key-only verb may cost.
    auto const* const row = FindOp(static_cast<std::uint8_t>(Op::CacheDrop));
    REQUIRE(row != nullptr);
    CHECK(row->name == "cache-drop");
    CHECK(row->fieldCount == 1);
    CHECK(row->family == VerbFamily::Cache);
    CHECK(IsLegalStatus(Op::CacheDrop, Status::Miss));
    CHECK_FALSE(IsLegalStatus(Op::CacheDrop, Status::Progress));
    CHECK_FALSE(IsLegalStatus(Op::CacheDrop, Status::Push));
    CHECK_FALSE(row->preAuth.Allowed());
    CHECK(row->maxPayload.IsBounded());
    CHECK(OpPayloadCap(static_cast<std::uint8_t>(Op::CacheDrop), 256U * 1024U * 1024U) == MaxControlPayload);
}

TEST_CASE("DecodeFetchPayload round-trips the key", "[wire]")
{
    auto const frame = EncodeFetch("the-key");
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const key = DecodeFetchPayload(payload);

    REQUIRE(key.has_value());
    CHECK(AsStringView(Unwrap(key)) == "the-key");
}

TEST_CASE("SplitFields rejects a payload that disagrees with its field lengths", "[wire]")
{
    // The declared total and the per-field lengths are redundant by design.
    // Disagreement must be a typed rejection, never a silent reinterpretation.
    //
    // Each malformed payload is written out literally rather than derived by
    // mutating an encoder's output: the point of the test is a specific broken
    // byte sequence, and spelling it makes the case self-evident instead of
    // something the reader has to reconstruct.

    SECTION("truncated before the length prefix")
    {
        CHECK_FALSE(SplitFields(Bytes({ 0x00, 0x00 }), 1).has_value());
    }

    SECTION("a field length that overruns the payload")
    {
        // Declares two bytes, supplies one.
        CHECK_FALSE(SplitFields(Bytes({ 0x00, 0x00, 0x00, 0x02, 0x61 }), 1).has_value());
    }

    SECTION("trailing bytes after the last field")
    {
        // One well-formed 2-byte field, then a byte nothing accounts for.
        CHECK_FALSE(SplitFields(Bytes({ 0x00, 0x00, 0x00, 0x02, 0x61, 0x62, 0x00 }), 1).has_value());
    }

    SECTION("a field count the payload cannot satisfy")
    {
        // One field present, two demanded.
        CHECK_FALSE(SplitFields(Bytes({ 0x00, 0x00, 0x00, 0x02, 0x61, 0x62 }), 2).has_value());
    }

    SECTION("an exactly-filling payload is accepted")
    {
        // The positive control: without it the sections above could pass for the
        // wrong reason.
        //
        // The payload is a named local, not a temporary, because SplitFields
        // returns spans INTO it — handing it a temporary leaves every field
        // dangling the moment the call returns.
        auto const payload = Bytes({ 0x00, 0x00, 0x00, 0x02, 0x61, 0x62 });
        auto const fields = Unwrap(SplitFields(payload, 1));
        REQUIRE(fields.size() == 1);
        // .at() rather than operator[]: bounds-checked, so it is safe to the
        // reader and provably so to the optimizer, which cannot carry the
        // REQUIRE above through Catch2's macro.
        CHECK(AsStringView(fields.at(0)) == "ab");
    }
}

TEST_CASE("DecodeErrorPayload splits the code from the message", "[wire]")
{
    auto const reply = EncodeErrorReply(ErrorCode::PayloadTooLarge, "too big");
    auto const payload = std::span<std::byte const> { reply }.subspan(ReplyHeaderSize);
    auto const decoded = DecodeErrorPayload(payload);

    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).first == ErrorCode::PayloadTooLarge);
    CHECK(Unwrap(decoded).second == "too big");

    CHECK_FALSE(DecodeErrorPayload({}).has_value());
}

TEST_CASE("EncodeErrorReply falls back to the table's default message", "[wire]")
{
    auto const reply = EncodeErrorReply(ErrorCode::StorageWriteFailed);
    auto const decoded = DecodeErrorPayload(std::span<std::byte const> { reply }.subspan(ReplyHeaderSize));

    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).first == ErrorCode::StorageWriteFailed);
    CHECK(Unwrap(decoded).second == "storage write failed");
}

// --- table integrity -------------------------------------------------------

TEST_CASE("Every op descriptor is unique and well-formed", "[wire]")
{
    for (auto const& row: OpTable)
    {
        CHECK_FALSE(row.name.empty());
        // A zero count is legitimate only for a verb that asks nothing, and the
        // header `static_assert`s the two tables against each other -- so this is
        // the same rule stated where a reader of the table integrity case will
        // look for it.
        CHECK((row.fieldCount > 0 || CarriesNoFields(row.code)));
        CHECK(row.legalStatuses != 0);
        CHECK(FindOp(static_cast<std::uint8_t>(row.code)) == &row);
    }

    for (auto const& a: OpTable)
    {
        auto const duplicates = std::ranges::count_if(OpTable, [&](auto const& b) { return b.code == a.code; });
        CHECK(duplicates == 1);
    }
}

TEST_CASE("Every error descriptor is unique and carries a message", "[wire]")
{
    for (auto const& row: ErrorTable)
    {
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.defaultMessage.empty());
        CHECK(Describe(row.code) == &row);

        auto const duplicates = std::ranges::count_if(ErrorTable, [&](auto const& other) { return other.code == row.code; });
        CHECK(duplicates == 1);
    }
}

TEST_CASE("A FETCH may miss but a STORE may not", "[wire]")
{
    // Encoded as table data rather than convention, so the asymmetry is
    // assertable instead of merely intended.
    CHECK(IsLegalStatus(Op::Fetch, Status::Miss));
    CHECK_FALSE(IsLegalStatus(Op::Store, Status::Miss));

    for (auto const& row: OpTable)
    {
        CHECK(IsLegalStatus(row.code, Status::Ok));
        CHECK(IsLegalStatus(row.code, Status::Error));
    }
}

TEST_CASE("IsSupported admits exactly the declared range", "[wire]")
{
    CHECK(IsSupported(CurrentVersion));
    CHECK(IsSupported(MinSupportedVersion));
    CHECK_FALSE(IsSupported(static_cast<WireVersion>(CurrentVersion + 1)));
    CHECK_FALSE(IsSupported(0));
}

// --- AUTH ------------------------------------------------------------------

TEST_CASE("EncodeAuth emits the specified bytes exactly", "[wire]")
{
    auto const frame = EncodeAuth(AuthRequest { .username = "bob", .secret = "hunter2" });

    auto const expected = Bytes({
        0xFC, 0x0E, 0x03,             // magic, version, op=Auth
        0x00, 0x00, 0x00, 0x17,       // payload length: (4+1) + (4+3) + (4+7) = 23
        0x00, 0x00, 0x00, 0x01, 0x01, // kind = Password
        0x00, 0x00, 0x00, 0x03, 'b',  'o', 'b', 0x00, 0x00, 0x00, 0x07, 'h', 'u', 'n', 't', 'e', 'r', '2',
    });
    CHECK(frame == expected);
}

TEST_CASE("DecodeAuthPayload round-trips, including the empty-username form", "[wire]")
{
    // The empty username is the redis `requirepass` spelling and is a legitimate
    // credential, not a malformed one: a launcher configured with only a token
    // sends exactly this. A decoder that rejected it would lock out the common case.
    auto const frame = EncodeAuth(AuthRequest { .username = "", .secret = "s3cret" });
    std::span<std::byte const> const payload = std::span { frame }.subspan(RequestHeaderSize);

    auto const decoded = DecodeAuthPayload(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).kind == AuthKind::Password);
    CHECK(Unwrap(decoded).username.empty());
    CHECK(AsStringView(Unwrap(decoded).secret) == "s3cret");
}

TEST_CASE("DecodeAuthPayload rejects a payload with the wrong field count", "[wire]")
{
    // A FETCH payload is one field; AUTH demands three. Decoding one as the other
    // must fail rather than silently read the key as a username with no secret.
    auto const fetch = EncodeFetch("some-key");
    std::span<std::byte const> const payload = std::span { fetch }.subspan(RequestHeaderSize);
    CHECK_FALSE(DecodeAuthPayload(payload).has_value());
}

TEST_CASE("The credential kinds keep the bytes they were assigned", "[wire][auth][ticket]")
{
    CHECK(static_cast<std::uint8_t>(AuthKind::Password) == 0x01);
    CHECK(static_cast<std::uint8_t>(AuthKind::MachineTicket) == 0x02);
    CHECK(static_cast<std::uint8_t>(ErrorCode::TicketRefused) == 0x2D);
    auto const* const described = Describe(ErrorCode::TicketRefused);
    REQUIRE(described != nullptr);
    CHECK(described->name == "ticket-refused");
    CHECK(OpFieldCount(Op::Auth) == 3);
}

TEST_CASE("An operator's control verb is refused an unidentified caller under its own byte", "[wire][admission]")
{
    // The byte and the name, both: a symbol both ends spell tests only the first.
    CHECK(static_cast<std::uint8_t>(ErrorCode::IdentifiedCallerRequired) == 0x2E);
    auto const* const described = Describe(ErrorCode::IdentifiedCallerRequired);
    REQUIRE(described != nullptr);
    CHECK(described->name == "identified-caller-required");

    // The column, asked of the rows one at a time so a failure names the verb; the set itself is
    // `ControlVerbsNeedAnIdentifiedCaller`'s, a build failure.
    for (auto const op: { Op::ClusterSet,
                          Op::ClusterForget,
                          Op::ClusterAdmit,
                          Op::ClusterAdmitLearner,
                          Op::ClusterAdmitWorker,
                          Op::EnrollControl })
    {
        auto const* const row = FindOp(static_cast<std::uint8_t>(op));
        REQUIRE(row != nullptr);
        INFO(row->name);
        CHECK(row->identity == IdentityRequirement::IdentifiedCaller);
    }
    // And the verbs a client sends are not: a launcher on an open node leases and reads as before.
    for (auto const op: { Op::Lease, Op::Release, Op::ClusterStatus, Op::NodeStatus, Op::Fetch, Op::Enroll })
    {
        auto const* const row = FindOp(static_cast<std::uint8_t>(op));
        REQUIRE(row != nullptr);
        INFO(row->name);
        CHECK(row->identity == IdentityRequirement::AddressAdmits);
    }
}

TEST_CASE("A machine ticket travels in AUTH with its kind, and the kind is read back", "[wire][auth][ticket]")
{
    auto const ticket = std::string { "\x00\x01\x02opaque", 9 };
    auto const frame = EncodeAuth(AuthRequest { .kind = AuthKind::MachineTicket, .username = {}, .secret = ticket });
    auto const decoded = DecodeAuthPayload(std::span { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).kind == AuthKind::MachineTicket);
    CHECK(Unwrap(decoded).username.empty());
    CHECK(AsStringView(Unwrap(decoded).secret) == ticket);
}

TEST_CASE("The largest ticket a decoder reads fits AUTH's pre-auth ceiling", "[wire][auth][ticket]")
{
    // AUTH is served before anything is proved, so its payload ceiling is fixed and small. A
    // ticket the node would accept but the frame ceiling refused would read as a malformed frame
    // at every worker, whatever the ticket said.
    auto const largest = std::string(Distributed::MaxMachineTicketBytes, 't');
    auto const frame = EncodeAuth(AuthRequest { .kind = AuthKind::MachineTicket, .username = {}, .secret = largest });
    CHECK(frame.size() - RequestHeaderSize <= MaxAuthPayload);
}

TEST_CASE("An AUTH whose kind this build does not know, or a ticket with a username, is malformed", "[wire][auth][ticket]")
{
    auto rawAuth = [](std::byte kind, std::string_view username, std::string_view secret) {
        auto const kindField = std::array { kind };
        return WireFields::Encode({ std::span<std::byte const> { kindField }, AsBytes(username), AsBytes(secret) });
    };
    CHECK_FALSE(DecodeAuthPayload(rawAuth(std::byte { 0x00 }, "", "s")).has_value());
    CHECK_FALSE(DecodeAuthPayload(rawAuth(std::byte { 0x03 }, "", "s")).has_value());
    CHECK_FALSE(DecodeAuthPayload(rawAuth(std::byte { 0x02 }, "bob", "ticket")).has_value());
    // A two-field payload -- the layout before the kind was carried -- is refused rather than read
    // as a kind.
    CHECK_FALSE(DecodeAuthPayload(WireFields::Encode({ AsBytes("bob"), AsBytes("s3cret") })).has_value());
    CHECK(DecodeAuthPayload(rawAuth(std::byte { 0x01 }, "bob", "s3cret")).has_value()); // the control
}

TEST_CASE("Exactly the verbs meant to be reachable before AUTH are reachable", "[wire]")
{
    // The whole point of `preAuth` being a table column is that this list is
    // assertable. If a future verb is added with `preAuth = true`, this case is
    // what forces that to be a deliberate, reviewed decision rather than a
    // default nobody looked at.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::Auth)));
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::Fetch)));
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::Store)));

    // `Op::Enroll` is the SECOND pre-auth verb this wire has ever had, and this case
    // firing is what made that a reviewed decision rather than a column nobody read: a
    // joiner has no credential by construction -- the key is what it is asking for --
    // so the verb that asks for one cannot require the thing it is asking for. Its
    // decision half, `Op::EnrollControl`, stays behind AUTH and is asserted so beside
    // the family's own cases, because opening a family rather than a verb is the one
    // mistake that split exists to prevent.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::Enroll)));
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::EnrollControl)));

    // `Op::FleetSummary` is the third. A seed is asked which fleet it is in by a machine that is
    // nobody's member yet, so it holds no credential of that fleet to present; and the answer is
    // public -- what a beacon already states -- and signed over the asker's nonce, so opening it
    // hands a stranger nothing it could not read off the network.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::FleetSummary)));

    // `Op::ExplainAdmission` is the FOURTH, and the reviewed decision is this: its SELF form must
    // reach a caller the node refuses, or it could never report the refusal. It answers only about
    // the caller's own connection -- "refused, by no route" is what every gated verb already tells
    // a stranger -- while its MACHINE form, which reads the roster, stays gated by membership in
    // the node. Bounded by its own ceiling, as `PreAuthVerbsAreBounded` requires of every one.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::ExplainAdmission)));

    // The COUNT lives here and nowhere else, so a fifth verb arriving reddens exactly
    // one case rather than being argued about in two.
    auto const openVerbs = std::ranges::count_if(OpTable, [](auto const& row) { return row.preAuth.Allowed(); });
    CHECK(openVerbs == 4);
}

TEST_CASE("An unknown opcode is never reachable before AUTH", "[wire]")
{
    // The gate has to fail CLOSED for a byte it does not recognise. A predicate
    // resolving the descriptor first and treating "no row" as permissive would
    // hand every future or bogus verb a free pass past authentication.
    CHECK_FALSE(IsPreAuthAllowed(0x00));
    CHECK_FALSE(IsPreAuthAllowed(0xFF));
    for (auto const raw: std::views::iota(0, 256))
    {
        auto const opRaw = static_cast<std::uint8_t>(raw);
        if (FindOp(opRaw) == nullptr)
            CHECK_FALSE(IsPreAuthAllowed(opRaw));
    }
}

// --- distributed execution ---------------------------------------------------

TEST_CASE("Every dispatch verb round-trips its fields", "[wire]")
{
    SECTION("REGISTER")
    {
        // Every field a different value, for the reason `RaftWire`'s exemplars are:
        // two fields sharing one lets a transposition through, and the capacity
        // record's four members are exactly the shape a transposition hides in.
        auto const frame = EncodeRegister(RegisterRequest {
            .fingerprint = "gcc-13-abc",
            .endpoint = "10.0.0.1:6676",
            .slots = 8,
            .acceptedCodecs = { 2, 1 },
            .capacity = CapacityFields {
                .logicalCores = 24, .totalMemoryBytes = 137438953472, .nodeClassRaw = 1, .reservedCores = 5 } });
        auto const decoded = DecodeRegisterPayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).fingerprint) == "gcc-13-abc");
        CHECK(AsStringView(Unwrap(decoded).endpoint) == "10.0.0.1:6676");
        CHECK(Unwrap(decoded).slots == 8);
        CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 2, 1 });
        CHECK(Unwrap(decoded).capacity.logicalCores == 24);
        CHECK(Unwrap(decoded).capacity.totalMemoryBytes == 137438953472);
        CHECK(Unwrap(decoded).capacity.nodeClassRaw == 1);
        CHECK(Unwrap(decoded).capacity.reservedCores == 5U);
    }
    SECTION("HEARTBEAT")
    {
        // Every field a distinct value again, and the three inside the load record
        // are of two different widths -- which is where a transposition here would
        // land, since swapping the two u64s is the one mistake that still decodes.
        auto const frame = EncodeHeartbeat("w7",
                                           3,
                                           LoadFields { .cpuBusyPermille = 640,
                                                        .availableMemoryBytes = 8589934592,
                                                        .freeScratchBytes = 274877906944,
                                                        .history = {} });
        auto const decoded = DecodeHeartbeatPayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).workerId) == "w7");
        CHECK(Unwrap(decoded).inFlight == 3);
        CHECK(Unwrap(decoded).load.cpuBusyPermille == 640U);
        CHECK(Unwrap(decoded).load.availableMemoryBytes == 8589934592ULL);
        CHECK(Unwrap(decoded).load.freeScratchBytes == 274877906944ULL);
    }
    SECTION("LEASE")
    {
        auto const frame =
            EncodeLease(LeaseRequest { .fingerprint = "gcc-13-abc", .key = "objkey", .acceptedCodecs = { 1 } });
        auto const decoded = DecodeLeasePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).fingerprint) == "gcc-13-abc");
        CHECK(AsStringView(Unwrap(decoded).key) == "objkey");
    }
    SECTION("RELEASE")
    {
        auto const frame = EncodeRelease(ReleaseRequest { .leaseToken = "l42", .key = "objkey" });
        auto const decoded = DecodeReleasePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).leaseToken) == "l42");
        // The key is what makes a release the CALLER's: a token alone is a number a
        // restarted scheduler will have reissued.
        CHECK(AsStringView(Unwrap(decoded).key) == "objkey");
    }
    SECTION("COMPILE")
    {
        auto const args = Bytes({ 0x01, 0x02 });
        auto const source = Bytes({ 0xAA, 0xBB, 0xCC });
        auto const frame = EncodeCompile(CompileRequest { .leaseToken = "l1",
                                                          .fingerprint = "gcc-13-abc",
                                                          .args = args,
                                                          .source = source,
                                                          .acceptedCodecs = { 1 },
                                                          .sourceName = "Widget.cpp",
                                                          .compileDir = "/home/ci/build",
                                                          .compileDirReplacement = "./sub",
                                                          .sourceRoot = {},
                                                          .sourceRootReplacement = {} });
        auto const decoded = DecodeCompilePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).leaseToken) == "l1");
        CHECK(std::ranges::equal(Unwrap(decoded).args, args));
        CHECK(std::ranges::equal(Unwrap(decoded).source, source));
        CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 1 });
        CHECK(AsStringView(Unwrap(decoded).sourceName) == "Widget.cpp");
        // The compilation-directory pair, which is what lets a dispatched object
        // record the directory the client's own compile records (#506).
        CHECK(AsStringView(Unwrap(decoded).compileDir) == "/home/ci/build");
        CHECK(AsStringView(Unwrap(decoded).compileDirReplacement) == "./sub");
    }

    SECTION("COMPILE, from a client that maps nothing")
    {
        // Empty is a VALUE here rather than an absent field: it says the client asked
        // for no mapping, which is what stops the worker inventing one. `SplitFields`
        // is exact, so this also pins that an empty last field still makes seven.
        auto const frame = EncodeCompile(CompileRequest { .leaseToken = "l1",
                                                          .fingerprint = "gcc-13-abc",
                                                          .args = {},
                                                          .source = {},
                                                          .acceptedCodecs = { 1 },
                                                          .sourceName = "Widget.cpp",
                                                          .compileDir = {},
                                                          .compileDirReplacement = {},
                                                          .sourceRoot = {},
                                                          .sourceRootReplacement = {} });
        auto const decoded = DecodeCompilePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).compileDir.empty());
        CHECK(Unwrap(decoded).compileDirReplacement.empty());
        CHECK(AsStringView(Unwrap(decoded).sourceName) == "Widget.cpp");
    }

    SECTION("COMPILE carries a PATH in sourceName, not a base name")
    {
        // Every other case here sends `Widget.cpp`, which is what this field used to
        // carry. #800 made it the client's source path put through the client's own
        // `-fdebug-prefix-map` rules -- clang takes `DW_AT_name` from the input file
        // path, so without it a dispatched object records the worker's scratch
        // directory -- and the header went on saying "the base name only" for months
        // afterwards ([#907](https://github.com/LASTRADA-Software/fastcached/issues/907)).
        // So the wire test was exercising a value shape the wire no longer carries.
        //
        // **The `!=` arm is the assertion, not decoration.** A round trip alone passes
        // just as well on a base name, so it asserts what both readings produce. What
        // DISCRIMINATES is that nothing between the encoder and the decoder reduces this
        // to a component -- which is exactly the "fix" the stale contract invited, and
        // the one that would silently undo #800 while every on-disk file-name test
        // stayed green.
        //
        // The value is deliberately awkward in the three ways a real one is: separators,
        // a space, and a Windows drive colon. `SafeSourceName` would cut all of it back
        // to `Widget.cpp` -- that is correct for naming the scratch FILE and wrong for
        // the prefix-map rule, which is the split the two halves of this field have.
        constexpr auto MappedSource = std::string_view { "C:/src/my project/sub dir/Widget.cpp" };
        auto const frame = EncodeCompile(CompileRequest { .leaseToken = "l1",
                                                          .fingerprint = "gcc-13-abc",
                                                          .args = {},
                                                          .source = {},
                                                          .acceptedCodecs = { 1 },
                                                          .sourceName = MappedSource,
                                                          .compileDir = "/home/ci/build",
                                                          .compileDirReplacement = ".",
                                                          .sourceRoot = "/home/ci/src",
                                                          .sourceRootReplacement = "." });
        auto const decoded = DecodeCompilePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).sourceName) == MappedSource);
        CHECK(AsStringView(Unwrap(decoded).sourceName) != "Widget.cpp");
        // The pair that only exists because gcc reads the name from the `#line` marker
        // instead (#883), and which travels BESIDE this rather than inside it.
        CHECK(AsStringView(Unwrap(decoded).sourceRoot) == "/home/ci/src");
        CHECK(AsStringView(Unwrap(decoded).sourceRootReplacement) == ".");
    }
}

TEST_CASE("A capacity record tolerates a peer that says less, or more", "[wire]")
{
    // The whole reason it is NESTED rather than four more REGISTER fields.
    // `SplitFields` is exact by design, so a fact added at the top level would move
    // REGISTER's arity and make two builds of one fleet unable to speak at all.
    // Inside a field, a shorter record keeps this build's defaults and a longer one
    // is read as far as this build understands it.
    SECTION("a record from a peer that knew fewer facts")
    {
        // Cores and memory only: what a build predating the class byte would send.
        auto const cores = WireFields::ToBigEndian<std::uint32_t>(12U);
        auto const memory = WireFields::ToBigEndian<std::uint64_t>(17179869184ULL);
        auto const shortened =
            WireFields::Encode({ std::span<std::byte const> { cores }, std::span<std::byte const> { memory } });

        auto const decoded = DecodeCapacity(shortened);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).logicalCores == 12);
        CHECK(Unwrap(decoded).totalMemoryBytes == 17179869184ULL);
        CHECK(Unwrap(decoded).nodeClassRaw == 0);
        // Absent, not zero -- so the receiver applies the class reserve rather than
        // concluding the operator asked for none.
        CHECK_FALSE(Unwrap(decoded).reservedCores.has_value());
    }
    SECTION("a record from a peer that knew more")
    {
        auto const full = EncodeCapacity(CapacityFields {
            .logicalCores = 12, .totalMemoryBytes = 17179869184ULL, .nodeClassRaw = 1, .reservedCores = 3 });
        auto const surplus = WireFields::ToBigEndian<std::uint64_t>(99U);
        auto extended = full;
        auto const tail = WireFields::Encode({ std::span<std::byte const> { surplus } });
        extended.insert(extended.end(), tail.begin(), tail.end());

        auto const decoded = DecodeCapacity(extended);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).logicalCores == 12);
        CHECK(Unwrap(decoded).nodeClassRaw == 1);
        CHECK(Unwrap(decoded).reservedCores == 3U);
    }
    SECTION("no record at all")
    {
        // Answered rather than refused: an empty field is a peer with nothing to say,
        // and the defaults it lands on are the "did not say" the fields document.
        auto const decoded = DecodeCapacity({});
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).logicalCores == 0);
        CHECK_FALSE(Unwrap(decoded).reservedCores.has_value());
    }
    SECTION("a field of the wrong width is refused")
    {
        // The one thing tolerance must not extend to. Reading the first four bytes of
        // a five-byte core count would invent the number that decides this machine's
        // share of the fleet.
        auto const bad = WireFields::Encode({ Bytes({ 0x00, 0x00, 0x01 }) });
        CHECK_FALSE(DecodeCapacity(bad).has_value());
    }
    SECTION("a reserve of zero survives the round trip as a reserve of zero")
    {
        // The distinction the optional exists for: "drive this machine to its last
        // core" must not arrive as "I did not mention a reserve".
        auto const encoded = EncodeCapacity(
            CapacityFields { .logicalCores = 0, .totalMemoryBytes = 0, .nodeClassRaw = 0, .reservedCores = 0 });
        auto const decoded = DecodeCapacity(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(Unwrap(decoded).reservedCores.has_value());
        CHECK(Unwrap(Unwrap(decoded).reservedCores) == 0U);
    }
}

TEST_CASE("A load record tells silence apart from a measured zero", "[wire]")
{
    // The distinction the whole record is optional for, and it runs both ways: a
    // machine that could not read its CPU must be scheduled on its other
    // properties, while one that read it and got zero is idle and should be given
    // work. A wire that flattened them would have to pick one, and both choices are
    // wrong for half the fleet.
    SECTION("a worker with nothing to report")
    {
        auto const frame = EncodeHeartbeat("w1", 0);
        auto const decoded = DecodeHeartbeatPayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK_FALSE(Unwrap(decoded).load.cpuBusyPermille.has_value());
        CHECK_FALSE(Unwrap(decoded).load.availableMemoryBytes.has_value());
        CHECK_FALSE(Unwrap(decoded).load.freeScratchBytes.has_value());
    }
    SECTION("a worker reporting a measured zero")
    {
        auto const frame = EncodeHeartbeat(
            "w1",
            0,
            LoadFields { .cpuBusyPermille = 0, .availableMemoryBytes = std::nullopt, .freeScratchBytes = 0, .history = {} });
        auto const decoded = DecodeHeartbeatPayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        REQUIRE(Unwrap(decoded).load.cpuBusyPermille.has_value());
        CHECK(Unwrap(Unwrap(decoded).load.cpuBusyPermille) == 0U);
        REQUIRE(Unwrap(decoded).load.freeScratchBytes.has_value());
        CHECK(Unwrap(Unwrap(decoded).load.freeScratchBytes) == 0ULL);
        // And the one it did not mention stays unmentioned.
        CHECK_FALSE(Unwrap(decoded).load.availableMemoryBytes.has_value());
    }
    SECTION("a field present at the wrong width is refused")
    {
        auto const bad = WireFields::Encode({ Bytes({ 0x00, 0x01 }) });
        CHECK_FALSE(DecodeLoad(bad).has_value());
    }
}

TEST_CASE("A u32 field of the wrong width is rejected, not read", "[wire]")
{
    // A short integer field is a sender speaking a shape this build does not know.
    // Reading the first four bytes of a longer one, or padding a shorter one, would
    // invent a value -- and `slots` deciding capacity or `inFlight` deciding load
    // are exactly the values that must not be invented.
    CHECK_FALSE(DecodeU32Field(Bytes({ 0x00, 0x00, 0x01 })).has_value());
    CHECK_FALSE(DecodeU32Field(Bytes({ 0x00, 0x00, 0x00, 0x00, 0x00 })).has_value());
    CHECK_FALSE(DecodeU32Field({}).has_value());
    CHECK(DecodeU32Field(Bytes({ 0x00, 0x00, 0x01, 0x00 })) == 256U);
}

TEST_CASE("A dispatch payload decoded as the wrong verb fails", "[wire]")
{
    // Each verb has its own arity, and SplitFields is strict in both directions, so
    // one verb's payload cannot be silently reinterpreted as another's.
    auto const lease = EncodeLease(LeaseRequest { .fingerprint = "f", .key = "k", .acceptedCodecs = {} });
    auto const payload = std::span<std::byte const> { lease }.subspan(RequestHeaderSize);
    CHECK_FALSE(DecodeRegisterPayload(payload).has_value());
    CHECK_FALSE(DecodeCompilePayload(payload).has_value());
    CHECK_FALSE(DecodeReleasePayload(payload).has_value());
    CHECK(DecodeLeasePayload(payload).has_value());

    // And the other way round: RELEASE carries two fields, so a LEASE, which carries
    // more, cannot be read out of it either.
    auto const release = EncodeRelease(ReleaseRequest { .leaseToken = "l1", .key = "k" });
    auto const releasePayload = std::span<std::byte const> { release }.subspan(RequestHeaderSize);
    CHECK_FALSE(DecodeLeasePayload(releasePayload).has_value());
    CHECK(DecodeReleasePayload(releasePayload).has_value());
}

TEST_CASE("A codec envelope round-trips its tag, raw size and bytes", "[wire]")
{
    auto const payload = Bytes({ 0xDE, 0xAD, 0xBE, 0xEF });
    auto const envelope = EncodeCodecEnvelope(/*codec=*/2, /*rawLength=*/9999, payload);

    auto const decoded = DecodeCodecEnvelope(envelope);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).codec == 2);
    // rawLength is the size BEFORE compression, and is what a receiver sizes its
    // output buffer from -- so it is deliberately not derivable from bytes.size().
    CHECK(Unwrap(decoded).rawLength == 9999);
    CHECK(std::ranges::equal(Unwrap(decoded).bytes, payload));
}

TEST_CASE("An envelope too short to hold a header is rejected", "[wire]")
{
    CHECK_FALSE(DecodeCodecEnvelope({}).has_value());
    CHECK_FALSE(DecodeCodecEnvelope(Bytes({ 0x00, 0x00, 0x00, 0x00 })).has_value());
    CHECK(DecodeCodecEnvelope(Bytes({ 0x00, 0x00, 0x00, 0x00, 0x00 })).has_value());
}

TEST_CASE("An empty payload still travels in a well-formed envelope", "[wire]")
{
    // The zero-length case is the one an encoder is most likely to get wrong, and a
    // failed compile legitimately produces an empty object.
    auto const envelope = EncodeCodecEnvelope(IdentityCodec, 0, {});
    auto const decoded = DecodeCodecEnvelope(envelope);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).codec == IdentityCodec);
    CHECK(Unwrap(decoded).rawLength == 0);
    CHECK(Unwrap(decoded).bytes.empty());
}

TEST_CASE("Codec negotiation prefers the sender's order and falls back to Identity", "[wire]")
{
    // The SENDER's order decides, because the sender has to decode the answer and
    // knows what is cheap for it.
    CHECK(ChooseCodec(/*accepted=*/ { 2, 1 }, /*available=*/ { 1, 2 }) == 2);
    CHECK(ChooseCodec(/*accepted=*/ { 1, 2 }, /*available=*/ { 1, 2 }) == 1);

    // Nothing in common falls back to Identity rather than refusing: an
    // uncompressed answer is always correct, and a build must never lose its cache
    // because two peers were compiled with different codec sets.
    CHECK(ChooseCodec(/*accepted=*/ { 7, 8 }, /*available=*/ { 1, 2 }) == IdentityCodec);
    CHECK(ChooseCodec(/*accepted=*/ {}, /*available=*/ { 1, 2 }) == IdentityCodec);
    CHECK(ChooseCodec(/*accepted=*/ { 2 }, /*available=*/ {}) == IdentityCodec);
}

TEST_CASE("A build with compression disabled still interoperates", "[wire]")
{
    // Such a build offers only Identity and can produce only Identity. Both
    // directions must still resolve, or enabling compression on one machine would
    // break the cache for every machine that has it compiled out.
    CodecList const none { IdentityCodec };
    CHECK(ChooseCodec(none, { 1, 2 }) == IdentityCodec);
    CHECK(ChooseCodec({ 2, 1 }, none) == IdentityCodec);
}

TEST_CASE("A decoder's result outlives the buffer it was decoded from", "[wire]")
{
    // #366, and #355's acceptance criterion for it: `Decode(Encode(x))` must not
    // compile, or must be SAFE. This takes the second branch -- the spelling is made
    // safe -- which is also what the rule concluded for `CapacityFields`.
    //
    // The first branch was available and was not taken. A deleted
    // `std::vector<std::byte>&&` overload would reject the temporary, and it can be
    // probed from a dependent context. It is not used because the ten `*View` types
    // in this header carry the identical trap, so guarding one alone would be
    // inconsistent, and it would reject three call sites in this file that use the
    // spelling safely inside a single full expression.
    //
    // **Written as the trap spelling on purpose.** The encoded vector is a temporary
    // that dies at the end of each full expression below, so every one of these
    // reads is a use-after-free if the decoded struct ever goes back to borrowing --
    // which the ASan leg then reports. Naming the payload in a local, as every
    // production call site does, would make the case pass either way and prove
    // nothing.
    //
    // `CodecEnvelopeView` is deliberately absent from this case. It BORROWS, says so
    // in its name, and is meant to: its production consumer reads it in scope, and
    // owning there would reinstate a full copy of a preprocessed translation unit on
    // the path a compression-less build takes for every payload. Asserting that it
    // outlives its buffer would be asserting the opposite of its design.
    //
    // **The payload size is load-bearing and must not be shrunk.** Measured, not
    // assumed: with a four-byte object this case passes under ASan even when the
    // decoder borrows -- the freed block is small enough that reading it back still
    // returns the right bytes, so the case would prove nothing. At 64 KiB the read
    // is reported immediately. Somebody tidying these into short literals would
    // disarm the test without changing a single assertion.
    static constexpr std::size_t detectableSize = 64 * 1024;

    SECTION("a COMPILE result")
    {
        std::vector<std::byte> const object(detectableSize, std::byte { 0xAB });
        auto const decoded = DecodeCompileResult(EncodeCompileResult(CompileResult { .exitCode = 3,
                                                                                     .object = object,
                                                                                     .stdoutText = AsBytes("out"),
                                                                                     .stderrText = AsBytes("err"),
                                                                                     .correlation = AsBytes("c0") }));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).exitCode == 3);
        CHECK(std::ranges::equal(Unwrap(decoded).object, object));
        CHECK(AsStringView(Unwrap(decoded).stdoutText) == "out");
        CHECK(AsStringView(Unwrap(decoded).stderrText) == "err");
        CHECK(AsStringView(Unwrap(decoded).correlation) == "c0");
    }

    SECTION("with the buffer explicitly scoped away")
    {
        // The second shape `SchedulerProtocol_test` uses for this same rule, and it
        // is not a duplicate of the sections above: a temporary dies at a semicolon,
        // which several things could hide, while this puts a scope boundary between
        // the decode and every read. Both are kept for the reason that file gives --
        // a rule this shape cannot be left to a case that happens to exercise it.
        std::vector<std::byte> const object(detectableSize, std::byte { 0xEF });
        std::optional<CompileResultFields> decoded;
        {
            auto const encoded = EncodeCompileResult(CompileResult { .exitCode = 7,
                                                                     .object = object,
                                                                     .stdoutText = AsBytes("gone"),
                                                                     .stderrText = {},
                                                                     .correlation = AsBytes("c1") });
            decoded = DecodeCompileResult(encoded);
        }
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).exitCode == 7);
        CHECK(std::ranges::equal(Unwrap(decoded).object, object));
        CHECK(AsStringView(Unwrap(decoded).stdoutText) == "gone");
        CHECK(Unwrap(decoded).stderrText.empty());
        CHECK(AsStringView(Unwrap(decoded).correlation) == "c1");
    }
}

TEST_CASE("A LEASE grant and a COMPILE result round-trip", "[wire]")
{
    SECTION("grant")
    {
        // The lifetime is deliberately NOT zero, and not the default either. A
        // round trip carrying zero passes against a build that dropped the field on
        // one side and default-constructed it on the other, which is the whole class
        // of defect a round-trip case exists to catch; one carrying the default passes
        // against a client that ignored the field and fell back to its constant.
        auto const payload = EncodeLeaseGrant(LeaseGrant { .endpoint = "10.0.0.2:6676",
                                                           .leaseToken = "l42",
                                                           .workerCodecs = { 2, 1 },
                                                           .lifetime = std::chrono::milliseconds { 2'400'000 } });
        auto const decoded = DecodeLeaseGrant(payload);
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).endpoint) == "10.0.0.2:6676");
        CHECK(AsStringView(Unwrap(decoded).leaseToken) == "l42");
        // Relayed from the worker's registration so the client can pick a codec for
        // the preprocessed payload without a negotiation round trip.
        CHECK(Unwrap(decoded).workerCodecs == CodecList { 2, 1 });
        // How long the fleet agreed this lease lives, in the clear beside the token
        // because the client holds no key and must bound its own wait (#522).
        CHECK(Unwrap(decoded).lifetime == std::chrono::milliseconds { 2'400'000 });
        CHECK(Unwrap(decoded).lifetime != DefaultCompileLeaseTimeout);
    }
    SECTION("result")
    {
        auto const object = Bytes({ 0x7F, 0x45, 0x4C, 0x46 });
        auto const payload = EncodeCompileResult(CompileResult { .exitCode = 0,
                                                                 .object = object,
                                                                 .stdoutText = AsBytes("out"),
                                                                 .stderrText = AsBytes("err"),
                                                                 .correlation = AsBytes("c0") });
        auto const decoded = DecodeCompileResult(payload);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).exitCode == 0);
        CHECK(std::ranges::equal(Unwrap(decoded).object, object));
        CHECK(AsStringView(Unwrap(decoded).stdoutText) == "out");
        CHECK(AsStringView(Unwrap(decoded).stderrText) == "err");
        // The correlation is a field like any other and must survive the round trip:
        // a client refuses a reply whose digest does not match, so one lost in
        // framing would refuse every honest compile (#280).
        CHECK(AsStringView(Unwrap(decoded).correlation) == "c0");
    }
    SECTION("a failed compile carries its diagnostics and no object")
    {
        auto const payload = EncodeCompileResult(CompileResult { .exitCode = 1,
                                                                 .object = {},
                                                                 .stdoutText = {},
                                                                 .stderrText = AsBytes("error: nope"),
                                                                 .correlation = AsBytes("c1") });
        auto const decoded = DecodeCompileResult(payload);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).exitCode == 1);
        CHECK(Unwrap(decoded).object.empty());
        CHECK(AsStringView(Unwrap(decoded).stderrText) == "error: nope");
    }
}

TEST_CASE("A WITHDRAW round-trips, and its byte is pinned", "[wire]")
{
    // The value as well as the name. A symbol both ends spell can only test the first,
    // and this end is the only one that can be recompiled: a launcher or node already
    // deployed tolerates `0x0D` and nobody here can rebuild it, so moving the
    // enumerator consistently would leave every in-tree test agreeing while every
    // deployed peer broke.
    CHECK(static_cast<std::uint8_t>(Op::Withdraw) == 0x0D);

    auto const frame = EncodeWithdraw("w-1");
    auto const header = DecodeRequestHeader(std::span<std::byte const> { frame }.first(RequestHeaderSize));
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Op::Withdraw));

    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    CHECK(payload.size() == Unwrap(header).payloadLength);

    auto const fields = DecodeWithdrawPayload(payload);
    REQUIRE(fields.has_value());
    CHECK(AsStringView(Unwrap(fields).workerId) == "w-1");

    // One field exactly. An empty payload cannot carry the id this verb IS, and is
    // refused rather than read as a withdrawal naming nothing.
    CHECK_FALSE(DecodeWithdrawPayload(std::span<std::byte const> {}).has_value());
}

TEST_CASE("No distributed verb is reachable before authentication", "[wire]")
{
    // Causing a compiler to run on another machine is the last thing an
    // unauthenticated peer should reach.
    for (auto const op: { Op::Register, Op::Heartbeat, Op::Withdraw, Op::Lease, Op::Compile })
    {
        INFO("op 0x" << static_cast<unsigned>(op));
        CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(op)));
    }
}

TEST_CASE("Every verb states which family it belongs to", "[wire]")
{
    // A grouping the `Op` enum has always carried as comment blocks, and which stopped
    // being enough the moment one listener served several families (#290): a merged
    // surface asks the OWNER of a verb who is admitted, whether a credential is
    // required, and which counter a refusal moves.
    //
    // The build already refuses a row that omits it -- `EveryVerbHasAFamily` is a
    // `static_assert` -- so what is left here is that the classification is the RIGHT
    // one. A row that said `Cache` for `LEASE` would compile, pass that assertion, and
    // route the fleet's capacity requests into the cache tier.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Store)) == VerbFamily::Cache);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Fetch)) == VerbFamily::Cache);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::CacheDrop)) == VerbFamily::Cache);

    // AUTH is nobody's verb family and its own: it establishes a credential for the
    // connection, and which component owns the credential is a routing decision the
    // surface makes rather than a fact about the verb.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Auth)) == VerbFamily::Session);

    for (auto const op: { Op::Register, Op::Heartbeat, Op::Withdraw, Op::Lease, Op::Release })
    {
        INFO("op 0x" << static_cast<unsigned>(op));
        CHECK(FamilyOf(static_cast<std::uint8_t>(op)) == VerbFamily::Scheduler);
    }
    for (auto const op: { Op::ClusterStatus, Op::ClusterSet, Op::ClusterForget, Op::ClusterAdmit })
    {
        INFO("op 0x" << static_cast<unsigned>(op));
        CHECK(FamilyOf(static_cast<std::uint8_t>(op)) == VerbFamily::Scheduler);
    }

    // COMPILE is its own family rather than the scheduler's, and the split is
    // load-bearing: it is the one verb that must not be served on a reactor, because
    // it spawns a process and blocks for seconds (#213).
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Compile)) == VerbFamily::Compile);

    // SUBSCRIBE is answered by a stream, which no request/reply component owns (#1399).
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Subscribe)) == VerbFamily::Live);
}

TEST_CASE("A byte that names no verb has no family", "[wire]")
{
    // Total over every byte value, like every other predicate reading the third header
    // byte: what arrives is a byte, not an `Op`, and a lookup that assumed otherwise
    // would push "is this even a verb" onto each call site separately.
    CHECK(FamilyOf(0x00) == VerbFamily::Unset);
    CHECK(FamilyOf(0xFF) == VerbFamily::Unset);

    for (auto const raw: std::views::iota(unsigned { 0 }, unsigned { 0xFF } + 1))
    {
        INFO("op 0x" << raw);
        auto const known = FindOp(static_cast<std::uint8_t>(raw)) != nullptr;
        CHECK(known == (FamilyOf(static_cast<std::uint8_t>(raw)) != VerbFamily::Unset));
    }
}

TEST_CASE("The scheduler's control verbs are bounded well below the session cap", "[wire]")
{
    // These are answered on a listener a whole fleet is meant to reach. A scheduler
    // that can be made to allocate the full payload cap per frame by anything that
    // authenticated once is a scheduler that stops scheduling.
    constexpr std::size_t SessionCap = 256U * 1024U * 1024U;
    for (auto const op: { Op::Register, Op::Heartbeat, Op::Lease })
    {
        INFO("op 0x" << static_cast<unsigned>(op));
        CHECK(OpPayloadCap(static_cast<std::uint8_t>(op), SessionCap) == MaxControlPayload);
    }
    // COMPILE is the deliberate exception: it carries a preprocessed translation
    // unit, so the operator's own cap is the only sensible bound.
    CHECK(OpPayloadCap(static_cast<std::uint8_t>(Op::Compile), SessionCap) == SessionCap);
}

TEST_CASE("A heartbeat carries closed history buckets and gets them back", "[wire][compile-cache][history]")
{
    using namespace FastCache::CompileCacheWire;

    LoadFields sent {};
    sent.cpuBusyPermille = 420;
    for (auto const index: std::views::iota(0, 3))
    {
        HistoryBucketFields bucket {};
        bucket.startMillis = 1'700'000'000'000 + (static_cast<std::uint64_t>(index) * 60'000);
        bucket.sampleMillis = bucket.startMillis + 137;
        bucket.values[5] = static_cast<std::uint64_t>(index) * 11; // cache hits
        bucket.values[8] = static_cast<std::uint64_t>(index) % 4;  // in flight
        sent.history.push_back(bucket);
    }

    auto const decoded = DecodeLoad(EncodeLoad(sent));
    REQUIRE(decoded.has_value());
    auto const& read = Unwrap(decoded);
    REQUIRE(read.history.size() == sent.history.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, sent.history.size()))
    {
        INFO("bucket " << index);
        CHECK(read.history[index].startMillis == sent.history[index].startMillis);
        // Carried rather than derived: a ring that folds sixty readings has no way to
        // recover when the last of them landed, and a rate divided by a nominal width
        // instead of the span observed understates by whatever was never sampled.
        CHECK(read.history[index].sampleMillis == sent.history[index].sampleMillis);
        CHECK(read.history[index].values == sent.history[index].values);
    }
    // The fields beside it are untouched: this is an addition to the nested record,
    // not a reshaping of it.
    CHECK(read.cpuBusyPermille == sent.cpuBusyPermille);
}

TEST_CASE("A history bucket tolerates a peer with fewer or more slots than this build", "[wire][compile-cache][history]")
{
    using namespace FastCache::CompileCacheWire;

    // The tolerance `HistorySlotCount`'s own doc comment claims, and the reason
    // growing `FleetMetric` needed no `CompileCacheWire` version move: a peer
    // carrying fewer slots is read with the rest defaulting to zero, and one
    // carrying more has the extra words ignored. Pinned here as a hand-packed
    // record rather than through `HistorySlotCount` itself, so a future width
    // change cannot silently narrow what this test actually exercises.
    auto const packWords = [](std::initializer_list<std::uint64_t> words) {
        std::vector<std::byte> packed;
        for (auto const word: words)
        {
            auto const be = WireFields::ToBigEndian<std::uint64_t>(word);
            packed.insert(packed.end(), be.begin(), be.end());
        }
        return packed;
    };
    auto const oneBucket = [&](std::vector<std::byte> const& packed) {
        auto const record =
            WireFields::Encode({ std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(1'700'000'000'000ULL) },
                                 std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(1'700'000'000'137ULL) },
                                 std::span<std::byte const> { packed } });
        std::vector<std::span<std::byte const>> fields { record };
        return WireFields::Encode(WireFields::FieldList { fields });
    };

    SECTION("nine words: a peer built before DispatchAllExcluded")
    {
        auto const decoded = DecodeHistoryBuckets(oneBucket(packWords({ 1, 2, 3, 4, 5, 6, 7, 8, 9 })));
        REQUIRE(decoded.has_value());
        REQUIRE(Unwrap(decoded).size() == 1);
        CHECK(Unwrap(decoded).front().values[8] == 9);
        // The slot this peer never sent stays at its default, not garbage.
        CHECK(Unwrap(decoded).front().values[9] == 0);
    }

    SECTION("eleven words: a peer built after this one's HistorySlotCount")
    {
        auto const decoded = DecodeHistoryBuckets(oneBucket(packWords({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 999 })));
        REQUIRE(decoded.has_value());
        REQUIRE(Unwrap(decoded).size() == 1);
        CHECK(Unwrap(decoded).front().values[9] == 10);
        // The eleventh word is this build's future; it decodes, rather than refuses.
    }
}

TEST_CASE("A heartbeat from a peer that sends no history is not a refusal", "[wire][compile-cache][history]")
{
    using namespace FastCache::CompileCacheWire;

    // What every build before this field looks like on the wire: the nested record
    // simply ends earlier. It has to decode to "no buckets" rather than to a
    // rejection, or the field could never have been added at all -- a refused
    // heartbeat is a worker the fleet stops seeing.
    LoadFields older {};
    older.availableMemoryBytes = 4096;
    auto const encoded = WireFields::Encode({ std::span<std::byte const> {},
                                              std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(4096) },
                                              std::span<std::byte const> {},
                                              std::span<std::byte const> { EncodeCacheLoad(CacheLoadFields {}) } });

    auto const decoded = DecodeLoad(encoded);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).history.empty());
    CHECK(Unwrap(decoded).availableMemoryBytes == older.availableMemoryBytes);
}

TEST_CASE("A history batch above the ceiling is refused, not truncated", "[wire][compile-cache][history]")
{
    using namespace FastCache::CompileCacheWire;

    // The encoder takes at most the ceiling, oldest first, and the rest wait for the
    // next round -- a node absent for a day has 1440 buckets to hand over.
    std::vector<HistoryBucketFields> many(MaxHistoryBucketsPerHeartbeat + 40);
    for (auto const index: std::views::iota(std::size_t { 0 }, many.size()))
        many[index].startMillis = 1'700'000'000'000 + (index * 60'000);

    auto const decoded = DecodeHistoryBuckets(EncodeHistoryBuckets(many));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).size() == MaxHistoryBucketsPerHeartbeat);
    // Oldest first, so a catch-up makes progress from the far end rather than
    // repeatedly resending the newest and never closing the gap.
    CHECK(Unwrap(decoded).front().startMillis == many.front().startMillis);

    // And a PEER that ignores the ceiling is refused rather than quietly clipped:
    // keeping the first 128 would leave the rest looking delivered.
    std::vector<std::span<std::byte const>> overSized;
    std::vector<std::vector<std::byte>> owned;
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, MaxHistoryBucketsPerHeartbeat + 1))
    {
        owned.push_back(WireFields::Encode({ std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(0) },
                                             std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(0) },
                                             std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(0) },
                                             std::span<std::byte const> {} }));
        overSized.emplace_back(owned.back());
    }
    CHECK_FALSE(DecodeHistoryBuckets(WireFields::Encode(WireFields::FieldList { overSized })).has_value());
}

TEST_CASE("The pre-payload gate refuses an unknown opcode before anything else", "[wire][prepayload]")
{
    // Total by design, and the header says why: an earlier draft took a RESOLVED
    // opcode as a precondition, which left the node's loop -- the one surface that
    // does not resolve opcodes before reading -- free to buffer the whole request cap
    // for opcode 0xFF from an unauthenticated peer. Asked for every byte value, that
    // hole cannot be reconstructed.
    //
    // Asserted with the gate OFF as well, because that is the configuration the hole
    // was reachable in: a surface with no credential still must not buffer for a verb
    // it cannot name.
    constexpr std::uint8_t NoSuchVerb = 0xFF;
    static_assert(FindOp(NoSuchVerb) == nullptr, "0xFF must stay unassigned for this case to mean anything");

    CHECK(DecidePrePayload({ .opRaw = NoSuchVerb,
                             .declaredLength = 16,
                             .sessionCap = 64 * 1024,
                             .authRequired = false,
                             .credentialAccepted = false })
          == PrePayloadDecision::UnknownOpcode);
    CHECK(DecidePrePayload({ .opRaw = NoSuchVerb,
                             .declaredLength = 16,
                             .sessionCap = 64 * 1024,
                             .authRequired = true,
                             .credentialAccepted = true })
          == PrePayloadDecision::UnknownOpcode);
}

TEST_CASE("A gated verb is refused without a credential and served with one", "[wire][prepayload]")
{
    // BOTH halves, and the second is the one that matters. A gate that refused
    // everything would satisfy the first on its own while serving nobody, and would
    // look exactly like a working credential check -- which is the shape #355 exists
    // to refuse.
    constexpr auto Gated = static_cast<std::uint8_t>(Op::Lease);
    static_assert(!OpTable[static_cast<std::size_t>(Op::Lease)].preAuth.Allowed(),
                  "this case is about a verb the gate covers");

    auto const request = [](bool authRequired, bool accepted) {
        return PrePayloadRequest { .opRaw = Gated,
                                   .declaredLength = 16,
                                   .sessionCap = 64 * 1024,
                                   .authRequired = authRequired,
                                   .credentialAccepted = accepted };
    };

    CHECK(DecidePrePayload(request(true, false)) == PrePayloadDecision::Unauthenticated);
    CHECK(DecidePrePayload(request(true, true)) == PrePayloadDecision::Serve);

    // A surface with no credential configured serves it, which is what keeps turning
    // a token on at a client from being a breaking change against a server needing
    // none.
    CHECK(DecidePrePayload(request(false, false)) == PrePayloadDecision::Serve);

    // The transposition the request STRUCT exists to prevent, pinned as behaviour so
    // it is caught even if somebody flattens the parameters back out: the two flags
    // are adjacent booleans, and swapping them turns a refusal into a Serve.
    CHECK(DecidePrePayload(request(true, false)) != DecidePrePayload(request(false, true)));
}

TEST_CASE("AUTH itself is reachable before a credential exists, and still bounded", "[wire][prepayload]")
{
    // Otherwise the gate is a deadlock: the verb that establishes the credential
    // would need the credential.
    constexpr auto Auth = static_cast<std::uint8_t>(Op::Auth);
    CHECK(DecidePrePayload({ .opRaw = Auth,
                             .declaredLength = 16,
                             .sessionCap = 64 * 1024,
                             .authRequired = true,
                             .credentialAccepted = false })
          == PrePayloadDecision::Serve);

    // And bounded by its OWN ceiling rather than the session's, which is the whole
    // reason a pre-auth verb carries one: the session cap here is far larger, so a
    // reply of PayloadTooLarge can only have come from `MaxAuthPayload`.
    CHECK(DecidePrePayload({ .opRaw = Auth,
                             .declaredLength = MaxAuthPayload + 1,
                             .sessionCap = 64 * 1024,
                             .authRequired = true,
                             .credentialAccepted = false })
          == PrePayloadDecision::PayloadTooLarge);
}

TEST_CASE("The size ceiling is decided before the credential, not after", "[wire][prepayload]")
{
    // Ordering, and it is deliberate rather than incidental. Refusing on auth first
    // would let a peer that merely holds a connection open declare an enormous
    // payload for a pre-auth verb and take exactly the allocation this gate denies.
    //
    // The observable consequence: an oversize frame from an UNAUTHENTICATED peer
    // reports the size, not the credential.
    constexpr auto Gated = static_cast<std::uint8_t>(Op::Lease);
    CHECK(DecidePrePayload({ .opRaw = Gated,
                             .declaredLength = 1024 * 1024,
                             .sessionCap = 64 * 1024,
                             .authRequired = true,
                             .credentialAccepted = false })
          == PrePayloadDecision::PayloadTooLarge);
}

TEST_CASE("Every verb refuses at its own declared ceiling, not the listener's", "[wire][prepayload]")
{
    // #284. The 64 KiB control ceiling used to be a property of the LISTENER, so it
    // applied uniformly to every verb arriving on it -- wrong in both directions once
    // surfaces share a port: a scheduler verb got 64 KiB of headroom it never needs,
    // and a compile that legitimately carries megabytes could not share the listener
    // at all. A merged port cannot have one ceiling, which is why this is #290's
    // prerequisite.
    //
    // Driven off `OpTable` rather than a list of verbs, so a verb added later is
    // covered by this case without anybody remembering to extend it.
    constexpr std::size_t SessionCap = 256ULL * 1024ULL * 1024ULL;

    for (auto const& row: OpTable)
    {
        INFO("verb " << row.name);
        auto const opRaw = static_cast<std::uint8_t>(row.code);
        auto const cap = OpPayloadCap(opRaw, SessionCap);

        // A declared length is a `uint32_t` on the wire, so the ceiling has to be
        // expressible as one -- which is also why a session cap above 4 GiB could
        // never be reached by a frame that says how big it is.
        REQUIRE(cap <= std::numeric_limits<std::uint32_t>::max() - 1);

        auto const at = [&](std::size_t declared) {
            return DecidePrePayload({ .opRaw = opRaw,
                                      .declaredLength = static_cast<std::uint32_t>(declared),
                                      .sessionCap = SessionCap,
                                      .authRequired = false,
                                      .credentialAccepted = false });
        };

        // Exactly at the ceiling is allowed; one byte over is not. Asserting the
        // boundary rather than "a big number is refused" is what pins the ceiling to
        // this verb's own row -- a listener-wide bound would pass the second half for
        // every verb while getting the first half wrong for the tight ones.
        CHECK(at(cap) == PrePayloadDecision::Serve);
        CHECK(at(cap + 1) == PrePayloadDecision::PayloadTooLarge);

        // And the row's declared bound is what produced that ceiling, rather than the
        // session cap standing in for it.
        if (row.maxPayload.IsBounded())
        {
            CHECK(cap == row.maxPayload.Bytes());
            CHECK(cap < SessionCap);
        }
        else
        {
            CHECK(cap == SessionCap);
        }
    }
}

// --- the node's runtime record (#1294, #1295) --------------------------------

TEST_CASE("A node's runtime record round-trips every toolchain state it can report", "[wire][node-status]")
{
    // Driven over all three rather than one, and with a DIFFERENT count per state,
    // because the failures worth catching both survive a single-state check: an encoder
    // that drops the state decodes disengaged and one that hard-codes a state agrees
    // with whichever case happens to name it. Neither can pass all three.
    struct Row
    {
        ToolchainState state;
        std::uint32_t served;
        std::uint32_t discovered;
    };
    constexpr std::array Rows {
        Row { .state = ToolchainState::Surveying, .served = 0, .discovered = 7 },
        Row { .state = ToolchainState::Serving, .served = 3, .discovered = 4 },
        Row { .state = ToolchainState::NothingToServe, .served = 0, .discovered = 2 },
    };

    for (auto const& row: Rows)
    {
        INFO("state " << static_cast<int>(row.state));
        NodeStatusFields const sent {
            .version = "9.9.9",
            .nodeId = {},
            .uptimeSeconds = 11,
            .surfaces = {},
            .components = NodeComponentBit::Worker,
            .runtime = { .toolchains = row.state, .toolchainsServed = row.served, .toolchainsDiscovered = row.discovered }
        };

        auto const back = DecodeNodeStatus(EncodeNodeStatus(sent));
        REQUIRE(back.has_value());
        auto const& runtime = Unwrap(back).runtime;
        CHECK(runtime.toolchains == std::optional { row.state });
        CHECK(runtime.toolchainsServed == row.served);
        CHECK(runtime.toolchainsDiscovered == row.discovered);

        // The record travels INSIDE `NodeStatus` rather than beside it, so the fields
        // that were already there must survive its arrival.
        CHECK(Unwrap(back).version == "9.9.9");
        CHECK(Unwrap(back).components == NodeComponentBit::Worker);
    }
}

TEST_CASE("A runtime record nobody sent decodes ABSENT, never as a reading", "[wire][node-status]")
{
    // **The discriminating assertion is `has_value()`, not the counts.** An
    // implementation defaulting the state to `Surveying` reports zero served and zero
    // discovered too, so a case that only checks the numbers passes under exactly the
    // collapse this record exists to prevent: *this node said nothing* and *this node is
    // surveying nothing* are different facts.
    auto const empty = DecodeNodeRuntime({});
    REQUIRE(empty.has_value());
    CHECK_FALSE(Unwrap(empty).toolchains.has_value());
    CHECK(Unwrap(empty).toolchainsServed == 0);
    CHECK(Unwrap(empty).toolchainsDiscovered == 0);
}

TEST_CASE("A toolchain state this build cannot name is skipped rather than refused", "[wire][node-status]")
{
    // An older client meeting a newer node reports what it understands rather than
    // declining to report anything -- the rule `DecodeNodeStatus` already holds for an
    // unknown `WireSurface`. The discriminating half is that the record still DECODES:
    // an implementation refusing the unknown byte returns nullopt and takes the whole
    // reply with it, which is how one added enumerator would blind every older operator
    // tool to a node's version, uptime and ports as well.
    auto const unknownState = std::array { std::byte { 0x7F } };
    auto const served = WireFields::ToBigEndian<std::uint32_t>(5);
    auto const record =
        WireFields::Encode({ std::span<std::byte const> { unknownState }, std::span<std::byte const> { served }, {} });

    auto const back = DecodeNodeRuntime(record);
    REQUIRE(back.has_value());
    CHECK_FALSE(Unwrap(back).toolchains.has_value());
    // And the fields it DID understand are still reported.
    CHECK(Unwrap(back).toolchainsServed == 5);
}

TEST_CASE("A runtime record shorter than this build expects keeps its defaults", "[wire][node-status]")
{
    // The variable-arity half of the contract: a sender that named only the state is a
    // peer this build can still read, and the facts it did not send are "did not say"
    // rather than a refusal.
    auto const stateOnly = std::array { static_cast<std::byte>(ToolchainState::Serving) };
    auto const back = DecodeNodeRuntime(WireFields::Encode({ std::span<std::byte const> { stateOnly } }));

    REQUIRE(back.has_value());
    CHECK(Unwrap(back).toolchains == std::optional { ToolchainState::Serving });
    CHECK(Unwrap(back).toolchainsServed == 0);
}

TEST_CASE("A runtime field of the wrong width is refused rather than read", "[wire][node-status]")
{
    // The one thing variable arity does NOT tolerate. A three-byte count is a sender
    // speaking a shape this build does not know, and reading its first bytes would
    // invent a number -- which is worse than refusing, because it is a plausible one.
    auto const state = std::array { static_cast<std::byte>(ToolchainState::Serving) };
    auto const narrow = std::array { std::byte { 0 }, std::byte { 0 }, std::byte { 1 } };
    CHECK_FALSE(DecodeNodeRuntime(
                    WireFields::Encode({ std::span<std::byte const> { state }, std::span<std::byte const> { narrow } }))
                    .has_value());

    // A one-byte state is the only width that byte may have, for the same reason.
    auto const wideState = std::array { std::byte { 0 }, static_cast<std::byte>(ToolchainState::Serving) };
    CHECK_FALSE(DecodeNodeRuntime(WireFields::Encode({ std::span<std::byte const> { wideState } })).has_value());

    // **And the same rule for an OPTIONAL field, which is a different decoder.** The
    // narrow field above sits at index 1, `toolchainsServed`, which is read inline --
    // so relaxing `ReadOptionalBigEndian`, which owns the width rule for every optional
    // field in this record, reddened nothing at all. Measured, by mutation. This arm
    // puts the narrow field at index 3, `compileSlots`, where that decoder is reached.
    auto const okState = std::array { static_cast<std::byte>(ToolchainState::Serving) };
    auto const count = WireFields::ToBigEndian<std::uint32_t>(1);
    CHECK_FALSE(DecodeNodeRuntime(WireFields::Encode({ std::span<std::byte const> { okState },
                                                       std::span<std::byte const> { count },
                                                       std::span<std::byte const> { count },
                                                       std::span<std::byte const> { narrow } }))
                    .has_value());
}

TEST_CASE("A NodeStatus body at the previous arity is refused, not read short", "[wire][node-status]")
{
    // What the version bump to 7 is FOR. `SplitFields` is exact, so the five-field body
    // a version-6 node emits is not a version-7 body missing its last field -- it is a
    // payload this build declines to read, loudly. Written out by hand because no
    // encoder in this build can produce it any more, which is the point.
    auto const surfaces = WireFields::Encode(WireFields::FieldList { std::vector<std::span<std::byte const>> {} });
    auto const uptime = EncodeU64Field(1);
    auto const components = EncodeU32Field(0);
    auto const fiveFieldBody = WireFields::Encode({ AsBytes(std::string_view { "1.2.3" }),
                                                    {},
                                                    std::span<std::byte const> { uptime },
                                                    std::span<std::byte const> { components },
                                                    std::span<std::byte const> { surfaces } });

    CHECK_FALSE(DecodeNodeStatus(fiveFieldBody).has_value());
}

TEST_CASE("A runtime record round-trips what a node is doing, not only what it runs", "[wire][node-status]")
{
    NodeRuntimeFields const sent { .toolchains = ToolchainState::Serving,
                                   .toolchainsServed = 2,
                                   .toolchainsDiscovered = 2,
                                   .compileSlots = 8,
                                   .compilesInFlight = 3,
                                   .schedulerRole = WireSchedulerRole::Follower,
                                   .leaderEndpoint = "10.0.0.9:6676",
                                   .registrarsRegistered = 2,
                                   .registrarsTotal = 3,
                                   .lastRegistrationSecondsAgo = 41 };

    auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
    REQUIRE(back.has_value());
    auto const& got = Unwrap(back);

    CHECK(got.compileSlots == std::optional<std::uint32_t> { 8 });
    CHECK(got.compilesInFlight == std::optional<std::uint32_t> { 3 });
    CHECK(got.schedulerRole == std::optional { WireSchedulerRole::Follower });
    CHECK(got.leaderEndpoint == "10.0.0.9:6676");
    CHECK(got.registrarsRegistered == std::optional<std::uint32_t> { 2 });
    CHECK(got.registrarsTotal == std::optional<std::uint32_t> { 3 });
    CHECK(got.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 41 });
    // The fields that were already there are unmoved by the ones appended after them,
    // which is the property a POSITIONAL record can silently lose.
    CHECK(got.toolchains == std::optional { ToolchainState::Serving });
    CHECK(got.toolchainsServed == 2);
}

TEST_CASE("An absent runtime fact and a zero one survive as different answers", "[wire][node-status]")
{
    // **The discriminating pair, and neither half alone is one.** An encoder that writes
    // a zero VALUE for a disengaged optional passes any round trip that only sends
    // engaged ones; a decoder that engages every field it sees passes any that only
    // sends absent ones. Sent together, one record must come back saying nothing and the
    // other must come back saying zero.
    NodeRuntimeFields silent {};
    NodeRuntimeFields stated {};
    stated.compileSlots = 0;
    stated.compilesInFlight = 0;
    stated.registrarsRegistered = 0;
    stated.lastRegistrationSecondsAgo = 0;

    auto const quiet = DecodeNodeRuntime(EncodeNodeRuntime(silent));
    auto const loud = DecodeNodeRuntime(EncodeNodeRuntime(stated));
    REQUIRE(quiet.has_value());
    REQUIRE(loud.has_value());

    // **`REQUIRE` and not `CHECK`, and the reason is the harness rather than the
    // subject.** Catch2's exit code IS its failed-assertion count, and this project
    // registers `SKIP_RETURN_CODE 4`, so a case that fails exactly FOUR assertions is
    // scored SKIPPED rather than failed -- #1128, whose repair is a change of mechanism
    // tracked as #1152. These four fail together under exactly the defect the case
    // exists to catch, so as `CHECK`s the case reported `***Skipped` while genuinely
    // red: measured, during the mutation run for this change. `REQUIRE` aborts at the
    // first failure, so the case can only ever report ONE -- which is deterministic and
    // is not 4. The cost is that a real failure names one field instead of four, and
    // they are one family that moves together.
    REQUIRE_FALSE(Unwrap(quiet).compileSlots.has_value());
    REQUIRE_FALSE(Unwrap(quiet).compilesInFlight.has_value());
    REQUIRE_FALSE(Unwrap(quiet).registrarsRegistered.has_value());
    REQUIRE_FALSE(Unwrap(quiet).lastRegistrationSecondsAgo.has_value());

    REQUIRE(Unwrap(loud).compileSlots == std::optional<std::uint32_t> { 0 });
    REQUIRE(Unwrap(loud).compilesInFlight == std::optional<std::uint32_t> { 0 });
    REQUIRE(Unwrap(loud).registrarsRegistered == std::optional<std::uint32_t> { 0 });
    REQUIRE(Unwrap(loud).lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 0 });
}

TEST_CASE("A scheduler role this build cannot name is skipped rather than refused", "[wire][node-status]")
{
    // The same rule the toolchain state holds, and worth its own case because it is a
    // second enum in one record: an unnamed byte must not cost a reader the rest of the
    // record, or one added enumerator blinds every older tool to a node's slots and
    // registrations as well.
    NodeRuntimeFields sent {};
    sent.compileSlots = 5;
    auto record = EncodeNodeRuntime(sent);

    // Rebuilt by hand with an unnamed role, because no encoder in this build emits one.
    auto const role = std::array { std::byte { 0x6E } };
    auto const slots = WireFields::ToBigEndian<std::uint32_t>(5);
    record =
        WireFields::Encode({ {}, {}, {}, std::span<std::byte const> { slots }, {}, std::span<std::byte const> { role } });

    auto const back = DecodeNodeRuntime(record);
    REQUIRE(back.has_value());
    CHECK_FALSE(Unwrap(back).schedulerRole.has_value());
    CHECK(Unwrap(back).compileSlots == std::optional<std::uint32_t> { 5 });
}

TEST_CASE("A verb's ceiling never exceeds what the operator configured", "[wire][prepayload]")
{
    // The per-verb bound is an ADDITIONAL restriction, never a licence to exceed the
    // session cap. Checked with a session cap far below every row's own bound, which
    // is the configuration an operator lowering the limit actually creates.
    constexpr std::size_t TinySession = 512;

    for (auto const& row: OpTable)
    {
        INFO("verb " << row.name);
        CHECK(OpPayloadCap(static_cast<std::uint8_t>(row.code), TinySession) <= TinySession);
    }
}

// --- the CLUSTER-ADMIT receipt ---------------------------------------------

TEST_CASE("A CLUSTER-ADMIT receipt round-trips what the leader wrote down", "[wire][cluster-admit]")
{
    // Both fields carry a DIFFERENT value and neither is a substring of the other:
    // two fields sharing one value let a transposed index through, which is the
    // mistake a two-field encoder invites and the one `RaftWire`'s exemplars were
    // rewritten to catch.
    //
    // Three fields since #178, and the key is a third distinct value for the same reason.
    auto const sent = ClusterAdmitReceipt { .memberId = "node-c",
                                            .raftEndpoint = "10.0.0.9:6675",
                                            .publicKey = "PKeyPKeyPKeyPKeyPKeyPKeyPKeyPKeyPKeyPKeyPKe" };

    auto const back = DecodeClusterAdmitReceipt(EncodeClusterAdmitReceipt(sent));
    REQUIRE(back.has_value());

    // Compared WHOLE rather than field by field, which is what a transposition
    // survives when each field is checked against the value it was handed.
    CHECK(Unwrap(back) == sent);
}

TEST_CASE("A receipt survives its own buffer, because an operator reads it", "[wire][cluster-admit]")
{
    // `Decode(Encode(x))` above is the obvious spelling and is a use-after-free the
    // moment either member becomes a view, which is why this record OWNS. Asserted
    // rather than left to the type, because the type is exactly what a later tidy
    // would change.
    //
    // A REAL size and the right ARRANGEMENT, or the case passes under the bug: read
    // inline, nothing dangles at any size, because a by-value parameter lives to the
    // end of the full expression. So the value is STORED, its source dropped, the
    // freed storage churned, and only then read -- and the strings are past
    // libstdc++'s 15-char inline buffer, where the block is heap and the failure is a
    // plain `heap-use-after-free` rather than something only ASan's stack poisoning
    // can see.
    constexpr std::string_view Endpoint = "[2001:db8:85a3::8a2e:370:7334]:6675";
    static_assert(Endpoint.size() > 15, "shorter than the SSO buffer and this case stops biting");

    auto receipt = std::optional<ClusterAdmitReceipt> {};
    {
        // A NAMED local, not a temporary in the initialiser. The difference is which
        // instrument catches a borrowing receipt: from a temporary the compiler refuses
        // the spelling outright, so the case would never run and the property it is
        // named for -- that the DECODED record outlives the payload -- would be
        // demonstrated by nothing.
        auto const endpoint = std::string { Endpoint };
        auto const payload = EncodeClusterAdmitReceipt(ClusterAdmitReceipt {
            .memberId = "a-node-id-of-a-realistic-length", .raftEndpoint = endpoint, .publicKey = std::nullopt });
        receipt = DecodeClusterAdmitReceipt(payload);
    }

    // Churn whatever the payload's allocation has become. The assertion is not the
    // point of the case: it is what stops an optimiser deciding this vector is never
    // read and eliding the allocations that do the churning.
    auto const churn = std::vector<std::string>(64, std::string(128, 'x'));
    CHECK(churn.size() == 64);

    REQUIRE(receipt.has_value());
    CHECK(Unwrap(receipt).raftEndpoint == Endpoint);
}

TEST_CASE("A CLUSTER-ADMIT reply with no receipt in it is refused, not read as nothing", "[wire][cluster-admit]")
{
    // An empty body is what a build older than this one answers, and *this leader
    // recorded nothing* is not a state that exists -- so it is `MinSupportedVersion`'s
    // question rather than a shape to tolerate here. Read as an absent receipt it
    // would render as a blank endpoint, which is the missing string the whole change
    // exists to prevent, arriving through the decoder.
    CHECK_FALSE(DecodeClusterAdmitReceipt({}).has_value());
}

TEST_CASE("A receipt at any arity but three is refused", "[wire][cluster-admit]")
{
    // Exact, like every other reply body here, and BOTH directions: a decoder that
    // only refuses short bodies reads a longer one's first fields and silently drops
    // whatever a newer leader added beside them. The TWO-field body is a version-10
    // receipt, and refusing it is what keeps a key the leader did record from reading
    // as none (#178).
    auto const one = WireFields::Encode({ AsBytes(std::string_view { "node-c" }) });
    CHECK_FALSE(DecodeClusterAdmitReceipt(one).has_value());

    auto const two =
        WireFields::Encode({ AsBytes(std::string_view { "node-c" }), AsBytes(std::string_view { "10.0.0.9:6675" }) });
    CHECK_FALSE(DecodeClusterAdmitReceipt(two).has_value());

    auto const four = WireFields::Encode({ AsBytes(std::string_view { "node-c" }),
                                           AsBytes(std::string_view { "10.0.0.9:6675" }),
                                           AsBytes(std::string_view {}),
                                           AsBytes(std::string_view { "something-newer" }) });
    CHECK_FALSE(DecodeClusterAdmitReceipt(four).has_value());

    // The control: the same bytes at three fields read.
    auto const three = WireFields::Encode({ AsBytes(std::string_view { "node-c" }),
                                            AsBytes(std::string_view { "10.0.0.9:6675" }),
                                            AsBytes(std::string_view {}) });
    CHECK(DecodeClusterAdmitReceipt(three).has_value());
}

TEST_CASE("A receipt names the key the leader recorded, or none, and none is not an empty key",
          "[wire][cluster-admit][identity]")
{
    // #178. Both cases, because a decoder that always produced a key -- or never did --
    // passes one of them. None travels as a ZERO-LENGTH third field and comes back
    // DISENGAGED: the decoder is the one place the encoding of absence is read.
    auto const keyed = ClusterAdmitReceipt { .memberId = "node-c",
                                             .raftEndpoint = "10.0.0.9:6675",
                                             .publicKey = "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK" };
    auto const keyedBack = DecodeClusterAdmitReceipt(EncodeClusterAdmitReceipt(keyed));
    REQUIRE(keyedBack.has_value());
    CHECK(Unwrap(keyedBack).publicKey == keyed.publicKey);

    auto const bare =
        ClusterAdmitReceipt { .memberId = "node-c", .raftEndpoint = "10.0.0.9:6675", .publicKey = std::nullopt };
    auto const bareBytes = EncodeClusterAdmitReceipt(bare);
    auto const fields = WireFields::SplitExactly(bareBytes, 3);
    REQUIRE(fields.has_value());
    CHECK(Unwrap(fields)[2].empty());

    auto const bareBack = DecodeClusterAdmitReceipt(bareBytes);
    REQUIRE(bareBack.has_value());
    CHECK_FALSE(Unwrap(bareBack).publicKey.has_value());
    CHECK(Unwrap(bareBack) == bare);
}

TEST_CASE("A receipt carries the bytes it was given, not a tidied version of them", "[wire][cluster-admit]")
{
    // The receipt's one claim is *these are the bytes I wrote down*, so a codec that
    // trimmed, folded case or otherwise normalised either field would make it a claim
    // about something else -- and a typo is exactly the kind of difference a
    // normaliser removes. Surrounding space and an unexpected case are both things an
    // operator's shell and keyboard produce; what this case pins is that they come
    // back unchanged, so they can be SEEN.
    auto const odd =
        ClusterAdmitReceipt { .memberId = " Node-C ", .raftEndpoint = "[2001:DB8::1]:6675", .publicKey = std::nullopt };

    auto const back = DecodeClusterAdmitReceipt(EncodeClusterAdmitReceipt(odd));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back) == odd);
}

TEST_CASE("The enrollment verbs occupy the bytes they were assigned, and the bytes are what travels", "[wire][enrollment]")
{
    // **The VALUE, not the name.** A symbol both ends spell can only test the first of
    // the two facts a wire constant has; change the enumerator consistently and every
    // in-tree test still agrees while every deployed peer breaks. These bytes were
    // picked because nothing had claimed them, which is a statement about the wire
    // rather than about this build's vocabulary.
    CHECK(static_cast<std::uint8_t>(Op::Enroll) == 0x10);
    CHECK(static_cast<std::uint8_t>(Op::EnrollControl) == 0x11);
    CHECK(static_cast<std::uint8_t>(ErrorCode::EnrollmentFull) == 0x23);

    // And nothing else answers to them, which a pair of equality checks does not cover:
    // a second row claiming one of these bytes would leave every assertion above true.
    CHECK(std::ranges::count(OpTable, Op::Enroll, &OpDescriptor::code) == 1);
    CHECK(std::ranges::count(OpTable, Op::EnrollControl, &OpDescriptor::code) == 1);
    CHECK(std::ranges::count(ErrorTable, ErrorCode::EnrollmentFull, &ErrorDescriptor::code) == 1);
}

TEST_CASE("Enrollment is its own verb family, and only the joiner's half is reachable before auth", "[wire][enrollment]")
{
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Enroll)) == VerbFamily::Enrollment);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::EnrollControl)) == VerbFamily::Enrollment);

    // **The asymmetry IS the design.** A case asserting only that `Enroll` is pre-auth
    // would pass under a build that opened the whole family, which is the one mistake
    // this split exists to prevent: the decision verb admits a stranger's key to the fleet.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::Enroll)));
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::EnrollControl)));

    // The COUNT of pre-auth rows is deliberately NOT re-asserted here. It already lives
    // in "Exactly the verbs meant to be reachable before AUTH are reachable", and two
    // cases asserting one number is two things to be wrong rather than a cross-check --
    // a third pre-auth verb should redden one case naming the set, not two arguing
    // about it. What is this case's own is the ASYMMETRY: the family is split down the
    // middle and both halves are named.

    // The pre-auth one is bounded far under the control cap, because the two are
    // bounded against different populations: a peer that authenticated once, and
    // anybody who can route to the port.
    CHECK(OpPayloadCap(static_cast<std::uint8_t>(Op::Enroll), MaxFramePayload) == MaxEnrollPayload);
    CHECK(OpPayloadCap(static_cast<std::uint8_t>(Op::EnrollControl), MaxFramePayload) == MaxControlPayload);
}

TEST_CASE("An enroll request round-trips, and a payload of the wrong arity is refused", "[wire][enrollment]")
{
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { 0x42 });
    auto nonce = std::array<std::byte, NodeChallengeBytes> {};
    nonce.fill(std::byte { 0x77 });
    auto signature = std::array<std::byte, NodeSignatureBytes> {};
    signature.fill(std::byte { 0x5C });
    auto const frame = EncodeEnroll(EnrollRequest { .nodeId = "joiner-a",
                                                    .nodeEndpoint = "10.0.0.9:6674",
                                                    .role = EnrollRole::Worker,
                                                    .publicKey = key,
                                                    .nonce = nonce,
                                                    .challenge = {},
                                                    .signature = signature });
    auto const header = DecodeRequestHeader(frame);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Op::Enroll));
    CHECK(OpFieldCount(Op::Enroll) == 7);

    auto const fields = DecodeEnrollPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(fields.has_value());
    CHECK(AsStringView(Unwrap(fields).nodeId) == "joiner-a");
    CHECK(AsStringView(Unwrap(fields).nodeEndpoint) == "10.0.0.9:6674");
    CHECK(Unwrap(fields).role == EnrollRole::Worker);
    CHECK(Unwrap(fields).publicKey == key);
    CHECK(Unwrap(fields).nonce == nonce);
    CHECK_FALSE(Unwrap(fields).challenge.has_value()); // a first ask answers none
    CHECK(Unwrap(fields).signature == signature);

    // And the leader's challenge, when a request answers one, travels whole.
    auto answered = EnrollChallenge {};
    answered.fill(std::byte { 0x2B });
    auto const refresh = EncodeEnroll(EnrollRequest { .nodeId = "joiner-a",
                                                      .nodeEndpoint = "10.0.0.9:6674",
                                                      .role = EnrollRole::Learner,
                                                      .publicKey = key,
                                                      .nonce = nonce,
                                                      .challenge = answered,
                                                      .signature = signature });
    auto const refreshed = DecodeEnrollPayload(std::span<std::byte const> { refresh }.subspan(RequestHeaderSize));
    REQUIRE(refreshed.has_value());
    CHECK(Unwrap(refreshed).challenge == answered);

    // The role's BYTES, since they travel: a symbol both ends spell tests only the name.
    CHECK(static_cast<std::uint8_t>(EnrollRole::Worker) == 0x02);
    CHECK(static_cast<std::uint8_t>(EnrollRole::Learner) == 0x03);

    // Two fields where seven are declared -- a version-11 request's shape -- refused on the
    // count alone, which is what keeps a peer speaking a shape this build does not know
    // from being read as a short one. So is the four-field request of the grammar before the
    // nonce, a request an admission cannot be signed over, the five-field one before the
    // joiner's signature, a request whose endpoint is anybody's word -- and the six-field one
    // before the leader's challenge, a request a recording can replay.
    auto const id = AsBytes(std::string_view { "joiner-a" });
    auto const endpoint = AsBytes(std::string_view { "10.0.0.9:6674" });
    auto const learner = std::array { static_cast<std::byte>(EnrollRole::Learner) };
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint })).has_value());
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key })).has_value());
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, nonce })).has_value());
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, nonce, signature })).has_value());

    // A role this build cannot name, a key or a nonce that is not 32 bytes, a challenge that is
    // neither absent nor 32 bytes and a signature that is not 64 are refused rather than defaulted:
    // a joiner recorded under a key it did not send is admitted under a key nobody holds, a role
    // guessed is a machine admitted as something it did not ask to be, a nonce or a challenge cut
    // short is one both ends would sign and verify over different bytes, and a prefix of a
    // signature verifies nothing.
    auto const none = std::span<std::byte const> {};
    auto const unknownRole = std::array { std::byte { 0x7F } };
    auto const shortKey = std::span<std::byte const> { key }.first(IdentityPublicKeyBytes - 1);
    auto const shortNonce = std::span<std::byte const> { nonce }.first(NodeChallengeBytes - 1);
    auto const shortChallenge = std::span<std::byte const> { answered }.first(NodeChallengeBytes - 1);
    auto const shortSignature = std::span<std::byte const> { signature }.first(NodeSignatureBytes - 1);
    CHECK_FALSE(
        DecodeEnrollPayload(WireFields::Encode({ id, endpoint, unknownRole, key, nonce, none, signature })).has_value());
    CHECK_FALSE(
        DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, shortKey, nonce, none, signature })).has_value());
    CHECK_FALSE(
        DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, shortNonce, none, signature })).has_value());
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, nonce, shortChallenge, signature }))
                    .has_value());
    CHECK_FALSE(
        DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, nonce, none, shortSignature })).has_value());
    CHECK(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, nonce, none, signature })).has_value());
    CHECK(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, learner, key, nonce, answered, signature })).has_value());

    // The RETIRED role byte is refused like any unknown one, never read as its successor: 0x01
    // was `Member`, and a joiner still sending it asks for a seat this build no longer grants.
    auto const retiredMember = std::array { std::byte { 0x01 } };
    CHECK_FALSE(
        DecodeEnrollPayload(WireFields::Encode({ id, endpoint, retiredMember, key, nonce, none, signature })).has_value());
}

TEST_CASE("A pending enroll reply carries a zero-length roster field rather than an absent one", "[wire][enrollment]")
{
    // **The property the reply's whole shape exists for.** A client asserts the LENGTH,
    // because an outcome byte is equally correct in a healthy build and in one that
    // answered a waiting machine with the roster anyway -- so the roster travels as a
    // field of its own and "there is none" is a length of zero rather than a convention.
    //
    // Every payload below is a NAMED local rather than a temporary, because
    // `EnrollReplyView` borrows: `Decode(Encode(x))` is the obvious spelling and reads
    // freed memory the moment the full expression ends. That is the rule the type is
    // named `*View` to announce, and this file is where it gets tested rather than
    // demonstrated -- the first draft of this case was written the obvious way and the
    // payload came back as thirteen bytes of rubble.
    auto const pendingPayload = EncodeEnrollReply(EnrollOutcome::Pending, {}, {}, std::nullopt);
    auto const pending = DecodeEnrollReply(pendingPayload);
    REQUIRE(pending.has_value());
    CHECK(Unwrap(pending).outcome == EnrollOutcome::Pending);
    CHECK(Unwrap(pending).roster.empty());

    auto const roster = AsBytes(std::string_view { "roster-bytes" });
    auto const approvedPayload = EncodeEnrollReply(EnrollOutcome::Approved, roster, {}, std::nullopt);
    auto const approved = DecodeEnrollReply(approvedPayload);
    REQUIRE(approved.has_value());
    CHECK(Unwrap(approved).outcome == EnrollOutcome::Approved);
    CHECK(AsStringView(Unwrap(approved).roster) == "roster-bytes");

    // The outcome BYTES, since they travel: a symbol both ends spell tests only the name.
    CHECK(static_cast<std::uint8_t>(EnrollOutcome::Pending) == 0x01);
    CHECK(static_cast<std::uint8_t>(EnrollOutcome::Approved) == 0x02);
    CHECK(static_cast<std::uint8_t>(EnrollOutcome::Rejected) == 0x03);

    // An outcome byte this build has no name for is REFUSED rather than skipped, which
    // is the opposite of the rule for a surface tag one level up and is right here: a
    // joiner that cannot tell approved from rejected has no safe default -- reading it
    // as pending polls forever, reading it as approved reads a roster out of a field that
    // may hold anything.
    auto const tag = std::array { std::byte { 0x7F } };
    CHECK_FALSE(DecodeEnrollReply(WireFields::Encode({ std::span<std::byte const> { tag }, {} })).has_value());
}

TEST_CASE("An enroll-control request carries a subject exactly when its verb takes one", "[wire][enrollment]")
{
    // Named frames, for the reason the reply case above gives: `EnrollControlView`
    // borrows its subject out of the bytes it was handed.
    for (auto const verb: { EnrollControlVerb::List, EnrollControlVerb::AutoApproveOff, EnrollControlVerb::Clear })
    {
        auto const frame = EncodeEnrollControl(verb);
        auto const decoded = DecodeEnrollControlPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).verb == verb);
        CHECK(Unwrap(decoded).subject.empty());
        CHECK_FALSE(Unwrap(decoded).duration.has_value());
    }

    {
        auto const frame = EncodeEnrollControl(EnrollControlVerb::Reject, "joiner-a");
        auto const decoded = DecodeEnrollControlPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).verb == EnrollControlVerb::Reject);
        CHECK(AsStringView(Unwrap(decoded).subject) == "joiner-a");
        CHECK_FALSE(Unwrap(decoded).duration.has_value());
        CHECK_FALSE(Unwrap(decoded).key.has_value());
    }

    // An approval carries the key it means, split back off the id -- whatever the id holds, since a
    // key is fixed-length.
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { 0x42 });
    for (auto const* id: { "joiner-a", "@", "an-id-longer-than-a-key-is-still-an-id-and-not-a-key" })
    {
        INFO(id);
        auto const frame = EncodeEnrollApprove(id, key);
        auto const decoded = DecodeEnrollControlPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).verb == EnrollControlVerb::Approve);
        CHECK(AsStringView(Unwrap(decoded).subject) == id);
        CHECK(Unwrap(decoded).key == std::optional { key });
    }
    // An approval naming an id and no key -- what a client built before the key rode with it
    // would send under this byte -- is refused, never read as a key with a short id.
    CHECK_FALSE(DecodeEnrollControlPayload(
                    std::span<std::byte const> { EncodeEnrollControl(EnrollControlVerb::Approve, "joiner-a") }.subspan(
                        RequestHeaderSize))
                    .has_value());

    // **Both directions of the arity rule**, because each alone passes under a decoder
    // that checks only the other: a `List` naming an id, and an `Approve` naming
    // nobody. Answering either by ignoring the mismatch is how an operator comes to
    // believe they approved somebody.
    CHECK_FALSE(DecodeEnrollControlPayload(
                    std::span<std::byte const> { EncodeEnrollControl(EnrollControlVerb::List, "joiner-a") }.subspan(
                        RequestHeaderSize))
                    .has_value());
    CHECK_FALSE(
        DecodeEnrollControlPayload(
            std::span<std::byte const> { EncodeEnrollControl(EnrollControlVerb::Approve) }.subspan(RequestHeaderSize))
            .has_value());

    // A verb byte this build cannot name is refused rather than dispatched, so a
    // surface never has to carry an arm for a verb it has no meaning for.
    auto const unknown = std::array { std::byte { 0x7F } };
    CHECK_FALSE(DecodeEnrollControlPayload(WireFields::Encode({ std::span<std::byte const> { unknown }, {} })).has_value());
}

TEST_CASE("An enrollment report round-trips every row, ages included", "[wire][enrollment]")
{
    auto keyA = std::array<std::byte, IdentityPublicKeyBytes> {};
    keyA.fill(std::byte { 0xA1 });
    auto keyB = std::array<std::byte, IdentityPublicKeyBytes> {};
    keyB.fill(std::byte { 0xB2 });
    auto fingerprint = std::array<std::byte, RosterFingerprintBytes> {};
    fingerprint.fill(std::byte { 0xF0 });

    EnrollmentReport const report { .state = WireEnrollmentState::AutoApprove,
                                    .autoApproveSecondsLeft = 137,
                                    .pending = { EnrollmentPendingEntry { .nodeId = "joiner-a",
                                                                          .nodeEndpoint = "10.0.0.9:6674",
                                                                          .peerId = "10.0.0.9",
                                                                          .firstSeenSecondsAgo = 90,
                                                                          .attempts = 45,
                                                                          .claimsChanged = 0,
                                                                          .decision = EnrollmentDecision::Pending,
                                                                          .role = EnrollRole::Learner,
                                                                          .publicKey = keyA,
                                                                          .rosterFingerprint = std::nullopt,
                                                                          .autoApprovedArmedSecondsAgo = std::nullopt,
                                                                          .firstPeerId = "10.0.0.9" },
                                                 EnrollmentPendingEntry { .nodeId = "joiner-b",
                                                                          .nodeEndpoint = {},
                                                                          .peerId = "198.51.100.4",
                                                                          .firstSeenSecondsAgo = 12,
                                                                          .attempts = 6,
                                                                          .claimsChanged = 3,
                                                                          .decision = EnrollmentDecision::Approved,
                                                                          .role = EnrollRole::Worker,
                                                                          .publicKey = keyB,
                                                                          .rosterFingerprint = fingerprint,
                                                                          .autoApprovedArmedSecondsAgo = 4,
                                                                          .firstPeerId = "203.0.113.9" } } };

    auto const back = DecodeEnrollmentReport(EncodeEnrollmentReport(report));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).state == WireEnrollmentState::AutoApprove);
    CHECK(Unwrap(back).autoApproveSecondsLeft == 137);
    REQUIRE(Unwrap(back).pending.size() == 2);

    // Every field of a row rather than a sample of them: these differ only by position
    // in the nested record, so a transposed pair would leave any single assertion true.
    // The two hosts particularly, since the whole point of the row is that a person
    // compares them.
    auto const& first = Unwrap(back).pending.front();
    CHECK(first.nodeId == "joiner-a");
    CHECK(first.nodeEndpoint == "10.0.0.9:6674");
    CHECK(first.peerId == "10.0.0.9");
    CHECK(first.firstSeenSecondsAgo == 90);
    CHECK(first.attempts == 45);
    CHECK(first.claimsChanged == 0);
    CHECK(first.decision == EnrollmentDecision::Pending);
    CHECK(first.role == EnrollRole::Learner);
    CHECK(first.publicKey == keyA);
    CHECK(first.firstPeerId == "10.0.0.9");

    // Absent stays ABSENT: a disengaged fingerprint must not come back as thirty-two zero
    // bytes, which would render as a fingerprint the joiner could never have printed. And a
    // row a person decided carries no auto-approval, rather than one armed zero seconds ago.
    CHECK_FALSE(first.rosterFingerprint.has_value());
    CHECK_FALSE(first.autoApprovedArmedSecondsAgo.has_value());

    // The second row differs from the first in every numeric field, so a transposition
    // between the two u32s -- `attempts` and `claimsChanged` -- cannot pass by both
    // happening to be zero. That pair is the one worth arranging against: they are the
    // same width, adjacent in meaning, and the marks an operator reads sit on one of
    // them.
    auto const& second = Unwrap(back).pending.back();
    CHECK(second.attempts == 6);
    CHECK(second.claimsChanged == 3);
    CHECK(second.decision == EnrollmentDecision::Approved);
    CHECK(second.nodeEndpoint.empty());
    CHECK(second.role == EnrollRole::Worker);
    CHECK(second.publicKey == keyB);
    CHECK(second.rosterFingerprint == fingerprint);
    CHECK(second.autoApprovedArmedSecondsAgo == std::optional<std::uint64_t> { 4 });
    // A row whose machine moved: the first address travels apart from the current one, and
    // neither is overwritten by the other.
    CHECK(second.peerId == "198.51.100.4");
    CHECK(second.firstPeerId == "203.0.113.9");

    // The decision BYTES, including the hole: `0x03` was `Collected`, retired with the
    // spend (#178), and never reused -- a row still carrying it is refused, because the
    // reader is a person deciding who to admit and a guessed state is the one wrong answer.
    CHECK(static_cast<std::uint8_t>(EnrollmentDecision::Pending) == 0x01);
    CHECK(static_cast<std::uint8_t>(EnrollmentDecision::Approved) == 0x02);
    CHECK(static_cast<std::uint8_t>(EnrollmentDecision::Rejected) == 0x04);
    CHECK_FALSE(IsKnownEnrollmentDecision(0x03));

    // A manual window with nothing waiting is a real reading rather than an empty
    // message, and round-trips as one.
    auto const quiet = DecodeEnrollmentReport(EncodeEnrollmentReport(EnrollmentReport {}));
    REQUIRE(quiet.has_value());
    CHECK(Unwrap(quiet).state == WireEnrollmentState::Manual);
    CHECK(Unwrap(quiet).autoApproveSecondsLeft == 0);
    CHECK(Unwrap(quiet).pending.empty());

    // A RETIRED state byte is refused rather than read as its successor: the reader is a person
    // deciding whom to admit, and a guessed mode is the one wrong answer.
    for (auto const raw: RetiredEnrollmentStates)
    {
        INFO("state byte " << static_cast<int>(raw));
        auto const tag = std::array { static_cast<std::byte>(raw) };
        auto const seconds = EncodeU64Field(0);
        auto const rows = WireFields::Encode(WireFields::FieldList {});
        CHECK_FALSE(DecodeEnrollmentReport(WireFields::Encode({ std::span<std::byte const> { tag },
                                                                std::span<std::byte const> { seconds },
                                                                std::span<std::byte const> { rows } }))
                        .has_value());
    }
}

TEST_CASE("A pending row is read at twelve facts and a surplus is skipped and fewer are refused", "[wire][enrollment]")
{
    // **The row's own variable arity.** A row from a build that records one more fact than
    // this one knows about is read for what it does know, exactly as `DecodeNodeRuntime`
    // reads its own record. The floor is every POSITION this build reads: the key above all,
    // which is not optional -- a row without one is a row an operator cannot check -- and the
    // two optional facts, the fingerprint and the auto-approval, whose absence travels as a
    // zero-length field rather than as a missing one. So fewer than twelve is refused rather
    // than padded.
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { 0x42 });
    EnrollmentReport const report { .state = WireEnrollmentState::AutoApprove,
                                    .autoApproveSecondsLeft = 5,
                                    .pending = { EnrollmentPendingEntry { .nodeId = "joiner-a",
                                                                          .nodeEndpoint = "10.0.0.9:6674",
                                                                          .peerId = "10.0.0.9",
                                                                          .firstSeenSecondsAgo = 1,
                                                                          .attempts = 2,
                                                                          .claimsChanged = 7,
                                                                          .decision = EnrollmentDecision::Approved,
                                                                          .role = EnrollRole::Learner,
                                                                          .publicKey = key,
                                                                          .rosterFingerprint = std::nullopt,
                                                                          .autoApprovedArmedSecondsAgo = 30,
                                                                          .firstPeerId = "10.0.0.9" } } };

    auto const encoded = EncodeEnrollmentReport(report);
    auto const outer = WireFields::SplitAll(encoded);
    REQUIRE(outer.has_value());
    REQUIRE(Unwrap(outer).size() == 3);
    auto const rows = WireFields::SplitAll(Unwrap(outer)[2]);
    REQUIRE(rows.has_value());
    REQUIRE(Unwrap(rows).size() == 1);
    auto const parts = WireFields::SplitAll(Unwrap(rows).front());
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() == 12);

    // The report carrying one row made of these fields.
    auto const reportWith = [&outer](std::vector<std::span<std::byte const>> const& fields) {
        auto const row = WireFields::Encode(WireFields::FieldList { fields });
        auto const packedRows = std::array { std::span<std::byte const> { row } };
        auto const packed = WireFields::Encode(WireFields::FieldList { packedRows });
        return DecodeEnrollmentReport(
            WireFields::Encode({ Unwrap(outer)[0], Unwrap(outer)[1], std::span<std::byte const> { packed } }));
    };

    // A row from a build carrying one fact more than this one knows of.
    auto surplus = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
    auto const extra = AsBytes(std::string_view { "a fact from the future" });
    surplus.emplace_back(extra);
    auto const widerBack = reportWith(surplus);
    REQUIRE(widerBack.has_value());
    REQUIRE(Unwrap(widerBack).pending.size() == 1);
    CHECK(Unwrap(widerBack).pending.front().claimsChanged == 7);
    CHECK(Unwrap(widerBack).pending.front().publicKey == key);
    CHECK(Unwrap(widerBack).pending.front().autoApprovedArmedSecondsAgo == std::optional<std::uint64_t> { 30 });

    // ELEVEN -- the row without its first-address slot -- is refused, not padded: reading a
    // missing field would invent a value rather than omit a fact, and that one is what a
    // host's cap is counted by.
    auto const truncated = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 11 };
    CHECK_FALSE(reportWith(truncated).has_value());

    // And the exact twelve is read, which is the control for the refusal above.
    auto const exact = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
    REQUIRE(reportWith(exact).has_value());
    CHECK(Unwrap(reportWith(exact)).pending.front().autoApprovedArmedSecondsAgo == std::optional<std::uint64_t> { 30 });
    CHECK(Unwrap(reportWith(exact)).pending.front().firstPeerId == "10.0.0.9");

    // An auto-approval of the wrong width is refused rather than read, for the runtime record's
    // reason: a prefix of a number is a different number.
    auto const threeBytes = std::array { std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x03 } };
    auto misshapen = exact;
    misshapen[10] = std::span<std::byte const> { threeBytes };
    CHECK_FALSE(reportWith(misshapen).has_value());
}

TEST_CASE("A node runtime record carries the enrollment mode and absent is not manual", "[wire][enrollment][node-status]")
{
    // **Absent is not zero, and here absent is not MANUAL.** A node running no
    // consensus has no window to report on, and a `Manual` there is a reassuring claim
    // about a thing that does not exist -- which is exactly the reading that stops an
    // operator looking.
    auto const silent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(silent.has_value());
    CHECK_FALSE(Unwrap(silent).enrollment.has_value());
    CHECK_FALSE(Unwrap(silent).enrollmentPending.has_value());
    CHECK_FALSE(Unwrap(silent).enrollmentAutoApproveSecondsLeft.has_value());

    NodeRuntimeFields armed {};
    armed.enrollment = WireEnrollmentState::AutoApprove;
    armed.enrollmentPending = 3;
    armed.enrollmentAutoApproveSecondsLeft = 600;
    auto const back = DecodeNodeRuntime(EncodeNodeRuntime(armed));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).enrollment == std::optional<WireEnrollmentState> { WireEnrollmentState::AutoApprove });
    CHECK(Unwrap(back).enrollmentPending == std::optional<std::uint32_t> { 3 });
    CHECK(Unwrap(back).enrollmentAutoApproveSecondsLeft == std::optional<std::uint64_t> { 600 });

    // Zero pending is a READING and not an absence: a window nobody has asked yet says
    // zero, and a node with no window says nothing. Both are asserted, because one
    // optional renders them alike to anybody who only checks the engaged case. And a
    // manual window arms no deadline, so its seconds stay DISENGAGED rather than reading
    // as a deadline that just ran out.
    NodeRuntimeFields quiet {};
    quiet.enrollment = WireEnrollmentState::Manual;
    quiet.enrollmentPending = 0;
    auto const none = DecodeNodeRuntime(EncodeNodeRuntime(quiet));
    REQUIRE(none.has_value());
    CHECK(Unwrap(none).enrollment == std::optional<WireEnrollmentState> { WireEnrollmentState::Manual });
    CHECK(Unwrap(none).enrollmentPending == std::optional<std::uint32_t> { 0 });
    CHECK_FALSE(Unwrap(none).enrollmentAutoApproveSecondsLeft.has_value());

    // A RETIRED state byte is one this build cannot name, so it is skipped -- disengaged, the
    // runtime record's rule for an unnamed byte -- and never read as `Manual` or `AutoApprove`.
    auto const emitted = EncodeNodeRuntime(quiet);
    auto parts = Unwrap(WireFields::SplitAll(emitted));
    for (auto const raw: RetiredEnrollmentStates)
    {
        INFO("state byte " << static_cast<int>(raw));
        auto const retired = std::array { static_cast<std::byte>(raw) };
        parts[10] = std::span<std::byte const> { retired };
        auto const read = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts }));
        REQUIRE(read.has_value());
        CHECK_FALSE(Unwrap(read).enrollment.has_value());
    }
}

TEST_CASE("An older peer's runtime record reads without the enrollment fields, and a newer one's surplus is skipped",
          "[wire][enrollment][node-status]")
{
    // **This is why the two fields cost no wire version.** The nested runtime record is
    // variable-arity by design: a sender that predates these fields is answered with
    // them disengaged, and one carrying more than this build knows is read for what it
    // does know. Both directions, because a decoder tolerant in only one of them still
    // leaves a fleet mid-upgrade unable to speak.
    NodeRuntimeFields older {};
    older.toolchainsServed = 4;
    auto const emitted = EncodeNodeRuntime(older);
    auto const parts = WireFields::SplitAll(emitted);
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() >= 10);

    // Rebuilt with exactly the ten fields a pre-enrollment build emitted.
    auto const truncated = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 10 };
    auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { truncated }));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).toolchainsServed == 4);
    CHECK_FALSE(Unwrap(back).enrollment.has_value());
    CHECK_FALSE(Unwrap(back).enrollmentPending.has_value());

    // And a record from a build with one more field than this one has heard of.
    auto surplus = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
    auto const extra = AsBytes(std::string_view { "a fact from the future" });
    surplus.emplace_back(extra);
    auto const ahead = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { surplus }));
    REQUIRE(ahead.has_value());
    CHECK(Unwrap(ahead).toolchainsServed == 4);
}

TEST_CASE("Enrollment bytes for learners and auto-approve are pinned and the retired ones stay retired",
          "[wire][enrollment][formation]")
{
    CHECK(static_cast<std::uint8_t>(EnrollRole::Learner) == 0x03);
    CHECK(static_cast<std::uint8_t>(EnrollRole::Worker) == 0x02);
    CHECK(static_cast<std::uint8_t>(EnrollControlVerb::AutoApprove) == 0x06);
    CHECK(static_cast<std::uint8_t>(EnrollControlVerb::AutoApproveOff) == 0x07);
    CHECK(static_cast<std::uint8_t>(EnrollControlVerb::Clear) == 0x08);
    CHECK(static_cast<std::uint8_t>(EnrollControlVerb::Approve) == 0x09);
    CHECK(static_cast<std::uint8_t>(WireEnrollmentState::Manual) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireEnrollmentState::AutoApprove) == 0x04);

    // A retired byte is refused by every decoder, never read as its successor.
    for (auto const raw: RetiredEnrollRoles)
        CHECK_FALSE(IsKnownEnrollRole(raw));
    for (auto const raw: RetiredEnrollControlVerbs)
        CHECK_FALSE(IsKnownEnrollControlVerb(raw));
    for (auto const raw: RetiredEnrollmentStates)
        CHECK_FALSE(IsKnownEnrollmentState(raw));
    CHECK(std::ranges::contains(RetiredErrorCodes, std::uint8_t { 0x22 }));

    // The retired bytes, spelled out once: the lists above are what the guards read, so a list
    // that lost a row would pass every loop above vacuously.
    CHECK(std::ranges::contains(RetiredEnrollRoles, std::uint8_t { 0x01 }));
    CHECK(std::ranges::contains(RetiredEnrollControlVerbs, std::uint8_t { 0x01 }));
    CHECK(std::ranges::contains(RetiredEnrollControlVerbs, std::uint8_t { 0x02 }));
    CHECK(std::ranges::contains(RetiredEnrollControlVerbs, std::uint8_t { 0x04 }));
    CHECK(std::ranges::contains(RetiredEnrollmentStates, std::uint8_t { 0x01 }));
    CHECK(std::ranges::contains(RetiredEnrollmentStates, std::uint8_t { 0x02 }));

    // A retired control-verb byte on the wire is refused by the decoder itself, which is what a
    // peer built before the retirement actually sends.
    for (auto const raw: RetiredEnrollControlVerbs)
    {
        INFO("verb byte " << static_cast<int>(raw));
        auto const tag = std::array { static_cast<std::byte>(raw) };
        CHECK_FALSE(DecodeEnrollControlPayload(WireFields::Encode({ std::span<std::byte const> { tag }, {} })).has_value());
    }
}

TEST_CASE("An auto-approve request carries whole seconds and the off verb carries nothing", "[wire][enrollment][formation]")
{
    auto const frame = EncodeEnrollAutoApprove(std::chrono::minutes { 15 });
    auto const decoded = DecodeEnrollControlPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).verb == EnrollControlVerb::AutoApprove);
    CHECK(Unwrap(decoded).duration == std::optional { std::chrono::seconds { 900 } });

    auto const off = EncodeEnrollControl(EnrollControlVerb::AutoApproveOff);
    auto const decodedOff = DecodeEnrollControlPayload(std::span<std::byte const> { off }.subspan(RequestHeaderSize));
    REQUIRE(decodedOff.has_value());
    CHECK_FALSE(Unwrap(decodedOff).duration.has_value());

    // Arity by the table, in both directions: a duration verb with none or with text that is not
    // a count of seconds, and the off verb carrying one.
    auto const refused = [](EnrollControlVerb verb, std::string_view subject) {
        auto const bytes = EncodeEnrollControl(verb, subject);
        return !DecodeEnrollControlPayload(std::span<std::byte const> { bytes }.subspan(RequestHeaderSize)).has_value();
    };
    CHECK(refused(EnrollControlVerb::AutoApprove, ""));
    CHECK(refused(EnrollControlVerb::AutoApprove, "15m"));
    CHECK(refused(EnrollControlVerb::AutoApproveOff, "60"));

    // Neither a sign nor a count too large to be a duration is a count of seconds.
    CHECK(refused(EnrollControlVerb::AutoApprove, "-60"));
    CHECK(refused(EnrollControlVerb::AutoApprove, "+60"));
    CHECK(refused(EnrollControlVerb::AutoApprove, "99999999999999999999"));
    // And the one count `from_chars` accepts that is still not a duration: one past
    // `seconds::max()`. It fits the `uint64_t` it is read into, so only the decoder's own range
    // comparison stands between it and a NEGATIVE duration.
    CHECK(refused(EnrollControlVerb::AutoApprove, "9223372036854775808"));
    CHECK_FALSE(refused(EnrollControlVerb::AutoApprove, "9223372036854775807"));

    // **Zero is NOT the decoder's to refuse**, and neither is a duration above the ceiling:
    // both are the leader's named refusals, so an operator hears which rule rather than *a frame
    // this build cannot read*. The control for the refusals above.
    CHECK_FALSE(refused(EnrollControlVerb::AutoApprove, "0"));
    auto const zero = EncodeEnrollAutoApprove(std::chrono::seconds { 0 });
    auto const decodedZero = DecodeEnrollControlPayload(std::span<std::byte const> { zero }.subspan(RequestHeaderSize));
    REQUIRE(decodedZero.has_value());
    CHECK(Unwrap(decodedZero).duration == std::optional { std::chrono::seconds { 0 } });
}

TEST_CASE("Every live enroll-control verb has one table row", "[wire][enrollment][formation]")
{
    for (auto const verb: { EnrollControlVerb::List,
                            EnrollControlVerb::Approve,
                            EnrollControlVerb::Reject,
                            EnrollControlVerb::AutoApprove,
                            EnrollControlVerb::AutoApproveOff,
                            EnrollControlVerb::Clear })
        CHECK(std::ranges::count(EnrollControlVerbTable, verb, &EnrollControlVerbRow::verb) == 1);
    CHECK(EnrollControlSubjectOf(EnrollControlVerb::Clear) == EnrollSubject::None);
    CHECK(EnrollControlSubjectOf(EnrollControlVerb::Approve) == EnrollSubject::NodeIdAndKey);
    CHECK(EnrollControlSubjectOf(EnrollControlVerb::Reject) == EnrollSubject::NodeId);
    CHECK(EnrollControlSubjectOf(EnrollControlVerb::AutoApprove) == EnrollSubject::Seconds);
    CHECK(EnrollControlSubjectOf(EnrollControlVerb::List) == EnrollSubject::None);
}

TEST_CASE("An enroll request carries the joiner's node endpoint as its second field", "[wire][enrollment][formation]")
{
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { 0x42 });
    auto const nonce = std::array<std::byte, NodeChallengeBytes> {};
    auto const signature = std::array<std::byte, NodeSignatureBytes> {};
    auto const frame = EncodeEnroll(EnrollRequest { .nodeId = "laptop",
                                                    .nodeEndpoint = "laptop.corp.example:6674",
                                                    .role = EnrollRole::Learner,
                                                    .publicKey = key,
                                                    .nonce = nonce,
                                                    .challenge = {},
                                                    .signature = signature });
    auto const view = DecodeEnrollPayload(std::span { frame }.subspan(RequestHeaderSize));
    REQUIRE(view.has_value());
    CHECK(AsStringView(Unwrap(view).nodeEndpoint) == "laptop.corp.example:6674");
    CHECK(Unwrap(view).role == EnrollRole::Learner);
}

TEST_CASE("An enrollment report carries the auto-approve time left and which rows it approved",
          "[wire][enrollment][formation]")
{
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { 0x42 });
    auto const report = EnrollmentReport {
        .state = WireEnrollmentState::AutoApprove,
        .autoApproveSecondsLeft = 540,
        .pending = { EnrollmentPendingEntry { .nodeId = "laptop",
                                              .nodeEndpoint = "laptop:6674",
                                              .peerId = "10.1.2.3",
                                              .firstSeenSecondsAgo = 12,
                                              .attempts = 6,
                                              .claimsChanged = 0,
                                              .decision = EnrollmentDecision::Approved,
                                              .role = EnrollRole::Learner,
                                              .publicKey = key,
                                              .rosterFingerprint = std::nullopt,
                                              .autoApprovedArmedSecondsAgo = 360,
                                              .firstPeerId = "10.1.2.3" } },
    };
    auto const decoded = DecodeEnrollmentReport(EncodeEnrollmentReport(report));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).state == WireEnrollmentState::AutoApprove);
    CHECK(Unwrap(decoded).autoApproveSecondsLeft == 540);
    REQUIRE(Unwrap(decoded).pending.size() == 1);
    CHECK(Unwrap(decoded).pending[0].autoApprovedArmedSecondsAgo == std::optional<std::uint64_t> { 360 });
    CHECK(Unwrap(decoded).pending[0].role == EnrollRole::Learner);
}

// --- Fleet formation --------------------------------------------------------

TEST_CASE("The fleet summary verb occupies its byte and is the formation family", "[wire][formation]")
{
    // The VALUE, not the name: a symbol both ends spell tests only the first fact.
    CHECK(static_cast<std::uint8_t>(Op::FleetSummary) == 0x1F);
    CHECK(std::ranges::count(OpTable, Op::FleetSummary, &OpDescriptor::code) == 1);
    CHECK(static_cast<std::uint8_t>(FleetState::Solitary) == 0x01);
    CHECK(static_cast<std::uint8_t>(FleetState::Established) == 0x02);
    CHECK(static_cast<std::uint8_t>(FleetState::Pending) == 0x03);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::FleetSummary)) == VerbFamily::Formation);

    // A stranger asks this before it has any credential, so it is pre-auth, and its ceiling is one
    // nonce: the one read a stranger sizes must not be the control cap.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::FleetSummary)));
    CHECK(OpPayloadCap(static_cast<std::uint8_t>(Op::FleetSummary), MaxFramePayload) == MaxFleetSummaryPayload);
    CHECK(MaxFleetSummaryPayload < MaxEnrollPayload);
}

namespace
{
/// A key every byte of which is @p fill.
/// @param fill The byte.
/// @return The key.
[[nodiscard]] std::array<std::byte, IdentityPublicKeyBytes> FilledKey(std::uint8_t fill)
{
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { fill });
    return key;
}

/// A summary every field of which the decoder accepts, each a different value.
/// @return The summary.
[[nodiscard]] FleetSummary AcceptedSummary()
{
    return FleetSummary { .clusterId = "c",
                          .state = FleetState::Solitary,
                          .createdAtUnixSeconds = 1'790'000'000,
                          .leaderId = "n-leader",
                          .leaderNodeEndpoint = "desk:6674",
                          .nodeId = "n-speaker",
                          .raftEndpoint = "desk:6680",
                          .members = { "n-leader", "n-speaker" },
                          .memberTotal = 2,
                          .nodeEndpoint = "desk:6675",
                          .leaderKey = FilledKey(0x4C),
                          .pointsAt = {} };
}

/// @p blob's fields with field @p index replaced by @p value, re-framed.
/// @param blob What `EncodeFleetSummaryFields` wrote.
/// @param index Which field.
/// @param value What it holds instead.
/// @return The spliced summary.
[[nodiscard]] std::vector<std::byte> WithField(std::span<std::byte const> blob,
                                               std::size_t index,
                                               std::span<std::byte const> value)
{
    auto const split = WireFields::SplitExactly(blob, FleetSummaryFieldCount);
    REQUIRE(split.has_value());
    auto parts = std::vector<std::span<std::byte const>> { Unwrap(split).begin(), Unwrap(split).end() };
    parts[index] = value;
    return WireFields::Encode(WireFields::FieldList { parts });
}

/// Text of exactly @p bytes that the decoder reads as field @p index's kind: a dialable endpoint
/// for an endpoint field, plain text otherwise.
/// @param bytes Its length.
/// @param endpoint Whether the field is a dialled endpoint.
/// @return The text.
[[nodiscard]] std::string TextOf(std::size_t bytes, bool endpoint)
{
    return endpoint ? std::string(bytes - 2, 'x') + ":1" : std::string(bytes, 'x');
}

/// The field indices the summary's dialled endpoints are written at, found from what the encoder
/// WRITES rather than restated: each endpoint is given a marker and looked for.
/// @return Whether each index is a dialled endpoint.
[[nodiscard]] std::array<bool, FleetSummaryFieldCount> DialledEndpointIndices()
{
    auto marked = AcceptedSummary();
    for (auto const endpoint: FleetSummaryDialledEndpoints)
        marked.*endpoint = "marked-endpoint:1";
    // Named: the split BORROWS from the bytes it splits, so a temporary here is freed before a
    // field is read -- and every comparison then reads rubble and finds no endpoint at all.
    auto const encoded = EncodeFleetSummaryFields(marked);
    auto const split = WireFields::SplitExactly(encoded, FleetSummaryFieldCount);
    REQUIRE(split.has_value());
    auto found = std::array<bool, FleetSummaryFieldCount> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, FleetSummaryFieldCount))
        found[index] = WireFields::AsStringView(Unwrap(split)[index]) == "marked-endpoint:1";
    return found;
}

/// A reply carrying @p summaryFields with a key and a signature of the right widths.
/// @param summaryFields The nested summary.
/// @return The reply payload.
[[nodiscard]] std::vector<std::byte> ReplyOf(std::span<std::byte const> summaryFields)
{
    auto const key = std::array<std::byte, IdentityPublicKeyBytes> {};
    auto const signature = std::array<std::byte, NodeSignatureBytes> {};
    return WireFields::Encode(
        { summaryFields, std::span<std::byte const> { key }, std::span<std::byte const> { signature } });
}

/// @p count distinct member ids, each @p bytes long.
/// @param count How many.
/// @param bytes How long each is.
/// @return The ids.
[[nodiscard]] std::vector<std::string> MemberIds(std::size_t count, std::size_t bytes)
{
    auto ids = std::vector<std::string> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
    {
        auto id = std::format("m{}", index);
        id.resize(bytes, 'x');
        ids.push_back(std::move(id));
    }
    return ids;
}
} // namespace

TEST_CASE("A fleet summary round-trips every field and refuses an unknown state", "[wire][formation]")
{
    auto const summary = FleetSummary { .clusterId = "3f1c9a0e5b7d2c4e6a8f0b1d3e5c7a9b",
                                        .state = FleetState::Established,
                                        .createdAtUnixSeconds = 1'790'000'000,
                                        .leaderId = "n-leader",
                                        .leaderNodeEndpoint = "office-a.corp.example:6674",
                                        .nodeId = "n-speaker",
                                        .raftEndpoint = "",
                                        .members = { "n-speaker", "n-leader", "n-laptop" },
                                        .memberTotal = 7,
                                        .nodeEndpoint = "office-b.corp.example:6674",
                                        .leaderKey = FilledKey(0x51),
                                        .pointsAt = {} };
    auto const blob = EncodeFleetSummaryFields(summary);
    auto const decoded = DecodeFleetSummaryFields(blob, MaxFleetSummaryMembers);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == summary);

    // The state byte is the second field. A byte this build cannot name is refused rather than
    // read as solitary, which would make an established fleet yield to anybody.
    auto const bogus = std::array { std::byte { 0x7F } };
    CHECK_FALSE(DecodeFleetSummaryFields(WithField(blob, 1, bogus), MaxFleetSummaryMembers).has_value());

    // Arity is exact.
    auto const split = WireFields::SplitExactly(blob, FleetSummaryFieldCount);
    REQUIRE(split.has_value());
    CHECK_FALSE(DecodeFleetSummaryFields(WireFields::Encode({ Unwrap(split)[0], Unwrap(split)[1] }), MaxFleetSummaryMembers)
                    .has_value());
}

TEST_CASE("A pending summary's leader slots arrive as its pointer, and its own leader fields stay empty",
          "[wire][formation][pointer]")
{
    // The ONE place a pointer could be taken for a leader is where the slots are read, so that is where
    // the state decides which members they land in (`FleetStateTable`): a reader of `leaderNodeEndpoint`
    // finds nothing to dial under `Pending`, and has nothing to remember to check.
    auto const own = AcceptedSummary();
    auto pointer = own;
    pointer.state = FleetState::Pending;
    pointer.pointsAt = JoinPointer { .leaderId = std::exchange(pointer.leaderId, {}),
                                     .leaderNodeEndpoint = std::exchange(pointer.leaderNodeEndpoint, {}),
                                     .leaderKey = std::exchange(pointer.leaderKey, std::nullopt) };

    // The same slots on the wire: only the state byte tells them apart. Both encodings are named: a
    // split BORROWS from the bytes it splits, so one of a temporary reads freed memory.
    auto const encodedOwn = EncodeFleetSummaryFields(own);
    auto const ownFields = WireFields::SplitExactly(encodedOwn, FleetSummaryFieldCount);
    auto const encodedPointer = EncodeFleetSummaryFields(pointer);
    auto const pointerFields = WireFields::SplitExactly(encodedPointer, FleetSummaryFieldCount);
    REQUIRE(ownFields.has_value());
    REQUIRE(pointerFields.has_value());
    for (auto const index: { std::size_t { 3 }, std::size_t { 4 }, FleetSummaryLeaderKeyField })
        CHECK(std::ranges::equal(Unwrap(ownFields)[index], Unwrap(pointerFields)[index]));

    auto const decoded = DecodeFleetSummaryFields(encodedPointer, MaxFleetSummaryMembers);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == pointer);
    CHECK(Unwrap(decoded).leaderNodeEndpoint.empty());
    CHECK(Unwrap(decoded).leaderId.empty());
    CHECK_FALSE(Unwrap(decoded).leaderKey.has_value());
    CHECK(Unwrap(decoded).pointsAt.leaderNodeEndpoint == own.leaderNodeEndpoint);
    CHECK(Unwrap(decoded).pointsAt.leaderKey == own.leaderKey);

    // And the other way: every state whose slots are its own leaves the pointer empty.
    for (auto const& row: FleetStateTable)
    {
        INFO(static_cast<int>(row.state));
        CHECK(LeaderSlotsNameAskedFleet(row.state) == (row.state == FleetState::Pending));
    }
    auto const decodedOwn = DecodeFleetSummaryFields(EncodeFleetSummaryFields(own), MaxFleetSummaryMembers);
    REQUIRE(decodedOwn.has_value());
    CHECK(Unwrap(decodedOwn).pointsAt == JoinPointer {});
    CHECK(Unwrap(decodedOwn) == own);

    // The pointer is dialled too, so it is held to the one dial rule.
    auto const undialable = WithField(encodedPointer, 4, WireFields::AsBytes(std::string_view { "6674" }));
    CHECK_FALSE(DecodeFleetSummaryFields(undialable, MaxFleetSummaryMembers).has_value());
}

TEST_CASE("A fleet summary's leader key is absent or exactly one key wide", "[wire][formation]")
{
    auto const blob = EncodeFleetSummaryFields(AcceptedSummary());
    auto noKey = AcceptedSummary();
    noKey.leaderKey.reset();
    auto const absent = DecodeFleetSummaryFields(EncodeFleetSummaryFields(noKey), MaxFleetSummaryMembers);
    REQUIRE(absent.has_value());
    CHECK_FALSE(Unwrap(absent).leaderKey.has_value()); // absent is not a key of zeroes

    // One byte short and one byte long are both damage, never a shorter or a longer key.
    for (auto const size: { IdentityPublicKeyBytes - 1, IdentityPublicKeyBytes + 1 })
    {
        INFO(size);
        auto const wrong = std::vector<std::byte>(size, std::byte { 0x4C });
        CHECK_FALSE(DecodeFleetSummaryFields(WithField(blob, FleetSummaryLeaderKeyField, wrong), MaxFleetSummaryMembers)
                        .has_value());
    }
    auto const right = std::vector<std::byte>(IdentityPublicKeyBytes, std::byte { 0x4C });
    CHECK(DecodeFleetSummaryFields(WithField(blob, FleetSummaryLeaderKeyField, right), MaxFleetSummaryMembers).has_value());
}

TEST_CASE("A fleet summary with an empty cluster id is refused and a one-byte id is not", "[wire][formation]")
{
    // The control first: a one-byte id, every other field one the decoder accepts, round-trips.
    auto const summary = AcceptedSummary();
    auto const control = EncodeFleetSummaryFields(summary);
    auto const oneByte = DecodeFleetSummaryFields(control, MaxFleetSummaryMembers);
    REQUIRE(oneByte.has_value());
    CHECK(Unwrap(oneByte) == summary);

    // The same fields with the id emptied, spliced by hand because the encoder refuses to write
    // one. They differ from the control in that field alone, so the ONLY refusal they can meet
    // is the empty id's.
    auto const empty = WithField(control, 0, {});
    REQUIRE(WireFields::SplitExactly(empty, FleetSummaryFieldCount).has_value());
    CHECK_FALSE(DecodeFleetSummaryFields(empty, MaxFleetSummaryMembers).has_value());

    // The reply carries the summary through the same decoder, so it refuses the same bytes; the
    // key and the signature are the right widths, so they are not what it refuses.
    CHECK(DecodeFleetSummaryReply(ReplyOf(control)).has_value());
    CHECK_FALSE(DecodeFleetSummaryReply(ReplyOf(empty)).has_value());
}

TEST_CASE("Every text field of a fleet summary is refused past its own bound, and a cluster id past the tightest",
          "[wire][formation]")
{
    // Walked from the table the decoder reads, so a field added to it is tested here without
    // being named. Each field at its bound decodes and one byte past it does not, in the summary
    // and in the reply that carries one; the rest of the summary is the same both times, so the
    // one refusal the longer bytes can meet is that field's bound. An endpoint field is given text
    // the dial rule reads, so the bound is the only rule it can meet.
    STATIC_REQUIRE(FleetSummaryTextFields[0].index == 0);
    STATIC_REQUIRE(FleetSummaryTextFields[0].maxBytes == MaxIdBytes);
    STATIC_REQUIRE(MaxIdBytes < MaxFleetSummaryTextBytes);

    auto const base = EncodeFleetSummaryFields(AcceptedSummary());
    auto const endpoints = DialledEndpointIndices();
    for (auto const& field: FleetSummaryTextFields)
    {
        INFO("field " << field.index << ", bound " << field.maxBytes);
        auto const withText = [&base, &field, &endpoints](std::size_t bytes) {
            auto const text = TextOf(bytes, endpoints[field.index]);
            return WithField(base, field.index, WireFields::AsBytes(text));
        };
        auto const atBound = withText(field.maxBytes);
        auto const pastBound = withText(field.maxBytes + 1);
        CHECK(DecodeFleetSummaryFields(atBound, MaxFleetSummaryMembers).has_value());
        CHECK_FALSE(DecodeFleetSummaryFields(pastBound, MaxFleetSummaryMembers).has_value());
        CHECK(DecodeFleetSummaryReply(ReplyOf(atBound)).has_value());
        CHECK_FALSE(DecodeFleetSummaryReply(ReplyOf(pastBound)).has_value());
    }
}

TEST_CASE("Every id a fleet summary names is held to the one id bound, and no endpoint is", "[wire][formation]")
{
    // Named by the summary's MEMBER rather than by a field index, so a table row pointing an id at
    // the wider text bound is caught here even though the walk above still agrees with the table.
    struct Row
    {
        std::string_view what;
        std::string FleetSummary::* member;
        bool isId;
    };
    auto const rows = std::array {
        Row { .what = "cluster id", .member = &FleetSummary::clusterId, .isId = true },
        Row { .what = "leader id", .member = &FleetSummary::leaderId, .isId = true },
        Row { .what = "node id", .member = &FleetSummary::nodeId, .isId = true },
        Row { .what = "leader endpoint", .member = &FleetSummary::leaderNodeEndpoint, .isId = false },
        Row { .what = "raft endpoint", .member = &FleetSummary::raftEndpoint, .isId = false },
        Row { .what = "node endpoint", .member = &FleetSummary::nodeEndpoint, .isId = false },
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        auto const naming = [&row](std::size_t bytes) {
            auto summary = AcceptedSummary();
            summary.*row.member = TextOf(bytes, !row.isId);
            return EncodeFleetSummaryFields(summary);
        };
        CHECK(DecodeFleetSummaryFields(naming(MaxIdBytes), MaxFleetSummaryMembers).has_value());
        CHECK(DecodeFleetSummaryFields(naming(MaxIdBytes + 1), MaxFleetSummaryMembers).has_value() == !row.isId);
    }
}

TEST_CASE("A fleet summary request carries exactly one nonce", "[wire][formation]")
{
    auto nonce = std::array<std::byte, NodeChallengeBytes> {};
    nonce.fill(std::byte { 0x5A });
    auto const frame = EncodeFleetSummaryRequest(nonce);
    auto const decoded = DecodeFleetSummaryRequestPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == nonce);

    auto const shortNonce = std::array<std::byte, NodeChallengeBytes - 1> {};
    CHECK_FALSE(
        DecodeFleetSummaryRequestPayload(WireFields::Encode({ std::span<std::byte const> { shortNonce } })).has_value());
}

TEST_CASE("A fleet summary reply round-trips and refuses a short signature", "[wire][formation]")
{
    auto reply = FleetSummaryReply { .summary = FleetSummary { .clusterId = "c1", .nodeId = "n1" } };
    reply.publicKey.fill(std::byte { 0x11 });
    reply.signature.fill(std::byte { 0x22 });
    auto const payload = EncodeFleetSummaryReply(reply);
    auto const decoded = DecodeFleetSummaryReply(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == reply);

    auto const split = WireFields::SplitExactly(payload, 3);
    REQUIRE(split.has_value());
    auto const& parts = Unwrap(split);
    auto const cut = parts[2].first(NodeSignatureBytes - 1);
    CHECK_FALSE(DecodeFleetSummaryReply(WireFields::Encode({ parts[0], parts[1], cut })).has_value());
}

TEST_CASE("The fleet summary's member fields sit where they are pinned, with the bounds they are pinned to",
          "[wire][formation]")
{
    // Names AND values: a symbol both ends spell tests only the first fact. The datagram cap is the
    // measured one (`DiscoveryWire_test.cpp` measures it); the reply cap is tied to the reply ceiling
    // by the assert beside `LargestFleetSummaryReply`, repeated here so a change to either is a red
    // case as well as a red build.
    STATIC_REQUIRE(FleetSummaryFieldCount == 11);
    STATIC_REQUIRE(FleetSummaryMembersField == 7);
    STATIC_REQUIRE(FleetSummaryMemberTotalField == 8);
    STATIC_REQUIRE(FleetSummaryLeaderKeyField == 10);
    STATIC_REQUIRE(MaxFleetSummaryMembers == 10);
    STATIC_REQUIRE(MaxFleetSummaryReplyMembers == 512);
    STATIC_REQUIRE(LargestFleetSummaryReply(MaxFleetSummaryReplyMembers) <= MaxFleetSummaryReply);
}

TEST_CASE("A two-member fleet summary encodes to exactly these bytes", "[wire][formation]")
{
    // A golden encoding, written out byte by byte rather than re-derived through any encoder, so
    // the ORDER of the eleven fields and the width of each is pinned: a transposed field or a total
    // written in four bytes changes these bytes, whatever both ends agree on.
    auto const summary = FleetSummary { .clusterId = "c",
                                        .state = FleetState::Established,
                                        .createdAtUnixSeconds = 0x0102030405060708ULL,
                                        .leaderId = "L",
                                        .leaderNodeEndpoint = "h:1",
                                        .nodeId = "n",
                                        .raftEndpoint = "",
                                        .members = { "a", "b" },
                                        .memberTotal = 3,
                                        .nodeEndpoint = "h:2" };
    auto const golden = [] {
        auto const values = std::to_array<std::uint8_t>({
            0, 0, 0, 1,  'c',                                     // cluster id
            0, 0, 0, 1,  0x02,                                    // state: Established
            0, 0, 0, 8,  1,    2,   3,   4, 5,   6, 7, 8,         // created, big-endian
            0, 0, 0, 1,  'L',                                     // leader id
            0, 0, 0, 3,  'h',  ':', '1',                          // leader's 0xFC endpoint
            0, 0, 0, 1,  'n',                                     // speaker id
            0, 0, 0, 0,                                           // speaker's Raft endpoint: none
            0, 0, 0, 10, 0,    0,   0,   1, 'a', 0, 0, 0, 1, 'b', // members, nested
            0, 0, 0, 8,  0,    0,   0,   0, 0,   0, 0, 3,         // member total, big-endian
            0, 0, 0, 3,  'h',  ':', '2',                          // speaker's own 0xFC endpoint
            0, 0, 0, 0,                                           // leader's key: none stated
        });
        auto bytes = std::vector<std::byte> {};
        for (auto const value: values)
            bytes.push_back(std::byte { value });
        return bytes;
    }();
    CHECK(EncodeFleetSummaryFields(summary) == golden);
    auto const decoded = DecodeFleetSummaryFields(golden, MaxFleetSummaryMembers);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == summary);
}

TEST_CASE("A fleet summary's member list is refused as malformed for each way it can be wrong", "[wire][formation]")
{
    // One row per refusal, each beside the nearest list that decodes, so the refusal a row meets can
    // only be its own. The list is text a peer sent -- a claim, rendered on a fleet page -- so it is
    // held to what an id is anywhere else.
    struct Row
    {
        std::string_view what;
        std::vector<std::string> refused;
        std::vector<std::string> accepted;
    };
    auto const rows = std::array {
        Row { .what = "an empty id", .refused = { "a", "" }, .accepted = { "a", "b" } },
        Row { .what = "an id past the id bound",
              .refused = { std::string(MaxIdBytes + 1, 'x') },
              .accepted = { std::string(MaxIdBytes, 'x') } },
        Row { .what = "an id that is not UTF-8", .refused = { "n-\xC3" }, .accepted = { "n-\xC3\xA9" } },
        Row { .what = "an id named twice", .refused = { "a", "b", "a" }, .accepted = { "a", "b", "c" } },
        Row { .what = "more ids than a datagram carries",
              .refused = MemberIds(MaxFleetSummaryMembers + 1, 8),
              .accepted = MemberIds(MaxFleetSummaryMembers, 8) },
    };
    auto const listing = [](std::vector<std::string> const& members) {
        auto summary = AcceptedSummary();
        summary.members = members;
        summary.memberTotal = members.size();
        return EncodeFleetSummaryFields(summary);
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        CHECK(DecodeFleetSummaryFields(listing(row.accepted), MaxFleetSummaryMembers).has_value());
        CHECK_FALSE(DecodeFleetSummaryFields(listing(row.refused), MaxFleetSummaryMembers).has_value());
    }

    // A total below the list, spliced by hand because the encoder refuses to write one: a cut list
    // names fewer than the fleet records, never more. The same total as the list is the control.
    auto const two = listing({ "a", "b" });
    auto const one = WireFields::ToBigEndian<std::uint64_t>(1);
    auto const exact = WireFields::ToBigEndian<std::uint64_t>(2);
    CHECK(DecodeFleetSummaryFields(WithField(two, FleetSummaryMemberTotalField, exact), MaxFleetSummaryMembers).has_value());
    CHECK_FALSE(
        DecodeFleetSummaryFields(WithField(two, FleetSummaryMemberTotalField, one), MaxFleetSummaryMembers).has_value());
}

TEST_CASE("The member cap is the carrier's: a reply reads the list a datagram may not, up to its own cap",
          "[wire][formation]")
{
    auto const listing = [](std::size_t count) {
        auto summary = AcceptedSummary();
        summary.members = MemberIds(count, MaxIdBytes);
        summary.memberTotal = count;
        return EncodeFleetSummaryFields(summary);
    };
    // One past the datagram's cap: refused there, read by a reply.
    auto const past = listing(MaxFleetSummaryMembers + 1);
    CHECK_FALSE(DecodeFleetSummaryFields(past, MaxFleetSummaryMembers).has_value());
    CHECK(DecodeFleetSummaryFields(past, MaxFleetSummaryReplyMembers).has_value());
    CHECK(DecodeFleetSummaryReply(ReplyOf(past)).has_value());

    // The reply's own cap, and one past it.
    CHECK(DecodeFleetSummaryReply(ReplyOf(listing(MaxFleetSummaryReplyMembers))).has_value());
    CHECK_FALSE(DecodeFleetSummaryReply(ReplyOf(listing(MaxFleetSummaryReplyMembers + 1))).has_value());
}

TEST_CASE("Every endpoint a fleet summary names is held to the one dial rule, and empty states none", "[wire][formation]")
{
    // One row per field, named by MEMBER rather than taken from `FleetSummaryDialledEndpoints`, so a
    // field dropped from that list is caught here: its row goes red and the other two stay green.
    struct Row
    {
        std::string_view what;
        std::string FleetSummary::* member;
    };
    auto const rows = std::array {
        Row { .what = "leader endpoint", .member = &FleetSummary::leaderNodeEndpoint },
        Row { .what = "raft endpoint", .member = &FleetSummary::raftEndpoint },
        Row { .what = "node endpoint", .member = &FleetSummary::nodeEndpoint },
    };
    // Each has already been a bug somewhere (`ParseDialEndpoint`): a sentence that merely splits, an
    // empty host, a bare port.
    auto const malformed = std::array<std::string_view, 3> { "no leader: try again", ":6674", "6675" };
    for (auto const& row: rows)
    {
        INFO(row.what);
        auto const with = [&row](std::string_view endpoint) {
            auto summary = AcceptedSummary();
            summary.*row.member = std::string { endpoint };
            return EncodeFleetSummaryFields(summary);
        };
        CHECK(DecodeFleetSummaryFields(with(""), MaxFleetSummaryMembers).has_value());
        CHECK(DecodeFleetSummaryFields(with("office:6674"), MaxFleetSummaryMembers).has_value());
        for (auto const text: malformed)
        {
            INFO(text);
            CHECK_FALSE(DecodeFleetSummaryFields(with(text), MaxFleetSummaryMembers).has_value());
        }
    }
}

TEST_CASE("A carrier cuts a member list to its cap, keeping the first ids and the total", "[wire][formation]")
{
    auto summary = AcceptedSummary();
    summary.members = MemberIds(20, 8);
    summary.memberTotal = 20;

    auto const cut = WithMembersAtMost(summary, MaxFleetSummaryMembers);
    REQUIRE(cut.members.size() == MaxFleetSummaryMembers);
    CHECK(std::ranges::equal(cut.members, summary.members | std::views::take(MaxFleetSummaryMembers)));
    CHECK(cut.memberTotal == 20); // a reader can tell it was cut

    // A list inside the cap is left whole.
    CHECK(WithMembersAtMost(summary, MaxFleetSummaryReplyMembers) == summary);
}

TEST_CASE("The largest reply a peer can make is exactly the size the ceiling is asserted against", "[wire][formation]")
{
    // Built, not reasoned: every text field at its bound, every member a reply carries at the id
    // bound. It decodes, and its size is `LargestFleetSummaryReply`'s -- so the static_assert that
    // ties the reply cap to `MaxFleetSummaryReply` is about a summary that exists.
    auto const endpoints = DialledEndpointIndices();
    auto summary = AcceptedSummary();
    summary.members = MemberIds(MaxFleetSummaryReplyMembers, MaxIdBytes);
    summary.memberTotal = MaxFleetSummaryReplyMembers;
    auto blob = EncodeFleetSummaryFields(summary);
    for (auto const& field: FleetSummaryTextFields)
    {
        auto const text = TextOf(field.maxBytes, endpoints[field.index]);
        blob = WithField(blob, field.index, WireFields::AsBytes(text));
    }
    auto const payload = ReplyOf(blob);
    CHECK(payload.size() == LargestFleetSummaryReply(MaxFleetSummaryReplyMembers));
    CHECK(DecodeFleetSummaryReply(payload).has_value());
}

// --- Live stats (#1399) -----------------------------------------------------

TEST_CASE("Only SUBSCRIBE may be answered with a push frame", "[wire]")
{
    // `PushIsSubscribeOnly` is the compile-time half; this is what fails when a row's mask is
    // widened by hand, the same pair `Progress` has.
    for (auto const& row: OpTable)
    {
        INFO("verb " << row.name);
        auto const pushes = (row.legalStatuses & StatusBit(Status::Push)) != 0;
        CHECK(pushes == (row.code == Op::Subscribe));
    }
    CHECK_FALSE(IsTerminalStatus(Status::Push));
    CHECK(IsLegalStatus(Op::Subscribe, Status::Ok));
    CHECK(IsLegalStatus(Op::Subscribe, Status::Error));
}

TEST_CASE("The live-stats wire bytes are pinned", "[wire]")
{
    // The BYTE, not the symbol, for the reason the constants case above gives: a peer built from
    // another revision of this header agrees about the values and nothing else.
    CHECK(static_cast<std::uint8_t>(LiveSubject::Cache) == 0x00);
    CHECK(static_cast<std::uint8_t>(LiveSubject::Node) == 0x01);
    CHECK(static_cast<std::uint8_t>(LiveSubject::Fleet) == 0x02);
    CHECK(static_cast<std::uint8_t>(PushKind::Subscribed) == 0x00);
    CHECK(static_cast<std::uint8_t>(PushKind::Snapshot) == 0x01);
    CHECK(static_cast<std::uint8_t>(PushKind::Event) == 0x02);
    CHECK(static_cast<std::uint8_t>(PushKind::Gap) == 0x03);
    CHECK(static_cast<std::uint8_t>(LiveEventKind::MemberJoined) == 0x00);
    CHECK(static_cast<std::uint8_t>(LiveEventKind::EnrollmentChanged) == 0x06);
}

TEST_CASE("A SUBSCRIBE request round-trips and refuses a subject this build does not know", "[wire]")
{
    auto const request = SubscribeRequest { .subject = LiveSubject::Fleet, .cadenceMillis = 1500, .dashboardToken = "t0k" };
    auto const frame = EncodeSubscribeRequest(request);
    REQUIRE(frame.size() > RequestHeaderSize);
    CHECK(frame[2] == static_cast<std::byte>(Op::Subscribe));

    auto const payload = std::span { frame }.subspan(RequestHeaderSize);
    auto const decoded = DecodeSubscribeRequest(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).subject == LiveSubject::Fleet);
    CHECK(Unwrap(decoded).cadenceMillis == 1500);
    CHECK(Unwrap(decoded).dashboardToken == "t0k");

    // The subject byte sits right after its field's length prefix.
    auto unknown = std::vector<std::byte> { payload.begin(), payload.end() };
    unknown[WireFields::FieldPrefixSize] = std::byte { 0x03 };
    CHECK_FALSE(DecodeSubscribeRequest(unknown).has_value());
}

TEST_CASE("A granted cadence is the request clamped to the subject floor and the ceiling", "[wire]")
{
    CHECK(GrantLiveCadence(LiveSubject::Node, 0) == std::chrono::milliseconds { 500 });
    CHECK(GrantLiveCadence(LiveSubject::Fleet, 200) == std::chrono::milliseconds { 1000 });
    CHECK(GrantLiveCadence(LiveSubject::Cache, 2000) == std::chrono::milliseconds { 2000 });
    CHECK(GrantLiveCadence(LiveSubject::Node, 3'600'000) == MaxLiveCadence);
    CHECK(LiveIdleBound(std::chrono::milliseconds { 500 }) == std::chrono::milliseconds { 1500 });
}

TEST_CASE("Every push kind round-trips through its own decoder", "[wire]")
{
    auto const subscribed = EncodeLiveSubscribed(LiveSubscribedFields { .subject = LiveSubject::Node,
                                                                        .grantedCadenceMillis = 500,
                                                                        .statsLayout = 0x1122334455667788ULL,
                                                                        .endpoint = "n1:6674" });
    auto const view = DecodePush(subscribed);
    REQUIRE(view.has_value());
    CHECK(Unwrap(view).kind == PushKind::Subscribed);
    auto const granted = DecodeLiveSubscribed(Unwrap(view).fields);
    REQUIRE(granted.has_value());
    CHECK(Unwrap(granted).grantedCadenceMillis == 500);
    CHECK(Unwrap(granted).statsLayout == 0x1122334455667788ULL);
    CHECK(Unwrap(granted).endpoint == "n1:6674");

    auto const body = std::vector<std::byte> { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };
    auto const snapshot = EncodeLiveSnapshot(42, body);
    auto const snapshotView = DecodePush(snapshot);
    REQUIRE(snapshotView.has_value());
    CHECK(Unwrap(snapshotView).kind == PushKind::Snapshot);
    auto const reading = DecodeLiveSnapshot(Unwrap(snapshotView).fields);
    REQUIRE(reading.has_value());
    CHECK(Unwrap(reading).tick == 42);
    CHECK(std::ranges::equal(Unwrap(reading).body, body));

    auto const event = EncodeLiveEvent(LiveEventFields { .kind = LiveEventKind::WorkerLeft, .detail = "build-07" });
    auto const eventView = DecodePush(event);
    REQUIRE(eventView.has_value());
    auto const changed = DecodeLiveEvent(Unwrap(eventView).fields);
    REQUIRE(changed.has_value());
    CHECK(Unwrap(changed).kind == LiveEventKind::WorkerLeft);
    CHECK(Unwrap(changed).detail == "build-07");

    auto const gap = EncodeLiveGap(LiveGapFields { .dropped = 3, .firstTick = 10, .lastTick = 12 });
    auto const gapView = DecodePush(gap);
    REQUIRE(gapView.has_value());
    auto const dropped = DecodeLiveGap(Unwrap(gapView).fields);
    REQUIRE(dropped.has_value());
    CHECK(Unwrap(dropped).dropped == 3);
    CHECK(Unwrap(dropped).lastTick == 12);

    // A kind byte past the last one this build knows is refused, never guessed at.
    auto unknown = gap;
    unknown[0] = std::byte { 0x04 };
    CHECK_FALSE(DecodePush(unknown).has_value());
}

// --- The cordon (#1303) -----------------------------------------------------

TEST_CASE("The cordon verb occupies the byte it was assigned, in the compile family", "[wire][cordon]")
{
    // The value as well as the name, for the reason the enrollment bytes are pinned: a
    // consistent renumbering keeps every in-tree test agreeing while a deployed CLI breaks.
    CHECK(static_cast<std::uint8_t>(Op::Cordon) == 0x13);
    CHECK(std::ranges::count(OpTable, Op::Cordon, &OpDescriptor::code) == 1);

    // The compile family, because what a cordon governs is that family's admission -- so
    // a node running no worker answers it the family's not-served sentence.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Cordon)) == VerbFamily::Compile);
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::Cordon)));

    // And the three states are the bytes a CLI of another build reads.
    CHECK(static_cast<std::uint8_t>(WireCordonState::Serving) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireCordonState::Draining) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireCordonState::Drained) == 0x03);
    CHECK(static_cast<std::uint8_t>(CordonAction::Lift) == 0x00);
    CHECK(static_cast<std::uint8_t>(CordonAction::Cordon) == 0x01);
}

TEST_CASE("A cordon request carries one action byte, and anything else is refused", "[wire][cordon]")
{
    for (auto const action: { CordonAction::Cordon, CordonAction::Lift })
    {
        auto const frame = EncodeCordonRequest(action);
        auto const header = DecodeRequestHeader(frame);
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).opRaw == 0x13);
        auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
        CHECK(DecodeCordonPayload(payload) == std::optional { action });
    }

    // A byte naming no action is refused rather than read as either: a cordon a client
    // did not ask for is a machine silently out of the fleet, and a lift it did not ask
    // for is one silently back in.
    auto const unnamed = std::array { std::byte { 0x02 } };
    CHECK_FALSE(DecodeCordonPayload(WireFields::Encode({ std::span<std::byte const> { unnamed } })).has_value());
    auto const wide = std::array { std::byte { 0x00 }, std::byte { 0x01 } };
    CHECK_FALSE(DecodeCordonPayload(WireFields::Encode({ std::span<std::byte const> { wide } })).has_value());
    CHECK_FALSE(DecodeCordonPayload({}).has_value());
}

TEST_CASE("A cordon reply round-trips every state and its count, and refuses a state it cannot name", "[wire][cordon]")
{
    // A different count per state, so an encoder that dropped either half cannot agree
    // with all three.
    for (auto const& sent: { CordonFields { .state = WireCordonState::Serving, .inFlight = 5 },
                             CordonFields { .state = WireCordonState::Draining, .inFlight = 2 },
                             CordonFields { .state = WireCordonState::Drained, .inFlight = 0 } })
    {
        auto const back = DecodeCordonFields(EncodeCordonFields(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).state == sent.state);
        CHECK(Unwrap(back).inFlight == sent.inFlight);
    }

    auto const unnamed = std::array { std::byte { 0x7F } };
    auto const count = WireFields::ToBigEndian<std::uint32_t>(1);
    CHECK_FALSE(DecodeCordonFields(
                    WireFields::Encode({ std::span<std::byte const> { unnamed }, std::span<std::byte const> { count } }))
                    .has_value());
    CHECK_FALSE(DecodeCordonFields({}).has_value());
}

TEST_CASE("A heartbeat carries the cordon, and a record without it is a serving worker", "[wire][cordon]")
{
    LoadFields cordoned {};
    cordoned.cordoned = true;
    auto const back = DecodeLoad(EncodeLoad(cordoned));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).cordoned);

    // Serving travels as an EMPTY field, and a record that stops before it -- five fields,
    // what a build without the cordon emits -- decodes as serving too.
    auto const serving = DecodeLoad(EncodeLoad(LoadFields {}));
    REQUIRE(serving.has_value());
    CHECK_FALSE(Unwrap(serving).cordoned);

    // The encoding is kept in a local: `SplitAll` hands back spans INTO it. Eight since the
    // interface addresses were appended after #1364's conditions list; the cuts below are still
    // counted from the cordon's position.
    auto const encoded = EncodeLoad(cordoned);
    auto const parts = WireFields::SplitAll(encoded);
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() == 8);
    auto const five = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 5 };
    auto const shorter = DecodeLoad(WireFields::Encode(WireFields::FieldList { five }));
    REQUIRE(shorter.has_value());
    CHECK_FALSE(Unwrap(shorter).cordoned);

    // Any byte but the one the encoder writes is a shape this build does not know, and is
    // refused rather than read as either answer.
    auto odd = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 5 };
    auto const two = std::array { std::byte { 0x02 } };
    odd.emplace_back(two);
    CHECK_FALSE(DecodeLoad(WireFields::Encode(WireFields::FieldList { odd })).has_value());
}

TEST_CASE("A LEASE grant carries a dial hint beside the endpoint the token signs", "[wire][lease][dialhint]")
{
    auto const payload = EncodeLeaseGrant(LeaseGrant { .endpoint = "laptop.corp:6676",
                                                       .leaseToken = "t",
                                                       .workerCodecs = {},
                                                       .lifetime = std::chrono::milliseconds { 0 },
                                                       .dialHint = "10.8.0.7:6676" });
    auto const grant = DecodeLeaseGrant(payload);
    REQUIRE(grant.has_value());
    CHECK(AsStringView(Unwrap(grant).endpoint) == "laptop.corp:6676");
    CHECK(AsStringView(Unwrap(grant).dialHint) == "10.8.0.7:6676");

    // The BYTES are pinned with the version bump, over the final field order.

    // No hint is an EMPTY fifth field; a four-field reply is a peer this build cannot read,
    // and so is a six-field one -- the split is exact in both directions.
    auto const bare =
        DecodeLeaseGrant(EncodeLeaseGrant(LeaseGrant { .endpoint = "w:1", .leaseToken = "t", .workerCodecs = {} }));
    REQUIRE(bare.has_value());
    CHECK(Unwrap(bare).dialHint.empty());
    auto const zero = EncodeU32Field(0);
    auto const fourFields = WireFields::Encode(
        { AsBytes("w:1"), AsBytes("t"), std::span<std::byte const> {}, std::span<std::byte const> { zero } });
    CHECK_FALSE(DecodeLeaseGrant(fourFields).has_value());
    auto const sixFields = WireFields::Encode({ AsBytes("w:1"),
                                                AsBytes("t"),
                                                std::span<std::byte const> {},
                                                std::span<std::byte const> { zero },
                                                AsBytes("10.8.0.7:6676"),
                                                AsBytes("extra") });
    CHECK_FALSE(DecodeLeaseGrant(sixFields).has_value());
}

TEST_CASE("A worker's interface addresses ride both REGISTER and HEARTBEAT, bounded", "[wire][dialhint]")
{
    std::vector<std::string> const addresses { "10.8.0.7", "fe80::1" };

    CapacityFields capacity {};
    capacity.interfaceAddresses = addresses;
    auto const capacityBack = DecodeCapacity(EncodeCapacity(capacity));
    REQUIRE(capacityBack.has_value());
    CHECK(Unwrap(capacityBack).interfaceAddresses == addresses);

    LoadFields load {};
    load.interfaceAddresses = addresses;
    auto const loadBack = DecodeLoad(EncodeLoad(load));
    REQUIRE(loadBack.has_value());
    CHECK(Unwrap(loadBack).interfaceAddresses == addresses);

    // A record from a peer that says nothing about them decodes as an empty list.
    CHECK(Unwrap(DecodeLoad(EncodeLoad(LoadFields {}))).interfaceAddresses.empty());
    CHECK(Unwrap(DecodeCapacity(EncodeCapacity(CapacityFields {}))).interfaceAddresses.empty());

    // At the caps is accepted, so the refusals below are the caps and not something coarser.
    std::string const longest(MaxInterfaceAddressBytes, '1');
    std::vector<std::span<std::byte const>> const full(MaxInterfaceAddresses, AsBytes(longest));
    auto const atCaps = DecodeAddressList(WireFields::Encode(WireFields::FieldList { full }));
    REQUIRE(atCaps.has_value());
    CHECK(Unwrap(atCaps).size() == MaxInterfaceAddresses);

    // More than the cap, an empty entry or an over-long one is refused with the record.
    std::vector<std::span<std::byte const>> const over(MaxInterfaceAddresses + 1, AsBytes("10.0.0.1"));
    CHECK_FALSE(DecodeAddressList(WireFields::Encode(WireFields::FieldList { over })).has_value());
    std::vector<std::span<std::byte const>> const empty { std::span<std::byte const> {} };
    CHECK_FALSE(DecodeAddressList(WireFields::Encode(WireFields::FieldList { empty })).has_value());
    std::string const tooLong(MaxInterfaceAddressBytes + 1, '1');
    std::vector<std::span<std::byte const>> const longOne { AsBytes(tooLong) };
    CHECK_FALSE(DecodeAddressList(WireFields::Encode(WireFields::FieldList { longOne })).has_value());

    // A malformed list refuses the RECORD that carries it, not only the list.
    auto const encodedOver = WireFields::Encode(WireFields::FieldList { over });
    std::vector<std::span<std::byte const>> capacityParts(9, std::span<std::byte const> {});
    capacityParts.emplace_back(encodedOver);
    CHECK_FALSE(DecodeCapacity(WireFields::Encode(WireFields::FieldList { capacityParts })).has_value());
    std::vector<std::span<std::byte const>> loadParts(7, std::span<std::byte const> {});
    loadParts.emplace_back(encodedOver);
    CHECK_FALSE(DecodeLoad(WireFields::Encode(WireFields::FieldList { loadParts })).has_value());

    // The encoder takes the FIRST `MaxInterfaceAddresses` rather than producing a list its
    // own decoder refuses.
    std::vector<std::string> many;
    many.reserve(MaxInterfaceAddresses + 1);
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxInterfaceAddresses + 1))
        many.push_back(std::format("10.0.0.{}", index));
    auto const taken = DecodeAddressList(EncodeAddressList(many));
    REQUIRE(taken.has_value());
    REQUIRE(Unwrap(taken).size() == MaxInterfaceAddresses);
    CHECK(Unwrap(taken).front() == "10.0.0.0");
    CHECK(Unwrap(taken).back() == std::format("10.0.0.{}", MaxInterfaceAddresses - 1));
}

TEST_CASE("An interface address the wire cannot carry is skipped, and its record still decodes", "[wire][dialhint]")
{
    // The decoder refuses the WHOLE record over one bad entry, so an encoder that sent one
    // would make a worker vanish from the fleet over an odd adapter name. The encoder skips
    // what `IsCarriedInterfaceAddress` rejects, and the good entries keep their order.
    std::string const tooLong(MaxInterfaceAddressBytes + 1, '1');
    std::vector<std::string> const mixed { "10.8.0.7", tooLong, "", "fe80::1" };
    std::vector<std::string> const carried { "10.8.0.7", "fe80::1" };

    CapacityFields capacity {};
    capacity.interfaceAddresses = mixed;
    auto const capacityBack = DecodeCapacity(EncodeCapacity(capacity));
    REQUIRE(capacityBack.has_value());
    CHECK(Unwrap(capacityBack).interfaceAddresses == carried);

    LoadFields load {};
    load.interfaceAddresses = mixed;
    auto const loadBack = DecodeLoad(EncodeLoad(load));
    REQUIRE(loadBack.has_value());
    CHECK(Unwrap(loadBack).interfaceAddresses == carried);

    // The count cap applies to what SURVIVES the skip: two rejected entries ahead of
    // `MaxInterfaceAddresses` good ones still leave every good one carried.
    std::vector<std::string> many { "", tooLong };
    many.reserve(MaxInterfaceAddresses + 2);
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxInterfaceAddresses))
        many.push_back(std::format("10.0.1.{}", index));
    auto const taken = DecodeAddressList(EncodeAddressList(many));
    REQUIRE(taken.has_value());
    REQUIRE(Unwrap(taken).size() == MaxInterfaceAddresses);
    CHECK(Unwrap(taken).front() == "10.0.1.0");
    CHECK(Unwrap(taken).back() == std::format("10.0.1.{}", MaxInterfaceAddresses - 1));
}

TEST_CASE("A worker's decoded interface addresses outlive the record they were read from", "[wire][dialhint]")
{
    // Both records are returned BY VALUE, so a list of views would be a use-after-free the
    // moment the encoding died. The ARRANGEMENT is what bites, for the reason the CLUSTER-ADMIT
    // receipt's case gives: STORED, the source dropped, the freed storage churned, and only
    // then read. The LENGTH is not what bites: a view would point into the encoding's
    // heap-allocated byte vector, not into a `std::string`'s inline buffer, so it dangles into
    // freed heap at any length. A view-typed copy was measured red under MSVC /MDd at 8 and at
    // 29 bytes -- the Debug CRT fills a freed block with 0xDD -- and under ASan as a
    // heap-use-after-free. The scoped literal is here because it is a realistic address.
    constexpr std::string_view Scoped = "fe80::1ff:fe23:4567:890a%eth0";
    static_assert(Scoped.size() <= MaxInterfaceAddressBytes, "the address must be one the decoder accepts");

    auto capacity = std::optional<CapacityFields> {};
    auto load = std::optional<LoadFields> {};
    {
        std::vector<std::string> const sent { std::string { Scoped }, "10.8.0.7" };
        CapacityFields capacitySent {};
        capacitySent.interfaceAddresses = sent;
        LoadFields loadSent {};
        loadSent.interfaceAddresses = sent;
        auto const capacityBytes = EncodeCapacity(capacitySent);
        auto const loadBytes = EncodeLoad(loadSent);
        capacity = DecodeCapacity(capacityBytes);
        load = DecodeLoad(loadBytes);
    }

    // Churn whatever the encodings' allocations have become; the assertion keeps an optimiser
    // from eliding the allocations that do the churning.
    auto const churn = std::vector<std::string>(64, std::string(128, 'x'));
    CHECK(churn.size() == 64);

    REQUIRE(capacity.has_value());
    REQUIRE(load.has_value());
    REQUIRE(Unwrap(capacity).interfaceAddresses.size() == 2);
    REQUIRE(Unwrap(load).interfaceAddresses.size() == 2);
    CHECK(Unwrap(capacity).interfaceAddresses.front() == Scoped);
    CHECK(Unwrap(load).interfaceAddresses.front() == Scoped);
}

TEST_CASE("A node runtime record carries the cordon state, and absent is not serving", "[wire][cordon][node-status]")
{
    // Absent on a node that runs no worker, for the enrollment window's reason: `Serving`
    // there would be a reassuring answer about a worker that does not exist.
    auto const silent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(silent.has_value());
    CHECK_FALSE(Unwrap(silent).cordon.has_value());

    for (auto const state: { WireCordonState::Serving, WireCordonState::Draining, WireCordonState::Drained })
    {
        NodeRuntimeFields sent {};
        sent.cordon = state;
        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).cordon == std::optional { state });
    }
}

// --- The fleet document, read once (#1391) -----------------------------------

TEST_CASE("The fleet-text wire bytes are pinned and the verb is a request with a reply", "[wire]")
{
    // The BYTE, not the symbol, for the reason the constants case above gives.
    CHECK(static_cast<std::uint8_t>(Op::FleetText) == 0x14);
    CHECK(static_cast<std::uint8_t>(ErrorCode::UnknownFleetSelector) == 0x25);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::FleetText)) == VerbFamily::Fleet);

    // `Ok | Error` and nothing else, which is why no version moved: an older node answers the
    // opcode `UnknownOpcode`, and no reader meets a status it does not know.
    CHECK(IsLegalStatus(Op::FleetText, Status::Ok));
    CHECK(IsLegalStatus(Op::FleetText, Status::Error));
    CHECK_FALSE(IsLegalStatus(Op::FleetText, Status::Push));
    CHECK_FALSE(IsLegalStatus(Op::FleetText, Status::Progress));
    CHECK_FALSE(IsLegalStatus(Op::FleetText, Status::Miss));

    // The fleet map is behind a credential, so the verb is not reachable before one.
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::FleetText)));
}

TEST_CASE("A FLEET-TEXT request carries its keys and its token as the words typed", "[wire]")
{
    auto const frame =
        EncodeFleetTextRequest(FleetTextRequest { .section = "series", .range = "7d", .dashboardToken = "t0k" });
    REQUIRE(frame.size() > RequestHeaderSize);
    CHECK(frame[2] == static_cast<std::byte>(Op::FleetText));

    auto const payload = std::span { frame }.subspan(RequestHeaderSize);
    auto const decoded = DecodeFleetTextRequest(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).section == "series");
    CHECK(Unwrap(decoded).range == "7d");
    CHECK(Unwrap(decoded).dashboardToken == "t0k");

    SECTION("a key this build does not serve still decodes, so the leader can refuse it by name")
    {
        auto const unknown =
            EncodeFleetTextRequest(FleetTextRequest { .section = "nope", .range = "1fortnight", .dashboardToken = {} });
        auto const read = DecodeFleetTextRequest(std::span { unknown }.subspan(RequestHeaderSize));
        REQUIRE(read.has_value());
        CHECK(Unwrap(read).section == "nope");
        CHECK(Unwrap(read).range == "1fortnight");
    }

    SECTION("empty words are the defaults, not a malformed frame")
    {
        auto const defaults = EncodeFleetTextRequest(FleetTextRequest {});
        auto const read = DecodeFleetTextRequest(std::span { defaults }.subspan(RequestHeaderSize));
        REQUIRE(read.has_value());
        CHECK(Unwrap(read).section.empty());
        CHECK(Unwrap(read).range.empty());
    }

    SECTION("a payload that is not exactly three fields is refused")
    {
        auto const twoFields = WireFields::Encode({ AsBytes("kpi"), AsBytes("1h") });
        CHECK_FALSE(DecodeFleetTextRequest(twoFields).has_value());

        auto trailing = std::vector<std::byte> { payload.begin(), payload.end() };
        trailing.push_back(std::byte { 0x00 });
        CHECK_FALSE(DecodeFleetTextRequest(trailing).has_value());
    }
}

// --- The consensus dial address (#1328) -------------------------------------

TEST_CASE("A node runtime record carries the consensus address peers dial, and absent is not empty",
          "[wire][consensus][node-status]")
{
    // Absent on a node that runs no consensus. A zero-length field is how every optional in
    // this record says so, which is why an engaged endpoint is never empty.
    auto const silent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(silent.has_value());
    CHECK_FALSE(Unwrap(silent).consensusEndpoint.has_value());

    NodeRuntimeFields sent {};
    sent.consensusEndpoint = "10.0.0.4:6680";
    sent.leaderEndpoint = "10.0.0.9:6680";
    auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
    // Two text fields in one record, so each is asserted where the OTHER would be read if
    // an index were shared: the leader is not this node, and neither is its dial address.
    CHECK(Unwrap(back).leaderEndpoint == "10.0.0.9:6680");
}

TEST_CASE("The consensus endpoint has one name, spelled as prose and as a record field", "[wire][consensus]")
{
    // The VALUES, pinned: both are text an operator types into a search or holds against a second
    // screen, so a consistent rename keeps every in-tree test agreeing while the name they compare
    // under silently moves.
    CHECK(ConsensusEndpointLabel == "consensus endpoint");
    CHECK(ConsensusEndpointField == "consensus-endpoint");
    CHECK(ConsensusEndpointHeading == "dialled at");

    // And the RELATION, which is what keeps them one name: the field is the label, hyphenated. A
    // rename of one alone fails here rather than in a comparison an operator makes.
    auto hyphenated = std::string { ConsensusEndpointLabel };
    std::ranges::replace(hyphenated, ' ', '-');
    CHECK(hyphenated == ConsensusEndpointField);
}

TEST_CASE("The consensus address rides the runtime record's variable arity, in both directions",
          "[wire][consensus][node-status]")
{
    // **Why an APPENDED field costs no wire version**, proved rather than inherited from the
    // enrollment case above: the decoder answers an index past the end as empty and ignores
    // a surplus. Several arities, because a decoder that expected EXACTLY one of them would
    // pass the round trip and fail the others. The cuts are THIS grammar's records cut short,
    // labelled by where they end, and a shorter record is legal only within this grammar
    // version: a REMOVED field moves every later one up a place, so the applied-tombstone
    // count's retirement is a grammar change, and a build from before it is refused by version
    // before any field is read.
    NodeRuntimeFields sent {};
    sent.toolchainsServed = 4;
    sent.cordon = WireCordonState::Draining;
    sent.consensusEndpoint = "10.0.0.4:6680";
    sent.consensusStanding = WireConsensusStanding::Learner;
    sent.conditions = std::vector { NodeConditionFields { .id = "scratch-root-unmappable",
                                                          .persistence = "latched",
                                                          .severity = "warning",
                                                          .state = "raised",
                                                          .detail = "no key",
                                                          .remedy = "name one" } };
    sent.identityPublicKey.emplace();
    sent.identityPublicKey->fill(std::byte { 0xA5 });
    sent.roster = NodeRosterFields { .version = 9, .voters = 3, .revoked = 1 };
    sent.enrollment = WireEnrollmentState::AutoApprove;
    sent.enrollmentAutoApproveSecondsLeft = 600;
    sent.stateDirectory = "/var/lib/fastcache-node";
    sent.stateDirectoryReason = "machine-wide: this process runs privileged";
    sent.sharedCache = SharedCacheStatusFields { .source = WireSharedCacheSource::Setting,
                                                 .machineId = "cache-c",
                                                 .endpoint = "10.0.0.9:6674",
                                                 .state = WireSharedCacheState::Proven,
                                                 .detail = {} };
    sent.fleetId = "0123456789abcdef0123456789abcdef@a-voter-key";
    sent.fleetPin = NodeFleetPinFields { .fleet = "fedcba9876543210fedcba9876543210@another-voter-key" };
    // Kept in a local: `SplitAll` hands back spans INTO it.
    auto const emitted = EncodeNodeRuntime(sent);
    auto const parts = WireFields::SplitAll(emitted);
    REQUIRE(parts.has_value());
    // Twenty-four: thirteen that predate #1328, the endpoint it added, #1449's consensus standing,
    // #1364's condition list, #178's identity key, the roster #178 reports, the auto-approve seconds
    // left, the state directory with the reason it is that one, the shared-cache record, and the
    // cluster id with the fleet pin. Pinned, since every cut below is counted from it and a record
    // that grew or shrank would move what "older" means -- which is how this case caught each
    // append, and the retirement of #1471's applied-tombstone count, rather than letting any of them
    // shift the cuts silently.
    REQUIRE(Unwrap(parts).size() == 24);

    SECTION("a record cut after field 13: the endpoint is absent, and the cordon still read")
    {
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 13 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).consensusEndpoint.has_value());
        CHECK_FALSE(Unwrap(back).consensusStanding.has_value());
        // The field before it survives the cut, so the cut is where it was meant to be.
        CHECK(Unwrap(back).cordon == std::optional { WireCordonState::Draining });
        CHECK(Unwrap(back).toolchainsServed == 4);
    }

    SECTION("a record cut after field 14: the standing is absent")
    {
        // The standing comes back "did not say" rather than taking the reply -- or the endpoint
        // before it -- with it.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 14 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).consensusStanding.has_value());
        CHECK_FALSE(Unwrap(back).conditions.has_value());
        // And everything that build DID send is still read, so the cut removed one fact rather
        // than truncating the record.
        CHECK(Unwrap(back).consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
        CHECK(Unwrap(back).cordon == std::optional { WireCordonState::Draining });
    }

    SECTION("a record cut after field 15: the conditions are absent")
    {
        // The list must come back ABSENT -- which every renderer shows as its absent marker --
        // and never as an empty list, which would read as a node with nothing raised.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 15 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).conditions.has_value());
        CHECK_FALSE(Unwrap(back).identityPublicKey.has_value());
        CHECK(Unwrap(back).consensusStanding == std::optional { WireConsensusStanding::Learner });
    }

    SECTION("a record cut after field 16: the key is absent")
    {
        // The key comes back "did not say" rather than taking the conditions before it -- or the
        // reply -- with it.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 16 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).identityPublicKey.has_value());
        CHECK(Unwrap(back).consensusStanding == std::optional { WireConsensusStanding::Learner });
        CHECK(Unwrap(back).conditions == sent.conditions);
    }

    SECTION("a record cut after field 17: the roster is absent")
    {
        // A node holding no roster answers exactly so, and a record that ends before the field
        // must read the same: absent, never a roster of nobody.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 17 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).roster.has_value());
        CHECK(Unwrap(back).identityPublicKey == sent.identityPublicKey);
    }

    SECTION("a record cut after field 18: the auto-approve seconds are absent")
    {
        // No deadline armed is what a record that ends before the field must read as: absent,
        // never a zero that would read as a deadline in its last second.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 18 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).enrollmentAutoApproveSecondsLeft.has_value());
        CHECK(Unwrap(back).roster == sent.roster);
        CHECK(Unwrap(back).enrollment == sent.enrollment);
    }

    SECTION("a record cut after field 19: the state directory is absent")
    {
        // Both absent -- a node that does not say where it keeps its identity -- and the field
        // before them survives the cut, so the cut is where it was meant to be.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 19 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).stateDirectory.has_value());
        CHECK_FALSE(Unwrap(back).stateDirectoryReason.has_value());
        CHECK(Unwrap(back).enrollmentAutoApproveSecondsLeft == std::optional<std::uint64_t> { 600 });
    }

    SECTION("the state directory and its reason travel both or neither")
    {
        // One alone is not half an answer: a path without its reason cannot be told from the
        // machine's other identity. Refused, in either direction, as a shape this build does not
        // know -- and the control, both present, is the round trip below.
        for (auto const dropped: { std::size_t { 19 }, std::size_t { 20 } })
        {
            INFO("field " << dropped << " sent empty");
            auto lopsided = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
            lopsided[dropped] = {};
            CHECK_FALSE(DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { lopsided })).has_value());
        }
    }

    SECTION("twenty-one fields, as a build after the state directory and before the shared-cache record emits: it is absent")
    {
        // Absent, never a record of zeroes: a node too old to say is not one whose shared cache
        // is `None`.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 21 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).sharedCache.has_value());
        CHECK(Unwrap(back).stateDirectory == sent.stateDirectory);
    }

    SECTION("twenty-two fields, as a build after the shared-cache record and before the fleet pin emits: the "
            "fleet id and the pin are absent")
    {
        // ABSENT, never "unpinned": a node too old to say must not read as one trusting on first
        // use, which is the answer an operator asks the pin for.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 22 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).fleetId.has_value());
        CHECK_FALSE(Unwrap(back).fleetPin.has_value());
        CHECK(Unwrap(back).stateDirectoryReason == sent.stateDirectoryReason);
    }

    SECTION("the whole record, twenty-four fields: every fact engaged")
    {
        auto const current = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { current }));
        REQUIRE(back.has_value());
        auto const runtime = Unwrap(back);
        CHECK(runtime.consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
        CHECK(runtime.consensusStanding == std::optional { WireConsensusStanding::Learner });
        CHECK(runtime.conditions == sent.conditions);
        CHECK(runtime.identityPublicKey == sent.identityPublicKey);
        CHECK(runtime.roster == sent.roster);
        CHECK(runtime.enrollmentAutoApproveSecondsLeft == std::optional<std::uint64_t> { 600 });
        CHECK(runtime.stateDirectory == sent.stateDirectory);
        CHECK(runtime.stateDirectoryReason == sent.stateDirectoryReason);
        CHECK(runtime.sharedCache == sent.sharedCache);
        CHECK(runtime.fleetId == sent.fleetId);
        CHECK(runtime.fleetPin == sent.fleetPin);
    }

    SECTION("twenty-five fields, one past the record: the surplus is skipped")
    {
        auto ahead = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
        auto const extra = AsBytes(std::string_view { "a fact from the future" });
        ahead.emplace_back(extra);
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { ahead }));
        REQUIRE(back.has_value());
        auto const runtime = Unwrap(back);
        CHECK(runtime.consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
        CHECK(runtime.consensusStanding == std::optional { WireConsensusStanding::Learner });
        CHECK(runtime.conditions == sent.conditions);
        CHECK(runtime.identityPublicKey == sent.identityPublicKey);
        CHECK(runtime.roster == sent.roster);
        CHECK(runtime.stateDirectory == sent.stateDirectory);
        CHECK(runtime.stateDirectoryReason == sent.stateDirectoryReason);
        CHECK(runtime.sharedCache == sent.sharedCache);
        CHECK(runtime.fleetPin == sent.fleetPin);
    }
}

TEST_CASE("A fleet pin travels as three answers: pinned to an id, pinned to nothing, and not said",
          "[wire][node-status][pin]")
{
    // The bytes, not only the names: the tag is transmitted.
    CHECK(static_cast<std::uint8_t>(WireFleetPinTag::Unpinned) == 0x00);
    CHECK(static_cast<std::uint8_t>(WireFleetPinTag::Pinned) == 0x01);

    // Unpinned is an ANSWER and must come back engaged: it is what an operator asks the pin for.
    for (auto const& pin:
         { NodeFleetPinFields {}, NodeFleetPinFields { .fleet = "0123456789abcdef0123456789abcdef@a-voter-key" } })
    {
        INFO("pinned to " << pin.fleet.value_or("nothing"));
        NodeRuntimeFields sent {};
        sent.fleetPin = pin;
        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).fleetPin == std::optional { pin });
    }
    auto const silent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(silent.has_value());
    CHECK_FALSE(Unwrap(silent).fleetPin.has_value());

    // A pin record this build cannot read refuses the record rather than reading as half an answer:
    // an unknown tag, an id beside "unpinned", no id beside "pinned", a tag of the wrong width.
    auto const id = AsBytes(std::string_view { "0123" });
    auto const unpinned = std::array { std::byte { 0x00 } };
    auto const pinned = std::array { std::byte { 0x01 } };
    auto const unknown = std::array { std::byte { 0x02 } };
    auto const wide = std::array { std::byte { 0x01 }, std::byte { 0x00 } };
    auto const malformed = std::array {
        WireFields::Encode({ std::span<std::byte const> { unknown }, std::span<std::byte const> {} }),
        WireFields::Encode({ std::span<std::byte const> { unpinned }, id }),
        WireFields::Encode({ std::span<std::byte const> { pinned }, std::span<std::byte const> {} }),
        WireFields::Encode({ std::span<std::byte const> { wide }, id }),
    };
    for (auto const& record: malformed)
    {
        auto emitted = EncodeNodeRuntime(NodeRuntimeFields {});
        auto parts = Unwrap(WireFields::SplitAll(emitted));
        REQUIRE(parts.size() == 24);
        parts[23] = record;
        CHECK_FALSE(DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts })).has_value());
    }
}

TEST_CASE("An identity key travels as its 32 bytes, absent as nothing, and any other width refuses the record",
          "[wire][node-status][identity]")
{
    // Absent is a zero-length field, as every optional here is -- a node that holds no key.
    auto const absent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(absent.has_value());
    CHECK_FALSE(Unwrap(absent).identityPublicKey.has_value());

    // A key, every byte distinct, so a decoder that read it from the wrong offset or in the
    // wrong order cannot come back equal.
    NodeRuntimeFields sent {};
    sent.identityPublicKey.emplace();
    for (auto const index: std::views::iota(std::size_t { 0 }, IdentityPublicKeyBytes))
        (*sent.identityPublicKey)[index] = static_cast<std::byte>(index + 1);
    auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).identityPublicKey == sent.identityPublicKey);

    // A PREFIX of a key is another key, so a short field is refused rather than padded, and a
    // long one rather than truncated: either would print a string an operator compares against
    // a machine that holds no such key. The key is the seventeenth field, behind #1364's
    // conditions and ahead of the roster.
    for (auto const width: { std::size_t { 31 }, std::size_t { 33 } })
    {
        auto emitted = EncodeNodeRuntime(NodeRuntimeFields {});
        auto parts = Unwrap(WireFields::SplitAll(emitted));
        REQUIRE(parts.size() == 24);
        auto const wrong = std::vector<std::byte>(width, std::byte { 0x11 });
        parts[16] = wrong;
        CHECK_FALSE(DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts })).has_value());
    }
}

TEST_CASE("A consensus standing travels as its pinned byte, and one this build cannot name is skipped",
          "[wire][consensus][node-status][learner]")
{
    // The BYTES, not only the names (#1449): a consistent renumbering keeps every in-tree
    // test agreeing while a deployed CLI reads a learner as a voter.
    CHECK(static_cast<std::uint8_t>(WireConsensusStanding::NoCluster) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireConsensusStanding::Voter) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireConsensusStanding::Learner) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireConsensusStanding::Outsider) == 0x04);

    for (auto const standing: { WireConsensusStanding::NoCluster,
                                WireConsensusStanding::Voter,
                                WireConsensusStanding::Learner,
                                WireConsensusStanding::Outsider })
    {
        NodeRuntimeFields sent {};
        sent.consensusStanding = standing;
        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).consensusStanding == std::optional { standing });
    }

    // A byte from a build ahead of this one is left DISENGAGED, never refused -- the rule
    // every enum in this record keeps, so an older client still reads the rest of the reply.
    NodeRuntimeFields sent {};
    sent.consensusEndpoint = "10.0.0.4:6680";
    auto emitted = EncodeNodeRuntime(sent);
    auto parts = Unwrap(WireFields::SplitAll(emitted));
    // Twenty-four: #1364 and #178 appended the condition list, the identity key and the roster
    // behind the standing, and the auto-approve seconds, the state directory with its reason, the
    // shared-cache record and the cluster id with the fleet pin behind those, while the
    // applied-tombstone count ahead of it was retired -- so the standing is the fifteenth field, and
    // the byte replaced below is the one under test.
    REQUIRE(parts.size() == 24);
    auto const unknown = std::array { std::byte { 0x7F } };
    parts[14] = unknown;
    auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts }));
    REQUIRE(back.has_value());
    CHECK_FALSE(Unwrap(back).consensusStanding.has_value());
    // The neighbour before it still reads, so the skip took one byte rather than the record.
    CHECK(Unwrap(back).consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
}

TEST_CASE("The retired client verbs' bytes are claimed by no row, and never will be", "[wire][retired]")
{
    // 0x16 was CLUSTER-ADMIT-CLIENT and 0x17 CLUSTER-FORGET-CLIENT (#1309). The bytes are
    // pinned as well as the table's silence about them: a table that forgot the array would
    // pass the loop over nothing.
    CHECK(RetiredOpcodes == std::array<std::uint8_t, 2> { 0x16, 0x17 });
    for (auto const byte: RetiredOpcodes)
    {
        INFO(static_cast<int>(byte));
        CHECK(FindOp(byte) == nullptr);
        CHECK(std::ranges::none_of(OpTable,
                                   [byte](OpDescriptor const& row) { return static_cast<std::uint8_t>(row.code) == byte; }));
    }
    // The control: a byte a live row claims is found, so `FindOp`'s nullptr above is an answer
    // about those bytes rather than about the function.
    REQUIRE(FindOp(static_cast<std::uint8_t>(Op::ClusterAdmit)) != nullptr);
}

// --- Explaining an admission (#1471) ----------------------------------------

TEST_CASE("explain-admission keeps its byte, and its standing and route bytes are pinned", "[wire][admission]")
{
    // The value as well as the name, for the cordon byte's reason: a consistent
    // renumbering keeps every in-tree test agreeing while a deployed CLI breaks.
    CHECK(static_cast<std::uint8_t>(Op::ExplainAdmission) == 0x1B);
    CHECK(std::ranges::count(OpTable, Op::ExplainAdmission, &OpDescriptor::code) == 1);

    // The node family, because the question is about THIS node's own fold rather than
    // about the cluster's agreed state -- so a node running no consensus still answers it.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::ExplainAdmission)) == VerbFamily::Node);

    // Reachable before admission, because its SELF form must reach a caller the node refuses, or
    // it could never report the refusal. Not a leak: the self form answers only about the caller's
    // own connection, and the machine form, which reads the roster, is gated in the node. What
    // bounds a stranger here is the verb's own ceiling, which `PreAuthVerbsAreBounded` requires.
    CHECK(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::ExplainAdmission)));
    CHECK(OpPayloadCap(static_cast<std::uint8_t>(Op::ExplainAdmission), MaxControlPayload) == MaxExplainAdmissionPayload);
    CHECK(MaxExplainAdmissionPayload == 512);

    // And the bytes a client of another build reads back.
    CHECK(static_cast<std::uint8_t>(WireMembership::Outsider) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireMembership::Member) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireMembership::Forgotten) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireMachineStanding::Voter) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireMachineStanding::Learner) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireMachineStanding::Pending) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireMachineStanding::Revoked) == 0x04);
    CHECK(static_cast<std::uint8_t>(WireMachineStanding::Unknown) == 0x05);
    CHECK(WireMembershipRoute::OpenPolicy == 0x08);
    CHECK(WireMembershipRoute::ProvenIdentity == 0x10);
    CHECK(WireMembershipRoute::KeyTombstone == 0x20);
    CHECK(WireMembershipRoute::Loopback == 0x40);
    CHECK(WireMembershipRoute::MachineTicket == 0x80);

    // The retired bits stay reserved: a deployed client may still render them by their old names.
    CHECK(RetiredWireMembershipRoutes == std::array<std::uint32_t, 3> { 0x01, 0x02, 0x04 });
    for (auto const retired: RetiredWireMembershipRoutes)
        for (auto const live: LiveWireMembershipRoutes)
            CHECK((retired & live) == 0);
}

TEST_CASE("Every admission route bit is one bit, and no two routes share one", "[wire][admission]")
{
    for (auto const bit: LiveWireMembershipRoutes)
        CHECK(std::popcount(bit) == 1);
    auto combined = std::uint32_t { 0 };
    for (auto const bit: LiveWireMembershipRoutes)
    {
        CHECK((combined & bit) == 0);
        combined |= bit;
    }
}

TEST_CASE("An empty subject asks about the caller, a machine question names one, and nothing else is read",
          "[wire][admission]")
{
    auto const self = EncodeExplainAdmissionRequest("");
    auto const header = DecodeRequestHeader(self);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == 0x1B);
    CHECK(DecodeExplainAdmissionPayload(std::span { self }.subspan(RequestHeaderSize)) == std::optional<std::string> { "" });

    auto const machine = EncodeExplainAdmissionRequest("pc-07");
    CHECK(DecodeExplainAdmissionPayload(std::span { machine }.subspan(RequestHeaderSize))
          == std::optional<std::string> { "pc-07" });

    // Two fields is a different question arriving under this verb's name -- refused rather than
    // answered about the first of them -- and no field is not an empty subject.
    auto const first = AsBytes(std::string_view { "a" });
    auto const second = AsBytes(std::string_view { "b" });
    CHECK_FALSE(DecodeExplainAdmissionPayload(WireFields::Encode({ first, second })).has_value());
    CHECK_FALSE(DecodeExplainAdmissionPayload({}).has_value());
}

TEST_CASE("An admission explanation round-trips both shapes and every verdict", "[wire][admission]")
{
    for (auto const& sent: {
             // A machine question: its standing and its name ride with the routes.
             AdmissionExplanationFields { .verdict = WireMembership::Member,
                                          .decidedBy =
                                              WireMembershipRoute::ProvenIdentity | WireMembershipRoute::MachineTicket,
                                          .standing = WireMachineStanding::Learner,
                                          .subject = "pc-07" },
             AdmissionExplanationFields { .verdict = WireMembership::Forgotten,
                                          .decidedBy = WireMembershipRoute::KeyTombstone,
                                          .standing = WireMachineStanding::Revoked,
                                          .subject = "gone" },
             // The caller itself: no standing, and the silence -- refused, and no route claims
             // authorship. Zero is the READING here rather than a missing field.
             AdmissionExplanationFields {
                 .verdict = WireMembership::Outsider, .decidedBy = 0, .standing = std::nullopt, .subject = "10.0.0.7" },
             AdmissionExplanationFields { .verdict = WireMembership::Member,
                                          .decidedBy = WireMembershipRoute::Loopback,
                                          .standing = std::nullopt,
                                          .subject = "127.0.0.1" },
         })
    {
        INFO(sent.subject);
        auto const back = DecodeAdmissionExplanation(EncodeAdmissionExplanation(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back) == sent);
    }
}

TEST_CASE("An unknown VERDICT or STANDING is refused and an unknown ROUTE is kept, which is not one rule twice",
          "[wire][admission]")
{
    auto const machine = AdmissionExplanationFields { .verdict = WireMembership::Member,
                                                      .decidedBy = WireMembershipRoute::ProvenIdentity,
                                                      .standing = WireMachineStanding::Learner,
                                                      .subject = "pc-07" };

    SECTION("a verdict byte this build cannot name is refused, never read as a refusal nobody authored")
    {
        // Falling back to `Outsider` would report a caller as refused-by-nobody on a build that had
        // learned a fourth answer -- which reads exactly like the healthy case.
        auto encoded = EncodeAdmissionExplanation(machine);
        // [4-byte len][verdict] ...
        encoded[4] = std::byte { 0x7F };
        CHECK_FALSE(DecodeAdmissionExplanation(encoded).has_value());
        CHECK_FALSE(DecodeAdmissionExplanation({}).has_value());
    }

    SECTION("a standing byte this build cannot name is refused, never read as some other standing")
    {
        // [4-byte len][verdict][4-byte len][u32 routes][4-byte len][standing] ...
        auto encoded = EncodeAdmissionExplanation(machine);
        REQUIRE(encoded[17] == static_cast<std::byte>(WireMachineStanding::Learner));
        encoded[17] = static_cast<std::byte>(static_cast<std::uint8_t>(WireMachineStanding::Unknown) + 1);
        CHECK_FALSE(DecodeAdmissionExplanation(encoded).has_value());
    }

    SECTION("a route bit this build cannot name is KEPT, so authorship is not under-reported")
    {
        // The opposite decision, deliberately: the routes are a SET, so a bit this build cannot name
        // still says *something decided*. Dropping it would say fewer things decided this than did,
        // on a fleet mid-upgrade -- and the reader can tell, because the bit is still there.
        constexpr auto ahead = std::uint32_t { 0x8000'0000 };
        auto sent = machine;
        sent.decidedBy |= ahead;
        auto const back = DecodeAdmissionExplanation(EncodeAdmissionExplanation(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).decidedBy == sent.decidedBy);
    }

    SECTION("a verdict or standing field that is not one byte is refused rather than read from its first")
    {
        auto const verdict = std::array { std::byte { 0x02 } };
        auto const wide = std::array { std::byte { 0x02 }, std::byte { 0x02 } };
        auto const routes = WireFields::ToBigEndian<std::uint32_t>(WireMembershipRoute::ProvenIdentity);
        auto const subject = AsBytes(std::string_view { "pc-07" });
        CHECK_FALSE(DecodeAdmissionExplanation(WireFields::Encode({ std::span<std::byte const> { wide },
                                                                    std::span<std::byte const> { routes },
                                                                    std::span<std::byte const> {},
                                                                    subject }))
                        .has_value());
        CHECK_FALSE(DecodeAdmissionExplanation(WireFields::Encode({ std::span<std::byte const> { verdict },
                                                                    std::span<std::byte const> { routes },
                                                                    std::span<std::byte const> { wide },
                                                                    subject }))
                        .has_value());
    }
}

// --- Admitting a learner (#1449) ----------------------------------------------

TEST_CASE("The learner admission occupies the byte it was assigned, beside the voter admission", "[wire][cluster][learner]")
{
    // The value as well as the name, for the explain-admission byte's reason.
    CHECK(static_cast<std::uint8_t>(Op::ClusterAdmitLearner) == 0x1C);
    CHECK(static_cast<std::uint8_t>(Op::ClusterAdmit) == 0x0B);
    CHECK(std::ranges::count(OpTable, Op::ClusterAdmitLearner, &OpDescriptor::code) == 1);

    // The scheduler family and never pre-auth, exactly as the voter admission: the two
    // change the same replicated state and must not be gated two ways.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::ClusterAdmitLearner)) == VerbFamily::Scheduler);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::ClusterAdmitLearner))
          == FamilyOf(static_cast<std::uint8_t>(Op::ClusterAdmit)));
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::ClusterAdmitLearner)));
    CHECK(OpFieldCount(Op::ClusterAdmitLearner) == OpFieldCount(Op::ClusterAdmit));

    CHECK(IsMemberAdmission(Op::ClusterAdmit));
    CHECK(IsMemberAdmission(Op::ClusterAdmitLearner));
    CHECK_FALSE(IsMemberAdmission(Op::ClusterForget));
}

TEST_CASE("Both member admissions frame one payload under their own byte", "[wire][cluster][learner]")
{
    auto const request = ClusterAdmitRequest { .memberId = "laptop",
                                               .raftEndpoint = "10.0.0.9:6675",
                                               .publicKey = "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL" };
    auto const learner = EncodeClusterAdmit<Op::ClusterAdmitLearner>(request);
    auto const voter = EncodeClusterAdmit<Op::ClusterAdmit>(request);

    auto const learnerHeader = DecodeRequestHeader(learner);
    REQUIRE(learnerHeader.has_value());
    CHECK(Unwrap(learnerHeader).opRaw == 0x1C);
    auto const voterHeader = DecodeRequestHeader(voter);
    REQUIRE(voterHeader.has_value());
    CHECK(Unwrap(voterHeader).opRaw == 0x0B);

    // The payloads are byte-identical: which set the member lands in is the op, never a
    // field -- so neither decoder can disagree with the other about the member.
    auto const learnerPayload = std::span<std::byte const> { learner }.subspan(RequestHeaderSize);
    auto const voterPayload = std::span<std::byte const> { voter }.subspan(RequestHeaderSize);
    CHECK(std::ranges::equal(learnerPayload, voterPayload));

    auto const decoded = DecodeClusterAdmitPayload<Op::ClusterAdmitLearner>(learnerPayload);
    REQUIRE(decoded.has_value());
    CHECK(AsStringView(Unwrap(decoded).memberId) == "laptop");
    CHECK(AsStringView(Unwrap(decoded).raftEndpoint) == "10.0.0.9:6675");
    REQUIRE(Unwrap(decoded).publicKey.has_value());
    CHECK(AsStringView(Unwrap(Unwrap(decoded).publicKey)) == "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL");
}

TEST_CASE("A member admission carries three fields and a version no version-10 peer reads, as bytes",
          "[wire][cluster][identity]")
{
    // #178. The COUNT and the VERSION, as values and as the bytes a frame carries -- a symbol
    // both ends spell can only test that they agree with each other, and a version-10 peer
    // reads the byte, not the name. Version 11 made the admission three fields; 14 is this
    // build's, moved since by ENROLL, the roster and the sealed handshake (#178), and the byte is
    // pinned at what is SENT.
    CHECK(OpFieldCount(Op::ClusterAdmit) == 3);
    CHECK(OpFieldCount(Op::ClusterAdmitLearner) == 3);

    auto const request =
        ClusterAdmitRequest { .memberId = "n4", .raftEndpoint = "10.0.0.4:6680", .publicKey = std::nullopt };
    for (auto const& frame:
         { EncodeClusterAdmit<Op::ClusterAdmit>(request), EncodeClusterAdmit<Op::ClusterAdmitLearner>(request) })
    {
        REQUIRE(frame.size() > RequestHeaderSize);
        CHECK(std::to_integer<unsigned>(frame[0]) == 0xFC);
        CHECK(std::to_integer<unsigned>(frame[1]) == 14);

        // No key is a zero-length THIRD field, never a two-field payload: the arity is exact.
        auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
        auto const fields = WireFields::SplitExactly(payload, 3);
        REQUIRE(fields.has_value());
        CHECK(Unwrap(fields)[2].empty());
    }
}

TEST_CASE("A member admission's key is carried, and absence decodes as absence", "[wire][cluster][identity]")
{
    // #178. Both directions, because a decoder that invented a key -- or dropped one --
    // passes the other. And the version-10 shape, two fields, is refused: read leniently it
    // would admit a member with no key while its operator believed it had one.
    auto const keyed = EncodeClusterAdmit<Op::ClusterAdmit>(ClusterAdmitRequest {
        .memberId = "n4", .raftEndpoint = "10.0.0.4:6680", .publicKey = "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK" });
    auto const keyedView =
        DecodeClusterAdmitPayload<Op::ClusterAdmit>(std::span<std::byte const> { keyed }.subspan(RequestHeaderSize));
    REQUIRE(keyedView.has_value());
    REQUIRE(Unwrap(keyedView).publicKey.has_value());
    CHECK(AsStringView(Unwrap(Unwrap(keyedView).publicKey)) == "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK");

    auto const bare = EncodeClusterAdmit<Op::ClusterAdmit>(
        ClusterAdmitRequest { .memberId = "n4", .raftEndpoint = "10.0.0.4:6680", .publicKey = std::nullopt });
    auto const bareView =
        DecodeClusterAdmitPayload<Op::ClusterAdmit>(std::span<std::byte const> { bare }.subspan(RequestHeaderSize));
    REQUIRE(bareView.has_value());
    CHECK_FALSE(Unwrap(bareView).publicKey.has_value());
    CHECK(AsStringView(Unwrap(bareView).raftEndpoint) == "10.0.0.4:6680");

    auto const versionTen =
        WireFields::Encode({ AsBytes(std::string_view { "n4" }), AsBytes(std::string_view { "10.0.0.4:6680" }) });
    CHECK_FALSE(DecodeClusterAdmitPayload<Op::ClusterAdmit>(versionTen).has_value());
}

namespace
{
/// A row with every field distinct, so a decoder that reads one field into its neighbour cannot
/// pass by returning the same word from the wrong place.
/// @param id Its id.
/// @param state Its state word.
/// @return The row.
[[nodiscard]] NodeConditionFields ConditionRow(std::string id, std::string state)
{
    return NodeConditionFields { .id = std::move(id),
                                 .persistence = "latched",
                                 .severity = "warning",
                                 .state = std::move(state),
                                 .detail = "the detail",
                                 .remedy = "the remedy" };
}
} // namespace

TEST_CASE("The condition vocabulary travels as WORDS, and each word is pinned", "[wire][conditions]")
{
    // #1364. The enumerators bind nothing on the wire -- a row travels as text, so an older reader
    // renders a newer node's row as written -- which makes the WORD the contract. A symbol both ends
    // spell cannot test it, so the spellings are written out here, once, as the anchor.
    CHECK(ConditionName(ConditionPersistence::Latched) == "latched");
    CHECK(ConditionName(ConditionPersistence::Live) == "live");
    CHECK(ConditionName(ConditionSeverity::Notice) == "notice");
    CHECK(ConditionName(ConditionSeverity::Warning) == "warning");
    CHECK(ConditionName(ConditionSeverity::Alert) == "alert");
    CHECK(ConditionName(ConditionState::Raised) == "raised");
    CHECK(ConditionName(ConditionState::Clear) == "clear");
    CHECK(ConditionName(ConditionState::NotEvaluated) == "not-evaluated");
    CHECK(ConditionName(ConditionState::Undecided) == "undecided");

    // And every word reads back as its enumerator, so the two halves are one table.
    for (auto const& word: ConditionStateWords)
        CHECK(ConditionStateNamed(word.name) == word.state);
    CHECK_FALSE(ConditionStateNamed("suppressed").has_value());

    // The field ORDER is the row's wire layout; pinned by name, position by position.
    auto const names = ConditionFieldTable | std::views::transform(&ConditionFieldRow::name);
    CHECK(std::ranges::equal(
        names, std::array<std::string_view, 6> { "id", "persistence", "severity", "state", "detail", "remedy" }));
}

TEST_CASE("A condition row's bytes are pinned, field by field", "[wire][conditions]")
{
    // The value half of the wire contract: a list is a field list of rows, and a row a field list of
    // six texts, each `[u32 big-endian length][bytes]`. A change to either framing moves these bytes.
    auto const rows = std::vector { NodeConditionFields {
        .id = "a", .persistence = "live", .severity = "alert", .state = "raised", .detail = "", .remedy = "r" } };
    auto const encoded = EncodeNodeConditions(rows);
    auto const expected = std::vector<std::uint8_t> {
        0, 0, 0, 41,                               // the one row's length
        0, 0, 0, 1,  'a',                          // id
        0, 0, 0, 4,  'l', 'i', 'v', 'e',           // persistence
        0, 0, 0, 5,  'a', 'l', 'e', 'r', 't',      // severity
        0, 0, 0, 6,  'r', 'a', 'i', 's', 'e', 'd', // state
        0, 0, 0, 0,                                // detail: empty is a reading here, not an absence
        0, 0, 0, 1,  'r',                          // remedy
    };
    REQUIRE(encoded.size() == expected.size());
    CHECK(std::ranges::equal(
        encoded, expected, [](std::byte b, std::uint8_t e) { return std::to_integer<std::uint8_t>(b) == e; }));
}

TEST_CASE("A condition list survives the wire, and absent is not an empty list", "[wire][conditions][node-status]")
{
    SECTION("every row, every field, in order")
    {
        NodeRuntimeFields fields {};
        fields.conditions = std::vector { ConditionRow("one", "raised"), ConditionRow("two", "clear") };
        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(fields));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).conditions == fields.conditions);
    }

    SECTION("a node that says nothing about conditions decodes ABSENT")
    {
        // The reading an older node gives, and the one every renderer must show as absent rather
        // than as *none raised*: the zero-length field is the only spelling it has.
        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).conditions.has_value());
    }

    SECTION("an announcement carries them on its load record, and a heartbeat does not")
    {
        LoadFields load {};
        load.conditions = std::vector { ConditionRow("one", "raised") };
        auto const announced = DecodeLoad(EncodeLoad(load));
        REQUIRE(announced.has_value());
        CHECK(Unwrap(announced).conditions == load.conditions);

        auto const quiet = DecodeLoad(EncodeLoad(LoadFields {}));
        REQUIRE(quiet.has_value());
        CHECK_FALSE(Unwrap(quiet).conditions.has_value());
    }
}

TEST_CASE("A condition row from a newer node keeps what this build can read", "[wire][conditions]")
{
    // A seventh field is a fact appended by a build ahead of this one: skipped, and the six this
    // build knows are still read. A state word it has never seen is KEPT, and asks for attention.
    auto const texts = std::array<std::string_view, 7> { "id", "latched", "warning", "suppressed", "d", "r", "since" };
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(texts.size());
    for (auto const text: texts)
        fields.push_back(AsBytes(text));
    auto const row = WireFields::Encode(WireFields::FieldList { fields });
    auto const list = WireFields::Encode({ std::span<std::byte const> { row } });

    std::optional<std::vector<NodeConditionFields>> out;
    REQUIRE(ReadNodeConditions(list, out));
    REQUIRE(out.has_value());
    REQUIRE(Unwrap(out).size() == 1);
    CHECK(Unwrap(out).front().state == "suppressed");
    CHECK(Unwrap(out).front().remedy == "r");
    CHECK(AsksForAttention(Unwrap(out).front()));
}

TEST_CASE("A condition list this build cannot read is refused, never read short", "[wire][conditions]")
{
    std::optional<std::vector<NodeConditionFields>> out;

    SECTION("a row short of the six fields")
    {
        auto const five = WireFields::Encode({ AsBytes(std::string_view { "id" }),
                                               AsBytes(std::string_view { "latched" }),
                                               AsBytes(std::string_view { "warning" }),
                                               AsBytes(std::string_view { "raised" }),
                                               AsBytes(std::string_view { "d" }) });
        CHECK_FALSE(ReadNodeConditions(WireFields::Encode({ std::span<std::byte const> { five } }), out));
    }

    SECTION("a field above its ceiling")
    {
        auto const longDetail = std::string(MaxConditionDetailBytes + 1, 'x');
        auto row = ConditionRow("one", "raised");
        row.detail = longDetail;
        CHECK_FALSE(ReadNodeConditions(EncodeNodeConditions(std::vector { row }), out));
    }

    SECTION("more rows than a list may carry")
    {
        // Built by hand, because the encoder honours the ceiling and would never produce one.
        auto const row = EncodeNodeConditions(std::vector { ConditionRow("one", "raised") });
        auto const inner = WireFields::SplitAll(row);
        REQUIRE(inner.has_value());
        auto const many = std::vector<std::span<std::byte const>>(MaxNodeConditions + 1, Unwrap(inner).front());
        CHECK_FALSE(ReadNodeConditions(WireFields::Encode(WireFields::FieldList { many }), out));
    }

    // Refused means refused: nothing was written into the caller's list on the way out.
    CHECK_FALSE(out.has_value());
}

TEST_CASE("NODE-ANNOUNCE carries an endpoint, a capacity, a load and its join memos, and nothing else", "[wire][roster]")
{
    // The retired certified roster's endorsement (#178) is gone from the grammar, not sent empty:
    // four fields, and the five-field request that carried one is refused on its count -- never read
    // with the endorsement's bytes taken for the join memos.
    CHECK(OpFieldCount(Op::NodeAnnounce) == 4);
    auto const frame = EncodeNodeAnnounce(NodeAnnounceRequest { .endpoint = "10.0.0.2:6674", .capacity = {}, .load = {} });
    REQUIRE(frame.size() > RequestHeaderSize);
    CHECK(std::to_integer<unsigned>(frame[1]) == 14);
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const fields = WireFields::SplitExactly(payload, 4);
    REQUIRE(fields.has_value());
    REQUIRE(DecodeNodeAnnouncePayload(payload).has_value());

    auto const endorsement = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x03 } };
    auto const& four = Unwrap(fields);
    auto const withEndorsement = WireFields::Encode({ four[0], four[1], four[2], endorsement, four[3] });
    CHECK_FALSE(DecodeNodeAnnouncePayload(withEndorsement).has_value());
}

TEST_CASE("NODE-ANNOUNCE carries the fleets a node once asked as its fourth field", "[wire][formation]")
{
    // The evidence a split of a fleet is told on reaches the leader in the verb every node sends. The
    // fourth field is present whether or not there are memos: none is a zero-length field.
    STATIC_REQUIRE(MaxAnnouncedJoinMemos == 8);
    auto memos = std::vector<JoinMemoFields> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxAnnouncedJoinMemos))
    {
        auto memo = JoinMemoFields { .clusterId = std::format("c-{}", index) };
        memo.provenKey.fill(static_cast<std::byte>(index));
        memos.push_back(memo);
    }
    for (auto const count: { std::size_t { 0 }, std::size_t { 1 }, MaxAnnouncedJoinMemos })
    {
        INFO(count << " memos");
        auto const carried = std::span<JoinMemoFields const> { memos }.first(count);
        auto const frame = EncodeNodeAnnounce(NodeAnnounceRequest { .endpoint = "10.0.0.2:6674", .joinMemos = carried });
        auto const decoded = DecodeNodeAnnouncePayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(std::ranges::equal(Unwrap(decoded).joinMemos, carried));
    }
}

TEST_CASE("A join memo list is refused as malformed for each way it can be wrong", "[wire][formation]")
{
    // Spliced by hand: the encoder refuses a list past the bound. Each row beside the nearest list
    // that decodes, so the refusal a row meets can only be its own.
    auto const memo = [](std::span<std::byte const> id, std::span<std::byte const> key) {
        return WireFields::Encode({ id, key });
    };
    auto const key = std::array<std::byte, IdentityPublicKeyBytes> {};
    auto const shortKey = std::array<std::byte, IdentityPublicKeyBytes - 1> {};
    auto const listOf = [](std::vector<std::vector<std::byte>> const& entries) {
        auto views = std::vector<std::span<std::byte const>> { entries.begin(), entries.end() };
        return WireFields::Encode(WireFields::FieldList { views });
    };
    auto const good = memo(WireFields::AsBytes(std::string_view { "c-lab" }), key);
    CHECK(DecodeJoinMemos(listOf({ good })).has_value());
    CHECK(DecodeJoinMemos({}).has_value()); // none is a list of none

    CHECK_FALSE(DecodeJoinMemos(listOf({ memo({}, key) })).has_value()); // an empty cluster id
    auto const longest = std::string(MaxIdBytes, 'c');
    CHECK(DecodeJoinMemos(listOf({ memo(WireFields::AsBytes(longest), key) })).has_value());
    auto const tooLong = std::string(MaxIdBytes + 1, 'c');
    CHECK_FALSE(DecodeJoinMemos(listOf({ memo(WireFields::AsBytes(tooLong), key) })).has_value());
    CHECK_FALSE(DecodeJoinMemos(listOf({ memo(WireFields::AsBytes(std::string_view { "c-lab" }), shortKey) })).has_value());
    CHECK_FALSE(DecodeJoinMemos(listOf({ WireFields::Encode({ WireFields::AsBytes(std::string_view { "c-lab" }) }) }))
                    .has_value()); // one field where a memo is two
    CHECK(DecodeJoinMemos(listOf(std::vector(MaxAnnouncedJoinMemos, good))).has_value());
    CHECK_FALSE(DecodeJoinMemos(listOf(std::vector(MaxAnnouncedJoinMemos + 1, good))).has_value());
}

TEST_CASE("grant-unverifiable is its own wire code, pinned as a byte, and roster-expired's is burnt", "[wire][roster]")
{
    // #178: a worker that can verify nobody's grant -- its applied state names no voter, or it has
    // not heard from a leader it counts -- refuses every grant with it. A client reads it as any
    // refused compile -- compile locally -- and an operator reads the worker's counters for which
    // of the two it was.
    CHECK(static_cast<std::uint8_t>(ErrorCode::GrantUnverifiable) == 0x31);
    auto const* const row = Describe(ErrorCode::GrantUnverifiable);
    REQUIRE(row != nullptr);
    CHECK(row->name == "grant-unverifiable");

    // 0x28 was roster-expired, named for a lapse that no longer exists: retired, and never answered
    // by a row a peer built before the retirement would report under the old name.
    CHECK(std::ranges::contains(RetiredErrorCodes, std::uint8_t { 0x28 }));
    CHECK(Describe(static_cast<ErrorCode>(0x28)) == nullptr);
}

TEST_CASE("roster-not-yet-applied is its own wire code, pinned as a byte, and a retriable one", "[wire][proof]")
{
    // Batch 3's M3: a node proof the answering node cannot judge YET, because its consensus has not
    // applied the log it recovered at start. Pinned as a byte beside the raw enumerator, since a peer
    // of another build reads the byte; and retriable, since the prover asks again on a backoff.
    CHECK(static_cast<std::uint8_t>(ErrorCode::RosterNotYetApplied) == 0x32);
    auto const* const row = Describe(ErrorCode::RosterNotYetApplied);
    REQUIRE(row != nullptr);
    CHECK(row->name == "roster-not-yet-applied");
    CHECK(row->retry.MayHelp());
    CHECK(std::ranges::contains(RetriableErrorCodes, ErrorCode::RosterNotYetApplied));
    // Not the refusal it replaces while the state catches up, which tells an operator to admit.
    CHECK(ErrorCode::RosterNotYetApplied != ErrorCode::NodeKeyUnknown);
    CHECK_FALSE(Describe(ErrorCode::NodeKeyUnknown)->retry.MayHelp());
}

TEST_CASE("A node's roster travels in its runtime record, absent when it holds none", "[wire][roster]")
{
    // Field 18 of the nested record (#178). Absent and a roster of nobody are different answers.
    auto runtime = NodeRuntimeFields {};
    SECTION("absent")
    {
        auto const decoded = DecodeNodeRuntime(EncodeNodeRuntime(runtime));
        REQUIRE(decoded.has_value());
        CHECK_FALSE(Unwrap(decoded).roster.has_value());
    }
    SECTION("a roster")
    {
        runtime.roster = NodeRosterFields { .version = 7, .voters = 3, .revoked = 1 };
        auto const decoded = DecodeNodeRuntime(EncodeNodeRuntime(runtime));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).roster == runtime.roster);
    }
}

TEST_CASE("A roster record keeps the retired principal count's position, empty, and drops the lapse", "[wire][roster]")
{
    // The record is read POSITIONALLY with surplus ignored, so the principal count's field stays as a
    // RESERVED, empty position: closing the gap would have a reader take it for the revoked count.
    auto const encoded = EncodeNodeRoster(NodeRosterFields { .version = 7, .voters = 3, .revoked = 1 });
    auto const parts = WireFields::SplitAll(encoded);
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() == NodeRosterFieldCount);
    STATIC_REQUIRE(NodeRosterFieldCount == 4);
    STATIC_REQUIRE(NodeRosterReservedField == 2);
    CHECK(Unwrap(parts)[NodeRosterReservedField].empty());

    // The five-field record of the grammar before -- a principal count third, a lapse fifth -- reads
    // its revoked count from the fourth position, whatever the third held, and ignores the lapse.
    auto const version = WireFields::ToBigEndian<std::uint64_t>(7);
    auto const voters = WireFields::ToBigEndian<std::uint32_t>(3);
    auto const principals = WireFields::ToBigEndian<std::uint32_t>(9);
    auto const revoked = WireFields::ToBigEndian<std::uint32_t>(1);
    auto const lapse = WireFields::ToBigEndian<std::uint64_t>(42);
    auto const older = WireFields::Encode({ std::span<std::byte const> { version },
                                            std::span<std::byte const> { voters },
                                            std::span<std::byte const> { principals },
                                            std::span<std::byte const> { revoked },
                                            std::span<std::byte const> { lapse } });
    auto read = std::optional<NodeRosterFields> {};
    REQUIRE(ReadNodeRoster(older, read));
    CHECK(read == std::optional { NodeRosterFields { .version = 7, .voters = 3, .revoked = 1 } });

    // And three fields is not a roster.
    auto const three = WireFields::Encode(
        { std::span<std::byte const> { version }, std::span<std::byte const> { voters }, std::span<std::byte const> {} });
    auto none = std::optional<NodeRosterFields> {};
    CHECK_FALSE(ReadNodeRoster(three, none));
}

// --- The node identity handshake (#178) -------------------------------------

namespace
{

/// @p count bytes, each @p value: a fixed-width field a case fills with something recognisable.
/// @param count How many.
/// @param value What each one is.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> Filled(std::size_t count, std::uint8_t value)
{
    return std::vector<std::byte>(count, std::byte { value });
}

} // namespace

TEST_CASE("The node handshake's widths, verbs and refusals are pinned as bytes", "[wire][nodeproof]")
{
    // A symbol both ends spell can only test that they agree with each other; a peer of another
    // build reads the BYTE. Every width here is one both ends sign over, so a field silently one
    // byte wider on one side reads as a forgery rather than as a version mismatch.
    CHECK(NodeChallengeBytes == 32);
    CHECK(NodeEphemeralKeyBytes == 32);
    CHECK(IdentityPublicKeyBytes == 32);
    CHECK(NodeSignatureBytes == 64);
    CHECK(SealedFrameTagBytes == 32);

    CHECK(static_cast<std::uint8_t>(Op::NodeChallenge) == 0x18);
    CHECK(static_cast<std::uint8_t>(Op::ProveNode) == 0x19);
    CHECK(static_cast<std::uint8_t>(Op::ClusterAdmitWorker) == 0x1D);
    CHECK(static_cast<std::uint8_t>(Op::SharedFetch) == 0x20);
    CHECK(static_cast<std::uint8_t>(Op::SharedStore) == 0x21);
    CHECK(OpFieldCount(Op::NodeChallenge) == 2);
    CHECK(OpFieldCount(Op::ProveNode) == 3);
    CHECK(OpFieldCount(Op::ClusterAdmitWorker) == 2);

    struct Pinned
    {
        ErrorCode code;
        std::uint8_t byte;
        std::string_view name;
    };
    for (auto const& pinned:
         { Pinned { .code = ErrorCode::NodeKeyUnknown, .byte = 0x29, .name = "node-key-unknown" },
           Pinned { .code = ErrorCode::NodeKeyRevoked, .byte = 0x2A, .name = "node-key-revoked" },
           Pinned { .code = ErrorCode::NodeIdentityRequired, .byte = 0x2B, .name = "node-identity-required" },
           Pinned { .code = ErrorCode::EnrollmentHostFull, .byte = 0x2C, .name = "enrollment-host-full" },
           Pinned { .code = ErrorCode::NotSharedCache, .byte = 0x2F, .name = "not-shared-cache" } })
    {
        CHECK(static_cast<std::uint8_t>(pinned.code) == pinned.byte);
        auto const* const row = Describe(pinned.code);
        REQUIRE(row != nullptr);
        CHECK(row->name == pinned.name);
    }
}

TEST_CASE("A worker's refusal of an argument is its own byte, not a malformed frame", "[wire]")
{
    // The byte is what a launcher of another build reads, and the name is what the verbose
    // line and a log print -- so both are pinned, beside the raw enumerator.
    CHECK(static_cast<std::uint8_t>(ErrorCode::WorkerRejectedArgument) == 0x30);
    auto const* const row = Describe(ErrorCode::WorkerRejectedArgument);
    REQUIRE(row != nullptr);
    CHECK(row->name == "worker-rejected-argument");

    // And it is not the code it was answered with before: `malformed-frame` is a framing
    // disagreement, which is what the launcher then told an operator it was.
    CHECK(ErrorCode::WorkerRejectedArgument != ErrorCode::MalformedFrame);
}

TEST_CASE("A node challenge carries a nonce and an ephemeral key, each exactly as wide as it is", "[wire][nodeproof]")
{
    auto request = NodeChallengeRequest {};
    std::ranges::fill(request.nonce, std::byte { 0x11 });
    std::ranges::fill(request.ephemeral, std::byte { 0x22 });
    auto const frame = EncodeNodeChallenge(request);
    auto const decoded = DecodeNodeChallengePayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == request);

    // One byte short in either field, a third field and a missing one are all refused: a nonce
    // silently truncated would have both ends sign different transcripts.
    auto const nonce = Filled(NodeChallengeBytes, 0x11);
    auto const shortNonce = Filled(NodeChallengeBytes - 1, 0x11);
    auto const ephemeral = Filled(NodeEphemeralKeyBytes, 0x22);
    auto const shortEphemeral = Filled(NodeEphemeralKeyBytes - 1, 0x22);
    CHECK(DecodeNodeChallengePayload(WireFields::Encode({ nonce, ephemeral })).has_value());
    CHECK_FALSE(DecodeNodeChallengePayload(WireFields::Encode({ shortNonce, ephemeral })).has_value());
    CHECK_FALSE(DecodeNodeChallengePayload(WireFields::Encode({ nonce, shortEphemeral })).has_value());
    CHECK_FALSE(DecodeNodeChallengePayload(WireFields::Encode({ nonce, ephemeral, {} })).has_value());
    CHECK_FALSE(DecodeNodeChallengePayload(WireFields::Encode({ nonce })).has_value());
}

TEST_CASE("A node challenge reply round-trips, and every fixed field is exactly as wide as it is", "[wire][nodeproof]")
{
    auto reply =
        NodeChallengeReply { .serverId = "scheduler-1", .serverKey = {}, .nonce = {}, .ephemeral = {}, .signature = {} };
    std::ranges::fill(reply.serverKey, std::byte { 0x33 });
    std::ranges::fill(reply.nonce, std::byte { 0x44 });
    std::ranges::fill(reply.ephemeral, std::byte { 0x55 });
    std::ranges::fill(reply.signature, std::byte { 0x66 });
    auto const payload = EncodeNodeChallengeReply(reply);
    auto const decoded = DecodeNodeChallengeReply(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == reply);

    auto const id = AsBytes(std::string_view { "scheduler-1" });
    auto const key = Filled(IdentityPublicKeyBytes, 0x33);
    auto const nonce = Filled(NodeChallengeBytes, 0x44);
    auto const ephemeral = Filled(NodeEphemeralKeyBytes, 0x55);
    auto const signature = Filled(NodeSignatureBytes, 0x66);
    auto const shortKey = Filled(IdentityPublicKeyBytes - 1, 0x33);
    auto const shortSignature = Filled(NodeSignatureBytes - 1, 0x66);
    CHECK(DecodeNodeChallengeReply(WireFields::Encode({ id, key, nonce, ephemeral, signature })).has_value());
    CHECK_FALSE(DecodeNodeChallengeReply(WireFields::Encode({ id, shortKey, nonce, ephemeral, signature })).has_value());
    CHECK_FALSE(DecodeNodeChallengeReply(WireFields::Encode({ id, key, nonce, ephemeral, shortSignature })).has_value());
    CHECK_FALSE(DecodeNodeChallengeReply(WireFields::Encode({ id, key, nonce, ephemeral })).has_value());
}

TEST_CASE("A node proof carries an id, a key and a signature, the last two exactly as wide as they are", "[wire][nodeproof]")
{
    auto proof = ProveNodeRequest { .nodeId = "node-7", .publicKey = {}, .signature = {} };
    std::ranges::fill(proof.publicKey, std::byte { 0x77 });
    std::ranges::fill(proof.signature, std::byte { 0x88 });
    auto const frame = EncodeProveNode(proof);
    auto const decoded = DecodeProveNodePayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == proof);

    // An EMPTY id is a well-formed proof: the roster is what refuses it, by name, rather than a
    // decoder that would make it read as a malformed frame.
    auto const key = Filled(IdentityPublicKeyBytes, 0x77);
    auto const longKey = Filled(IdentityPublicKeyBytes + 1, 0x77);
    auto const signature = Filled(NodeSignatureBytes, 0x88);
    auto const shortSignature = Filled(NodeSignatureBytes - 1, 0x88);
    CHECK(DecodeProveNodePayload(WireFields::Encode({ {}, key, signature })).has_value());
    CHECK_FALSE(DecodeProveNodePayload(WireFields::Encode({ {}, longKey, signature })).has_value());
    CHECK_FALSE(DecodeProveNodePayload(WireFields::Encode({ {}, key, shortSignature })).has_value());
    CHECK_FALSE(DecodeProveNodePayload(WireFields::Encode({ {}, key })).has_value());
}

TEST_CASE("An enroll reply is five fields, and the one that carried a certified roster is refused", "[wire][enrollment]")
{
    // The certified roster (#178) is gone from the grammar: the outcome, the roster, the challenge,
    // the key and the signature. Named payloads, for the borrowing reason the pending-reply case gives.
    auto const roster = AsBytes(std::string_view { "roster" });
    auto const payload = EncodeEnrollReply(EnrollOutcome::Approved, roster, {}, std::nullopt);
    REQUIRE(WireFields::SplitExactly(payload, 5).has_value());
    auto const reply = DecodeEnrollReply(payload);
    REQUIRE(reply.has_value());
    CHECK(AsStringView(Unwrap(reply).roster) == "roster");

    // The six-field reply that carried a certificate third is refused on its count, never read with the
    // certificate's bytes taken for the challenge -- and so is the two-field reply of an older build.
    auto const tag = std::array { static_cast<std::byte>(EnrollOutcome::Approved) };
    auto const certificate = AsBytes(std::string_view { "certificate" });
    CHECK_FALSE(
        DecodeEnrollReply(WireFields::Encode({ std::span<std::byte const> { tag }, roster, certificate, {}, {}, {} }))
            .has_value());
    CHECK_FALSE(DecodeEnrollReply(WireFields::Encode({ std::span<std::byte const> { tag }, roster })).has_value());
}

TEST_CASE("An enroll reply carries its signature as two fields, both or neither, each exactly one wide",
          "[wire][enrollment]")
{
    // Named payloads, for the borrowing reason the pending-reply case gives. The signature itself is
    // COPIED out, so it outlives its payload by design -- asserted after the payload is gone.
    auto const roster = AsBytes(std::string_view { "roster" });
    auto signature = EnrollReplySignature {};
    signature.publicKey.fill(std::byte { 0x31 });
    signature.signature.fill(std::byte { 0x62 });
    auto decodedSignature = std::optional<EnrollReplySignature> {};
    {
        auto const signedPayload = EncodeEnrollReply(EnrollOutcome::Approved, roster, {}, signature);
        auto const signedReply = DecodeEnrollReply(signedPayload);
        REQUIRE(signedReply.has_value());
        decodedSignature = Unwrap(signedReply).signature;
    }
    CHECK(decodedSignature == std::optional { signature });

    // None is two zero-length fields, and decodes as none rather than as a signature of zeroes.
    auto const unsignedPayload = EncodeEnrollReply(EnrollOutcome::Approved, roster, {}, std::nullopt);
    auto const unsignedReply = DecodeEnrollReply(unsignedPayload);
    REQUIRE(unsignedReply.has_value());
    CHECK_FALSE(Unwrap(unsignedReply).signature.has_value());

    // Half a signature, or a width that is not one key and one signature, is refused: a prefix of a
    // signature verifies nothing, and a caller must not be handed one to try.
    auto const tag = std::array { static_cast<std::byte>(EnrollOutcome::Approved) };
    auto const key = std::span<std::byte const> { signature.publicKey };
    auto const signatureBytes = std::span<std::byte const> { signature.signature };
    auto const refuses = [&tag, roster](std::span<std::byte const> keyField, std::span<std::byte const> signatureField) {
        return !DecodeEnrollReply(
                    WireFields::Encode({ std::span<std::byte const> { tag }, roster, {}, keyField, signatureField }))
                    .has_value();
    };
    CHECK(refuses(key, {}));
    CHECK(refuses({}, signatureBytes));
    CHECK(refuses(key.first(IdentityPublicKeyBytes - 1), signatureBytes));
    CHECK(refuses(key, signatureBytes.first(NodeSignatureBytes - 1)));
    CHECK_FALSE(refuses(key, signatureBytes)); // the control: both, each exactly one wide

    // And the three-field reply of the grammar before the signature is refused, never read as unsigned.
    CHECK_FALSE(DecodeEnrollReply(WireFields::Encode({ std::span<std::byte const> { tag }, roster, {} })).has_value());
}

TEST_CASE("An enroll reply hands a challenge as its third field, exactly one wide or empty", "[wire][enrollment]")
{
    // Named payloads, for the borrowing reason the pending-reply case gives; the challenge is COPIED
    // out, so it outlives its payload by design.
    auto challenge = EnrollChallenge {};
    challenge.fill(std::byte { 0x4D });
    auto decoded = std::optional<EnrollChallenge> {};
    {
        auto const pendingPayload = EncodeEnrollReply(EnrollOutcome::Pending, {}, challenge, std::nullopt);
        auto const pending = DecodeEnrollReply(pendingPayload);
        REQUIRE(pending.has_value());
        decoded = Unwrap(pending).challenge;
    }
    CHECK(decoded == std::optional { challenge });

    // None is a zero-length field, and decodes as none rather than as a challenge of zeroes.
    auto const nonePayload = EncodeEnrollReply(EnrollOutcome::Rejected, {}, {}, std::nullopt);
    auto const none = DecodeEnrollReply(nonePayload);
    REQUIRE(none.has_value());
    CHECK_FALSE(Unwrap(none).challenge.has_value());

    // A width that is not one challenge is refused: a joiner signing over a prefix signs over bytes the
    // leader's row does not hold, and would never refresh it.
    auto const tag = std::array { static_cast<std::byte>(EnrollOutcome::Pending) };
    auto const cut = std::span<std::byte const> { challenge }.first(NodeChallengeBytes - 1);
    CHECK_FALSE(DecodeEnrollReply(WireFields::Encode({ std::span<std::byte const> { tag }, {}, cut, {}, {} })).has_value());
    CHECK(DecodeEnrollReply(WireFields::Encode({ std::span<std::byte const> { tag }, {}, challenge, {}, {} })).has_value());
}

TEST_CASE("cluster-admit-worker carries the worker's id and its key as text", "[wire][nodeproof]")
{
    // The key travels as the text `--print-identity` printed, and the LEADER parses it -- so a
    // mistyped key is refused by the one parser every door reaches, in its own words.
    auto const frame = EncodeClusterAdmitWorker(ClusterAdmitWorkerRequest { .workerId = "w-1", .publicKey = "key-text" });
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const decoded = DecodeClusterAdmitWorkerPayload(payload);
    REQUIRE(decoded.has_value());
    CHECK(AsStringView(Unwrap(decoded).workerId) == "w-1");
    CHECK(AsStringView(Unwrap(decoded).publicKey) == "key-text");

    auto const idOnly = WireFields::Encode({ AsBytes(std::string_view { "w-1" }) });
    CHECK_FALSE(DecodeClusterAdmitWorkerPayload(idOnly).has_value());
}

TEST_CASE("The ticket mint occupies the byte it was assigned, in the session family", "[wire][ticket]")
{
    CHECK(static_cast<std::uint8_t>(Op::MintTicket) == 0x1E);
    CHECK(std::ranges::count(OpTable, Op::MintTicket, &OpDescriptor::code) == 1);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::MintTicket)) == VerbFamily::Session);
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::MintTicket)));
    CHECK(OpFieldCount(Op::MintTicket) == 1);
}

TEST_CASE("A mint request carries exactly one audience, and anything else is refused", "[wire][ticket]")
{
    auto const frame = EncodeMintTicketRequest("office.corp:6674");
    auto const header = DecodeRequestHeader(frame);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == 0x1E);
    CHECK(DecodeMintTicketPayload(std::span { frame }.subspan(RequestHeaderSize))
          == std::optional<std::string> { "office.corp:6674" });
    CHECK_FALSE(DecodeMintTicketPayload(WireFields::Encode({ AsBytes("a"), AsBytes("b") })).has_value());
    CHECK_FALSE(DecodeMintTicketPayload({}).has_value());

    // An empty audience is a well-formed field count but names nobody a ticket could be
    // scoped to, so it is refused here rather than left for whatever mints the ticket.
    CHECK_FALSE(DecodeMintTicketPayload(WireFields::Encode({ AsBytes(std::string_view {}) })).has_value());
}

TEST_CASE("The fleet cache verbs keep their bytes and their family", "[wire][optable][shared-cache]")
{
    // The raw enumerator, deliberately: a symbol both ends spell tests only the name.
    CHECK(static_cast<std::uint8_t>(Op::SharedFetch) == 0x20);
    CHECK(static_cast<std::uint8_t>(Op::SharedStore) == 0x21);
    REQUIRE(FindOp(0x20) != nullptr);
    REQUIRE(FindOp(0x21) != nullptr);
    CHECK(FindOp(0x20)->family == VerbFamily::SharedCache);
    CHECK(FindOp(0x21)->family == VerbFamily::SharedCache);
    CHECK(FindOp(0x20)->name == "shared-fetch");
    CHECK(FindOp(0x21)->name == "shared-store");
    // The private tier's verbs did not move family: FETCH/STORE stay this machine's alone.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Fetch)) == VerbFamily::Cache);
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::Store)) == VerbFamily::Cache);
}

TEST_CASE("A fleet cache verb has exactly its private twin's shape", "[wire][optable][shared-cache]")
{
    // Same objects, same payloads, same replies: only the policy differs, and the policy is the verb.
    for (auto const& [shared, twin]: { std::pair { Op::SharedFetch, Op::Fetch }, std::pair { Op::SharedStore, Op::Store } })
    {
        auto const* const a = FindOp(static_cast<std::uint8_t>(shared));
        auto const* const b = FindOp(static_cast<std::uint8_t>(twin));
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        INFO("verb " << a->name);
        CHECK(a->fieldCount == b->fieldCount);
        CHECK(a->legalStatuses == b->legalStatuses);
        CHECK(a->maxPayload.Bytes() == b->maxPayload.Bytes());
        CHECK(a->preAuth.Allowed() == b->preAuth.Allowed());
        CHECK(a->identity == b->identity);
    }
}

TEST_CASE("A verb pair encodes the verb it names and nothing else", "[wire][shared-cache]")
{
    auto const fleetFetch = EncodeFetchAs(FleetSharedCacheVerbs, "k");
    CHECK(Unwrap(DecodeRequestHeader(fleetFetch)).opRaw == 0x20);
    auto const key = DecodeFetchPayload(std::span<std::byte const> { fleetFetch }.subspan(RequestHeaderSize));
    REQUIRE(key.has_value());
    CHECK(AsStringView(Unwrap(key)) == "k");
    // The daemon pair is byte-for-byte what EncodeFetch always sent.
    CHECK(EncodeFetchAs(DaemonCacheVerbs, "k") == EncodeFetch("k"));
    CHECK(DaemonCacheVerbs != FleetSharedCacheVerbs);

    auto const request = StoreRequest { .key = "k", .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = {} };
    CHECK(Unwrap(DecodeRequestHeader(EncodeStoreAs(FleetSharedCacheVerbs, request))).opRaw == 0x21);
    CHECK(EncodeStoreAs(DaemonCacheVerbs, request) == EncodeStore(request));
}

TEST_CASE("The not-shared-cache refusal keeps its byte and its name", "[wire][errors][shared-cache]")
{
    CHECK(static_cast<std::uint8_t>(ErrorCode::NotSharedCache) == 0x2F);
    auto const* const row = Describe(ErrorCode::NotSharedCache);
    REQUIRE(row != nullptr);
    CHECK(row->name == "not-shared-cache");
    CHECK_FALSE(std::ranges::contains(RetiredErrorCodes, std::uint8_t { 0x2D }));
    // Not `NotAMember`, which would tell a proven member it is not one.
    CHECK(ErrorCode::NotSharedCache != ErrorCode::NotAMember);
}

TEST_CASE("A node status carries its shared-cache record and round-trips it", "[wire][node-status][shared-cache]")
{
    auto fields = NodeStatusFields {
        .version = "1.2.3", .nodeId = "pc-7", .uptimeSeconds = 0, .surfaces = {}, .components = 0, .runtime = {}
    };
    fields.runtime.sharedCache = SharedCacheStatusFields { .source = WireSharedCacheSource::Setting,
                                                           .machineId = "cache-c",
                                                           .endpoint = "cache-c.office.example:6674",
                                                           .state = WireSharedCacheState::WrongKey,
                                                           .detail = "the peer proved another key" };
    auto const decoded = DecodeNodeStatus(EncodeNodeStatus(fields));
    REQUIRE(decoded.has_value());
    REQUIRE(Unwrap(decoded).runtime.sharedCache.has_value());
    CHECK(Unwrap(Unwrap(decoded).runtime.sharedCache) == Unwrap(fields.runtime.sharedCache));

    // Absent is not a record of zeroes.
    auto const none = DecodeNodeStatus(EncodeNodeStatus(NodeStatusFields {
        .version = "1.2.3", .nodeId = {}, .uptimeSeconds = 0, .surfaces = {}, .components = 0, .runtime = {} }));
    REQUIRE(none.has_value());
    CHECK_FALSE(Unwrap(none).runtime.sharedCache.has_value());
}

TEST_CASE("The shared-cache status enums keep their bytes", "[wire][node-status][shared-cache]")
{
    CHECK(static_cast<std::uint8_t>(WireSharedCacheSource::None) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheSource::Setting) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheSource::Override) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheSource::ThisMachine) == 0x04);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::NotTried) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::Proven) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::Unresolved) == 0x03);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::WrongKey) == 0x04);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::Unreachable) == 0x05);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::Serving) == 0x06);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::Unavailable) == 0x07);
    CHECK(static_cast<std::uint8_t>(WireSharedCacheState::ProofRefused) == 0x08);
}

TEST_CASE("A shared-cache record with a state byte this build does not name is skipped", "[wire][node-status][shared-cache]")
{
    auto const record = EncodeSharedCacheStatus(SharedCacheStatusFields { .source = WireSharedCacheSource::Setting,
                                                                          .machineId = "cache-c",
                                                                          .endpoint = "c:6674",
                                                                          .state = WireSharedCacheState::Proven,
                                                                          .detail = {} });
    // The control: the record as sent reads back engaged, so the skip below is the BYTE's doing.
    auto control = std::optional<SharedCacheStatusFields> {};
    CHECK(ReadSharedCacheStatus(record, control));
    CHECK(control.has_value());

    auto parts = Unwrap(WireFields::SplitAll(record));
    REQUIRE(parts.size() == SharedCacheStatusFieldCount);
    for (auto const& [index, unnamed]: { std::pair { std::size_t { 0 }, std::byte { 0x7F } },
                                         std::pair { std::size_t { 3 }, std::byte { 0x7F } },
                                         std::pair { std::size_t { 3 }, std::byte { 0x00 } } })
    {
        INFO("field " << index << " byte " << static_cast<int>(unnamed));
        auto rewrittenParts = parts;
        auto const replacement = std::array { unnamed };
        rewrittenParts[index] = replacement;
        auto const rewritten = WireFields::Encode(WireFields::FieldList { rewrittenParts });
        auto out = std::optional<SharedCacheStatusFields> {};
        CHECK(ReadSharedCacheStatus(rewritten, out));
        CHECK_FALSE(out.has_value());
    }

    // A state two bytes wide is a shape this build does not know: refused, never read as its first byte.
    auto const wide = std::array { std::byte { 0x02 }, std::byte { 0x02 } };
    parts[3] = wide;
    auto out = std::optional<SharedCacheStatusFields> {};
    CHECK_FALSE(ReadSharedCacheStatus(WireFields::Encode(WireFields::FieldList { parts }), out));
    CHECK_FALSE(out.has_value());
}

TEST_CASE("A LEASE carries the client's unreachable workers, and none is an empty field", "[wire][lease][exclusion]")
{
    std::array<std::string_view, 2> const excluded { "laptop.corp:6676", "10.0.0.9:6676" };
    auto const frame =
        EncodeLease(LeaseRequest { .fingerprint = "f", .key = "k", .acceptedCodecs = {}, .excluded = excluded });
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);

    auto const view = DecodeLeasePayload(payload);
    REQUIRE(view.has_value());
    REQUIRE(Unwrap(view).excluded.size() == 2);
    CHECK(AsStringView(Unwrap(view).excluded[0]) == "laptop.corp:6676");
    CHECK(AsStringView(Unwrap(view).excluded[1]) == "10.0.0.9:6676");

    // The BYTES are pinned where the version that carries this field is declared,
    // over the final field order.

    // No exclusions is an EMPTY field, never an absent one: the arity is exact.
    auto const none = EncodeLease(LeaseRequest { .fingerprint = "f", .key = "k", .acceptedCodecs = {} });
    auto const noneView = DecodeLeasePayload(std::span<std::byte const> { none }.subspan(RequestHeaderSize));
    REQUIRE(noneView.has_value());
    CHECK(Unwrap(noneView).excluded.empty());
    CHECK(OpFieldCount(Op::Lease) == 5);
}

TEST_CASE("A LEASE naming too many, an empty or an over-long exclusion is refused", "[wire][lease][exclusion]")
{
    auto const leaseWith = [](std::vector<std::span<std::byte const>> const& entries) {
        auto const list = WireFields::Encode(WireFields::FieldList { entries });
        // The label after the list, empty: a LEASE carries both, and the arity is exact.
        auto const fields = std::vector<std::span<std::byte const>> {
            AsBytes("f"), AsBytes("k"), {}, std::span<std::byte const> { list }, {}
        };
        return WireFields::Encode(WireFields::FieldList { fields });
    };

    std::string const endpoint = "w:1";
    std::vector<std::span<std::byte const>> const atCap(MaxLeaseExclusions, AsBytes(endpoint));
    CHECK(DecodeLeasePayload(leaseWith(atCap)).has_value());

    std::vector<std::span<std::byte const>> const overCap(MaxLeaseExclusions + 1, AsBytes(endpoint));
    CHECK_FALSE(DecodeLeasePayload(leaseWith(overCap)).has_value());

    CHECK_FALSE(DecodeLeasePayload(leaseWith({ std::span<std::byte const> {} })).has_value());

    std::string const longest(MaxExcludedEndpointBytes, 'a');
    CHECK(DecodeLeasePayload(leaseWith({ AsBytes(longest) })).has_value());
    std::string const tooLong(MaxExcludedEndpointBytes + 1, 'a');
    CHECK_FALSE(DecodeLeasePayload(leaseWith({ AsBytes(tooLong) })).has_value());

    // The encoder cannot produce what the decoder refuses: it takes the newest sixteen,
    // which is the FRONT of a newest-first list, so the oldest four are the ones lost.
    std::vector<std::string> spelled;
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxLeaseExclusions + 4))
        spelled.push_back(std::format("w{}:1", index));
    std::vector<std::string_view> const many(spelled.begin(), spelled.end());
    auto const frame = EncodeLease(LeaseRequest { .fingerprint = "f", .key = "k", .acceptedCodecs = {}, .excluded = many });
    auto const view = DecodeLeasePayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(view.has_value());
    REQUIRE(Unwrap(view).excluded.size() == MaxLeaseExclusions);
    CHECK(AsStringView(Unwrap(view).excluded.front()) == "w0:1");
    CHECK(AsStringView(Unwrap(view).excluded.back()) == std::format("w{}:1", MaxLeaseExclusions - 1));
}

TEST_CASE("A LEASE carries its exclusions and its toolchain label together", "[wire][lease][exclusion]")
{
    // Two lanes each gave LEASE a fourth field -- the exclusion list and the label -- and the
    // integration carries both. Each lane's own case leaves the other's field EMPTY, so a decoder
    // that read the label out of the exclusions' slot, or the other way round, would pass both.
    // This one sets every field at once, with values neither field could mistake for the other.
    std::array<std::string_view, 2> const excluded { "laptop.corp:6676", "10.0.0.9:6676" };
    constexpr std::string_view Label = "cl 19.44.35207";
    auto const frame = EncodeLease(LeaseRequest {
        .fingerprint = "fp", .key = "objkey", .acceptedCodecs = { 1 }, .excluded = excluded, .toolchainLabel = Label });
    auto const decoded = DecodeLeasePayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(AsStringView(Unwrap(decoded).fingerprint) == "fp");
    CHECK(AsStringView(Unwrap(decoded).key) == "objkey");
    CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 1 });
    REQUIRE(Unwrap(decoded).excluded.size() == 2);
    CHECK(AsStringView(Unwrap(decoded).excluded[0]) == "laptop.corp:6676");
    CHECK(AsStringView(Unwrap(decoded).excluded[1]) == "10.0.0.9:6676");
    CHECK(AsStringView(Unwrap(decoded).toolchainLabel) == Label);
}

TEST_CASE("A LEASE carries the client's toolchain label", "[wire][lease]")
{
    // The label is what `unserved-toolchain` names when no worker serves the fingerprint: the
    // digest is opaque by design (#194), so without the client's own words an operator is shown a
    // hash. Display only -- nothing matches on it.
    //
    // Asserted by field NAME and round trip only, never by position: lane 0 owns LEASE's field
    // order in the v15 bump and adds the byte pin once that order is fixed. Every other field is
    // asserted beside the label, so a decoder reading the label out of a neighbour's slot fails.
    constexpr std::string_view Label = "cl 19.44.35207";
    auto const frame =
        EncodeLease(LeaseRequest { .fingerprint = "fp", .key = "objkey", .acceptedCodecs = { 1 }, .toolchainLabel = Label });
    auto const decoded = DecodeLeasePayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(decoded.has_value());
    CHECK(AsStringView(Unwrap(decoded).toolchainLabel) == Label);
    CHECK(AsStringView(Unwrap(decoded).fingerprint) == "fp");
    CHECK(AsStringView(Unwrap(decoded).key) == "objkey");
    CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 1 });

    // Unlabelled is an EMPTY field, never an absent one: the arity is exact, so a LEASE that names
    // nothing still decodes.
    auto const unlabelled = EncodeLease(LeaseRequest { .fingerprint = "fp", .key = "k", .acceptedCodecs = {} });
    auto const bare = DecodeLeasePayload(std::span<std::byte const> { unlabelled }.subspan(RequestHeaderSize));
    REQUIRE(bare.has_value());
    CHECK(Unwrap(bare).toolchainLabel.empty());
    CHECK(AsStringView(Unwrap(bare).fingerprint) == "fp");
}

// What `CarriedCacheTiers` stands for, in a file that may name both: this header stays free of
// `Cache/`, and a third tier must fail a build here rather than a payload budget at run time.
static_assert(CarriedCacheTiers == static_cast<std::size_t>(StorageTier::Last),
              "CarriedCacheTiers must equal StorageTier's count, or the NODE-ANNOUNCE budget adds up the wrong worst case");

// The condition list's bound is the most full rows its share holds, with no headroom: one row more
// would overrun it. Both sides, so a share that grew stops pinning a bound nobody re-derived, and a
// bound raised past the share fails here before any frame is built.
static_assert(MaxNodeConditions * MaxNodeConditionRowBytes() <= ConditionPayloadShare,
              "MaxNodeConditions full rows must fit the condition share");
static_assert((MaxNodeConditions + 1) * MaxNodeConditionRowBytes() > ConditionPayloadShare,
              "one row more than MaxNodeConditions must not fit the condition share: the bound is the share's");

TEST_CASE("The condition list's bound is the most full rows its share holds, and one more does not fit",
          "[wire][node-announce][conditions]")
{
    // The runtime side of the two assertions above, through the ENCODER at the worst-case row: what a
    // node actually sends at the bound fits, and one row's encoding more does not.
    NodeConditionFields row {};
    for (auto const& column: ConditionFieldTable)
        row.*column.member = std::string(column.maxBytes, 'c');
    auto const full = EncodeNodeConditions(std::vector<NodeConditionFields>(MaxNodeConditions, row));
    auto const one = EncodeNodeConditions(std::vector<NodeConditionFields>(1, row));
    CHECK(full.size() == MaxNodeConditionListBytes);
    CHECK(one.size() == MaxNodeConditionRowBytes());
    CHECK(full.size() <= ConditionPayloadShare);
    CHECK(full.size() + one.size() > ConditionPayloadShare);
}

TEST_CASE("The longest NODE-ANNOUNCE is exactly the budget its constants add up and it decodes", "[wire][node-announce]")
{
    // The budget beside `MaxNodeAnnounceOtherBytes` is a sum of named ceilings; this builds the
    // request that meets every one of them at once -- `MaxNodeConditions` full condition rows, a full
    // history batch, every string and list at its ceiling, every join memo a node may hand over -- and
    // asserts the payload is EXACTLY that sum, so
    // a field the constants forgot, or one they count twice, is a red case rather than a frame a
    // leader one day refuses.
    auto const fill = [](std::size_t bytes, char c) {
        return std::string(bytes, c);
    };
    auto const addresses = std::vector<std::string>(MaxInterfaceAddresses, fill(MaxInterfaceAddressBytes, 'a'));

    CapacityFields capacity {};
    capacity.logicalCores = 64;
    capacity.totalMemoryBytes = 1;
    capacity.nodeClassRaw = 1;
    capacity.reservedCores = 2;
    capacity.cache.tiers = PerTier<CacheTierBudget>(CarriedCacheTiers, CacheTierBudget { .bytesLimit = 1 });
    capacity.version = fill(MaxNodeVersionBytes, 'v');
    capacity.reservedMemoryBytes = 1;
    capacity.toolchainLabel = fill(MaxToolchainLabelBytes, 'l');
    capacity.displayName = fill(MaxDisplayNameBytes, 'd');
    capacity.interfaceAddresses = addresses;

    LoadFields load {};
    load.cpuBusyPermille = 1000;
    load.availableMemoryBytes = 1;
    load.freeScratchBytes = 1;
    load.cache.tiers = PerTier<CacheTierUsage>(
        CarriedCacheTiers, CacheTierUsage { .itemCount = 1, .bytesUsed = 1, .evictions = 1, .indexBytes = 1 });
    load.cache.hits = 1;
    load.cache.misses = 1;
    load.history = std::vector<HistoryBucketFields>(MaxHistoryBucketsPerHeartbeat, HistoryBucketFields { .startMillis = 1 });
    load.cordoned = true;
    NodeConditionFields row {};
    for (auto const& column: ConditionFieldTable)
        row.*column.member = fill(column.maxBytes, 'c');
    load.conditions = std::vector<NodeConditionFields>(MaxNodeConditions, row);
    load.interfaceAddresses = addresses;

    auto memo = JoinMemoFields { .clusterId = fill(MaxIdBytes, 'm') };
    memo.provenKey.fill(std::byte { 0x6b });
    auto const memos = std::vector<JoinMemoFields>(MaxAnnouncedJoinMemos, memo);
    auto const frame = EncodeNodeAnnounce(NodeAnnounceRequest {
        .endpoint = fill(MaxEndpointBytes, 'e'), .capacity = capacity, .load = load, .joinMemos = memos });
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);

    CHECK(payload.size()
          == MaxNodeAnnounceOtherBytes + (MaxHistoryBucketsPerHeartbeat * MaxHistoryBucketBytes)
                 + MaxNodeConditionListBytes);
    CHECK(payload.size() <= MaxControlPayload);
    REQUIRE(DecodeRequestHeader(frame).has_value());

    auto const decoded = DecodeNodeAnnouncePayload(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).endpoint.size() == MaxEndpointBytes);
    CHECK(Unwrap(decoded).capacity.displayName.size() == MaxDisplayNameBytes);
    CHECK(Unwrap(decoded).capacity.interfaceAddresses.size() == MaxInterfaceAddresses);
    CHECK(Unwrap(decoded).load.history.size() == MaxHistoryBucketsPerHeartbeat);
    REQUIRE(Unwrap(decoded).load.conditions.has_value());
    auto const& conditions = Unwrap(Unwrap(decoded).load.conditions);
    REQUIRE(conditions.size() == MaxNodeConditions);
    CHECK(conditions.back().remedy.size() == MaxConditionRemedyBytes);
    CHECK(Unwrap(decoded).joinMemos == memos);
}

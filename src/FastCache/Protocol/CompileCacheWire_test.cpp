// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

TEST_CASE("The wire constants have their specified byte values")
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
    CHECK(CurrentVersion == 13);
    CHECK(MinSupportedVersion == 13);
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

TEST_CASE("EncodeFetch emits the specified bytes exactly")
{
    auto const frame = EncodeFetch("ab");

    // clang-format off: the grid IS the specification -- one wire field per row.
    auto const expected = Bytes({
        0xFC,                   // magic
        0x0D,                   // version
        0x02,                   // op = Fetch
        0x00, 0x00, 0x00, 0x06, // payloadLength = 6
        0x00, 0x00, 0x00, 0x02, // field[0] length = 2
        0x61, 0x62,             // "ab"
    });
    // clang-format on

    CHECK(frame == expected);
}

TEST_CASE("EncodeStore emits the specified bytes exactly")
{
    auto const value = Bytes({ 0xAA, 0xBB });
    auto const frame = EncodeStore(StoreRequest {
        .key = "k", .prefetchGroup = "", .srcRoot = "s", .buildTree = "b", .value = std::span<std::byte const> { value } });

    auto const expected = Bytes({
        0xFC,                               // magic
        0x0D,                               // version
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

TEST_CASE("EncodeErrorReply emits the specified bytes exactly")
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

TEST_CASE("A miss reply is a zero-length payload, not an absent one")
{
    // The pre-version format answered a miss with a bare 0x00 and no length,
    // which is why an error and a miss could not be told apart and why no reply
    // could be drained without knowing which command produced it.
    auto const reply = EncodeReply(Status::Miss, {});
    CHECK(reply == Bytes({ 0x00, 0x00, 0x00, 0x00, 0x00 }));
    CHECK(reply.size() == ReplyHeaderSize);
}

// --- header round-trips ----------------------------------------------------

TEST_CASE("DecodeRequestHeader reads back what EncodeFetch wrote")
{
    auto const frame = EncodeFetch("ab");
    auto const header = DecodeRequestHeader(std::span<std::byte const> { frame }.first(RequestHeaderSize));

    REQUIRE(header.has_value());
    CHECK(Unwrap(header).version == CurrentVersion);
    CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Op::Fetch));
    CHECK(Unwrap(header).payloadLength == 6);
}

TEST_CASE("DecodeRequestHeader rejects a foreign magic but keeps an unknown opcode")
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

TEST_CASE("DecodeReplyHeader round-trips and rejects an unknown status")
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

TEST_CASE("EncodeProgressReply emits the specified bytes exactly")
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

TEST_CASE("A progress frame is a known status and is never a terminal one")
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

TEST_CASE("Only COMPILE may be answered with a progress pulse")
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

TEST_CASE("The pulse cadence and the client's patience are one pair of numbers")
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

TEST_CASE("DecodeStorePayload round-trips every field, including an empty one")
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
        0x0D,                   // version
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

TEST_CASE("DecodeFetchPayload round-trips the key")
{
    auto const frame = EncodeFetch("the-key");
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const key = DecodeFetchPayload(payload);

    REQUIRE(key.has_value());
    CHECK(AsStringView(Unwrap(key)) == "the-key");
}

TEST_CASE("SplitFields rejects a payload that disagrees with its field lengths")
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

TEST_CASE("DecodeErrorPayload splits the code from the message")
{
    auto const reply = EncodeErrorReply(ErrorCode::PayloadTooLarge, "too big");
    auto const payload = std::span<std::byte const> { reply }.subspan(ReplyHeaderSize);
    auto const decoded = DecodeErrorPayload(payload);

    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).first == ErrorCode::PayloadTooLarge);
    CHECK(Unwrap(decoded).second == "too big");

    CHECK_FALSE(DecodeErrorPayload({}).has_value());
}

TEST_CASE("EncodeErrorReply falls back to the table's default message")
{
    auto const reply = EncodeErrorReply(ErrorCode::StorageWriteFailed);
    auto const decoded = DecodeErrorPayload(std::span<std::byte const> { reply }.subspan(ReplyHeaderSize));

    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).first == ErrorCode::StorageWriteFailed);
    CHECK(Unwrap(decoded).second == "storage write failed");
}

// --- table integrity -------------------------------------------------------

TEST_CASE("Every op descriptor is unique and well-formed")
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

TEST_CASE("Every error descriptor is unique and carries a message")
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

TEST_CASE("A FETCH may miss but a STORE may not")
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

TEST_CASE("IsSupported admits exactly the declared range")
{
    CHECK(IsSupported(CurrentVersion));
    CHECK(IsSupported(MinSupportedVersion));
    CHECK_FALSE(IsSupported(static_cast<WireVersion>(CurrentVersion + 1)));
    CHECK_FALSE(IsSupported(0));
}

// --- AUTH ------------------------------------------------------------------

TEST_CASE("EncodeAuth emits the specified bytes exactly")
{
    auto const frame = EncodeAuth(AuthRequest { .username = "bob", .secret = "hunter2" });

    auto const expected = Bytes({
        0xFC, 0x0D, 0x03,       // magic, version, op=Auth
        0x00, 0x00, 0x00, 0x12, // payload length: (4+3) + (4+7) = 18
        0x00, 0x00, 0x00, 0x03, 'b', 'o', 'b', 0x00, 0x00, 0x00, 0x07, 'h', 'u', 'n', 't', 'e', 'r', '2',
    });
    CHECK(frame == expected);
}

TEST_CASE("DecodeAuthPayload round-trips, including the empty-username form")
{
    // The empty username is the redis `requirepass` spelling and is a legitimate
    // credential, not a malformed one: a launcher configured with only a token
    // sends exactly this. A decoder that rejected it would lock out the common case.
    auto const frame = EncodeAuth(AuthRequest { .username = "", .secret = "s3cret" });
    std::span<std::byte const> const payload = std::span { frame }.subspan(RequestHeaderSize);

    auto const decoded = DecodeAuthPayload(payload);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).username.empty());
    CHECK(AsStringView(Unwrap(decoded).secret) == "s3cret");
}

TEST_CASE("DecodeAuthPayload rejects a payload with the wrong field count")
{
    // A FETCH payload is one field; AUTH demands two. Decoding one as the other
    // must fail rather than silently read the key as a username with no secret.
    auto const fetch = EncodeFetch("some-key");
    std::span<std::byte const> const payload = std::span { fetch }.subspan(RequestHeaderSize);
    CHECK_FALSE(DecodeAuthPayload(payload).has_value());
}

TEST_CASE("Exactly the verbs meant to be reachable before AUTH are reachable")
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

    // The COUNT lives here and nowhere else, so a third verb arriving reddens exactly
    // one case rather than being argued about in two.
    auto const openVerbs = std::ranges::count_if(OpTable, [](auto const& row) { return row.preAuth.Allowed(); });
    CHECK(openVerbs == 2);
}

TEST_CASE("An unknown opcode is never reachable before AUTH")
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

TEST_CASE("Every dispatch verb round-trips its fields")
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

TEST_CASE("A capacity record tolerates a peer that says less, or more")
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

TEST_CASE("A load record tells silence apart from a measured zero")
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

TEST_CASE("A u32 field of the wrong width is rejected, not read")
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

TEST_CASE("A dispatch payload decoded as the wrong verb fails")
{
    // Each verb has its own arity, and SplitFields is strict in both directions, so
    // one verb's payload cannot be silently reinterpreted as another's.
    auto const lease = EncodeLease(LeaseRequest { .fingerprint = "f", .key = "k", .acceptedCodecs = {} });
    auto const payload = std::span<std::byte const> { lease }.subspan(RequestHeaderSize);
    CHECK_FALSE(DecodeRegisterPayload(payload).has_value());
    CHECK_FALSE(DecodeCompilePayload(payload).has_value());
    CHECK_FALSE(DecodeReleasePayload(payload).has_value());
    CHECK(DecodeLeasePayload(payload).has_value());

    // And the other way round: RELEASE carries one field, so a three-field LEASE
    // cannot be read out of it either.
    auto const release = EncodeRelease(ReleaseRequest { .leaseToken = "l1", .key = "k" });
    auto const releasePayload = std::span<std::byte const> { release }.subspan(RequestHeaderSize);
    CHECK_FALSE(DecodeLeasePayload(releasePayload).has_value());
    CHECK(DecodeReleasePayload(releasePayload).has_value());
}

TEST_CASE("A codec envelope round-trips its tag, raw size and bytes")
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

TEST_CASE("An envelope too short to hold a header is rejected")
{
    CHECK_FALSE(DecodeCodecEnvelope({}).has_value());
    CHECK_FALSE(DecodeCodecEnvelope(Bytes({ 0x00, 0x00, 0x00, 0x00 })).has_value());
    CHECK(DecodeCodecEnvelope(Bytes({ 0x00, 0x00, 0x00, 0x00, 0x00 })).has_value());
}

TEST_CASE("An empty payload still travels in a well-formed envelope")
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

TEST_CASE("Codec negotiation prefers the sender's order and falls back to Identity")
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

TEST_CASE("A build with compression disabled still interoperates")
{
    // Such a build offers only Identity and can produce only Identity. Both
    // directions must still resolve, or enabling compression on one machine would
    // break the cache for every machine that has it compiled out.
    CodecList const none { IdentityCodec };
    CHECK(ChooseCodec(none, { 1, 2 }) == IdentityCodec);
    CHECK(ChooseCodec({ 2, 1 }, none) == IdentityCodec);
}

TEST_CASE("A decoder's result outlives the buffer it was decoded from")
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

TEST_CASE("A LEASE grant and a COMPILE result round-trip")
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

TEST_CASE("A WITHDRAW round-trips, and its byte is pinned")
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

TEST_CASE("No distributed verb is reachable before authentication")
{
    // Causing a compiler to run on another machine is the last thing an
    // unauthenticated peer should reach.
    for (auto const op: { Op::Register, Op::Heartbeat, Op::Withdraw, Op::Lease, Op::Compile })
    {
        INFO("op 0x" << static_cast<unsigned>(op));
        CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(op)));
    }
}

TEST_CASE("Every verb states which family it belongs to")
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

TEST_CASE("A byte that names no verb has no family")
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

TEST_CASE("The scheduler's control verbs are bounded well below the session cap")
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

TEST_CASE("An applied-tombstone count survives the wire, and its ABSENCE does too", "[wire][node-status][forget]")
{
    // #1471. The count answers "has my `--cluster-forget-client` reached this machine", so the
    // two readings an operator must be able to tell apart are *this node has no committed
    // tombstone set at all* and *the cluster forgets nobody*. Both are encodable; only one is a
    // number.
    SECTION("a count is carried")
    {
        auto fields = NodeRuntimeFields {};
        fields.forgottenClients = 3;
        fields.registrarsTotal = 7;

        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(fields));
        REQUIRE(back.has_value());
        auto const runtime = Unwrap(back);
        REQUIRE(runtime.forgottenClients.has_value());
        CHECK(Unwrap(runtime.forgottenClients) == 3);
        // A neighbour, so a decoder reading the wrong POSITION cannot pass by returning the
        // right number from the wrong field.
        REQUIRE(runtime.registrarsTotal.has_value());
        CHECK(Unwrap(runtime.registrarsTotal) == 7);
    }

    SECTION("a zero is carried as a zero, not as absence")
    {
        // The direction that gets skipped. `0` is a real reading -- the cluster has agreed no
        // forgets -- and an encoder that treats it as "nothing to say" destroys the distinction
        // the field was added for while every value-carrying case still passes.
        auto fields = NodeRuntimeFields {};
        fields.forgottenClients = 0;

        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(fields));
        REQUIRE(back.has_value());
        auto const runtime = Unwrap(back);
        REQUIRE(runtime.forgottenClients.has_value());
        CHECK(Unwrap(runtime.forgottenClients) == 0);
    }

    SECTION("absence survives, and the fields around it still decode")
    {
        // A node running no consensus says nothing here. The neighbour assertion is what
        // distinguishes this from an implementation that blanked the tail of the record.
        auto fields = NodeRuntimeFields {};
        fields.registrarsTotal = 7;
        REQUIRE_FALSE(fields.forgottenClients.has_value());

        auto const back = DecodeNodeRuntime(EncodeNodeRuntime(fields));
        REQUIRE(back.has_value());
        auto const runtime = Unwrap(back);
        CHECK_FALSE(runtime.forgottenClients.has_value());
        REQUIRE(runtime.registrarsTotal.has_value());
        CHECK(Unwrap(runtime.registrarsTotal) == 7);
    }
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
    CHECK(static_cast<std::uint8_t>(ErrorCode::EnrollmentClosed) == 0x22);
    CHECK(static_cast<std::uint8_t>(ErrorCode::EnrollmentFull) == 0x23);

    // And nothing else answers to them, which a pair of equality checks does not cover:
    // a second row claiming one of these bytes would leave every assertion above true.
    CHECK(std::ranges::count(OpTable, Op::Enroll, &OpDescriptor::code) == 1);
    CHECK(std::ranges::count(OpTable, Op::EnrollControl, &OpDescriptor::code) == 1);
    CHECK(std::ranges::count(ErrorTable, ErrorCode::EnrollmentClosed, &ErrorDescriptor::code) == 1);
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
    auto const frame = EncodeEnroll(EnrollRequest {
        .nodeId = "joiner-a", .raftEndpoint = "10.0.0.9:7100", .role = EnrollRole::Worker, .publicKey = key });
    auto const header = DecodeRequestHeader(frame);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Op::Enroll));

    auto const fields = DecodeEnrollPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
    REQUIRE(fields.has_value());
    CHECK(AsStringView(Unwrap(fields).nodeId) == "joiner-a");
    CHECK(AsStringView(Unwrap(fields).raftEndpoint) == "10.0.0.9:7100");
    CHECK(Unwrap(fields).role == EnrollRole::Worker);
    CHECK(Unwrap(fields).publicKey == key);

    // The role's BYTES, since they travel: a symbol both ends spell tests only the name.
    CHECK(static_cast<std::uint8_t>(EnrollRole::Member) == 0x01);
    CHECK(static_cast<std::uint8_t>(EnrollRole::Worker) == 0x02);

    // Two fields where four are declared -- a version-11 request's shape -- refused on the
    // count alone, which is what keeps a peer speaking a shape this build does not know
    // from being read as a short one.
    auto const id = AsBytes(std::string_view { "joiner-a" });
    auto const endpoint = AsBytes(std::string_view { "10.0.0.9:7100" });
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint })).has_value());

    // A role this build cannot name, and a key that is not 32 bytes, are refused rather than
    // defaulted: a joiner recorded under a key it did not send is admitted under a key nobody
    // holds, and a role guessed is a machine admitted as something it did not ask to be.
    auto const member = std::array { static_cast<std::byte>(EnrollRole::Member) };
    auto const unknownRole = std::array { std::byte { 0x7F } };
    auto const shortKey = std::span<std::byte const> { key }.first(IdentityPublicKeyBytes - 1);
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, unknownRole, key })).has_value());
    CHECK_FALSE(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, member, shortKey })).has_value());
    CHECK(DecodeEnrollPayload(WireFields::Encode({ id, endpoint, member, key })).has_value());
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
    auto const pendingPayload = EncodeEnrollReply(EnrollOutcome::Pending, {});
    auto const pending = DecodeEnrollReply(pendingPayload);
    REQUIRE(pending.has_value());
    CHECK(Unwrap(pending).outcome == EnrollOutcome::Pending);
    CHECK(Unwrap(pending).roster.empty());

    auto const roster = AsBytes(std::string_view { "roster-bytes" });
    auto const approvedPayload = EncodeEnrollReply(EnrollOutcome::Approved, roster);
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
    for (auto const verb: { EnrollControlVerb::Open, EnrollControlVerb::Close, EnrollControlVerb::List })
    {
        auto const frame = EncodeEnrollControl(verb);
        auto const decoded = DecodeEnrollControlPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).verb == verb);
        CHECK(Unwrap(decoded).subject.empty());
    }

    for (auto const verb: { EnrollControlVerb::Approve, EnrollControlVerb::Reject })
    {
        auto const frame = EncodeEnrollControl(verb, "joiner-a");
        auto const decoded = DecodeEnrollControlPayload(std::span<std::byte const> { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).verb == verb);
        CHECK(AsStringView(Unwrap(decoded).subject) == "joiner-a");
    }

    // **Both directions of the arity rule**, because each alone passes under a decoder
    // that checks only the other: a `Close` naming an id, and an `Approve` naming
    // nobody. Answering either by ignoring the mismatch is how an operator comes to
    // believe they approved somebody.
    CHECK_FALSE(DecodeEnrollControlPayload(
                    std::span<std::byte const> { EncodeEnrollControl(EnrollControlVerb::Close, "joiner-a") }.subspan(
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

    EnrollmentReport const report { .state = WireEnrollmentState::Open,
                                    .openForSeconds = 137,
                                    .pending = { EnrollmentPendingEntry { .nodeId = "joiner-a",
                                                                          .raftEndpoint = "10.0.0.9:7100",
                                                                          .peerId = "10.0.0.9",
                                                                          .firstSeenSecondsAgo = 90,
                                                                          .attempts = 45,
                                                                          .claimsChanged = 0,
                                                                          .decision = EnrollmentDecision::Pending,
                                                                          .role = EnrollRole::Member,
                                                                          .publicKey = keyA,
                                                                          .rosterFingerprint = std::nullopt },
                                                 EnrollmentPendingEntry { .nodeId = "joiner-b",
                                                                          .raftEndpoint = {},
                                                                          .peerId = "198.51.100.4",
                                                                          .firstSeenSecondsAgo = 12,
                                                                          .attempts = 6,
                                                                          .claimsChanged = 3,
                                                                          .decision = EnrollmentDecision::Approved,
                                                                          .role = EnrollRole::Worker,
                                                                          .publicKey = keyB,
                                                                          .rosterFingerprint = fingerprint } } };

    auto const back = DecodeEnrollmentReport(EncodeEnrollmentReport(report));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).state == WireEnrollmentState::Open);
    CHECK(Unwrap(back).openForSeconds == 137);
    REQUIRE(Unwrap(back).pending.size() == 2);

    // Every field of a row rather than a sample of them: these differ only by position
    // in the nested record, so a transposed pair would leave any single assertion true.
    // The two hosts particularly, since the whole point of the row is that a person
    // compares them.
    auto const& first = Unwrap(back).pending.front();
    CHECK(first.nodeId == "joiner-a");
    CHECK(first.raftEndpoint == "10.0.0.9:7100");
    CHECK(first.peerId == "10.0.0.9");
    CHECK(first.firstSeenSecondsAgo == 90);
    CHECK(first.attempts == 45);
    CHECK(first.claimsChanged == 0);
    CHECK(first.decision == EnrollmentDecision::Pending);
    CHECK(first.role == EnrollRole::Member);
    CHECK(first.publicKey == keyA);

    // Absent stays ABSENT: a disengaged fingerprint must not come back as thirty-two zero
    // bytes, which would render as a fingerprint the joiner could never have printed.
    CHECK_FALSE(first.rosterFingerprint.has_value());

    // The second row differs from the first in every numeric field, so a transposition
    // between the two u32s -- `attempts` and `claimsChanged` -- cannot pass by both
    // happening to be zero. That pair is the one worth arranging against: they are the
    // same width, adjacent in meaning, and the marks an operator reads sit on one of
    // them.
    auto const& second = Unwrap(back).pending.back();
    CHECK(second.attempts == 6);
    CHECK(second.claimsChanged == 3);
    CHECK(second.decision == EnrollmentDecision::Approved);
    CHECK(second.raftEndpoint.empty());
    CHECK(second.role == EnrollRole::Worker);
    CHECK(second.publicKey == keyB);
    CHECK(second.rosterFingerprint == fingerprint);

    // The decision BYTES, including the hole: `0x03` was `Collected`, retired with the
    // spend (#178), and never reused -- a row still carrying it is refused, because the
    // reader is a person deciding who to admit and a guessed state is the one wrong answer.
    CHECK(static_cast<std::uint8_t>(EnrollmentDecision::Pending) == 0x01);
    CHECK(static_cast<std::uint8_t>(EnrollmentDecision::Approved) == 0x02);
    CHECK(static_cast<std::uint8_t>(EnrollmentDecision::Rejected) == 0x04);
    CHECK_FALSE(IsKnownEnrollmentDecision(0x03));

    // A closed window with nothing waiting is a real reading rather than an empty
    // message, and round-trips as one.
    auto const shut = DecodeEnrollmentReport(EncodeEnrollmentReport(EnrollmentReport {}));
    REQUIRE(shut.has_value());
    CHECK(Unwrap(shut).state == WireEnrollmentState::Closed);
    CHECK(Unwrap(shut).pending.empty());
}

TEST_CASE("A pending row is read at ten facts, a surplus is skipped, and fewer are refused", "[wire][enrollment]")
{
    // **The row's own variable arity.** A row from a build that records one more fact than
    // this one knows about is read for what it does know, exactly as `DecodeNodeRuntime`
    // reads its own record -- so the NEXT fact added to a row moves no version. What moved
    // the floor at #178 was the key, which is not optional: a row without one is a row an
    // operator cannot check, so fewer than ten facts is refused rather than padded.
    auto key = std::array<std::byte, IdentityPublicKeyBytes> {};
    key.fill(std::byte { 0x42 });
    EnrollmentReport const report { .state = WireEnrollmentState::Open,
                                    .openForSeconds = 5,
                                    .pending = { EnrollmentPendingEntry { .nodeId = "joiner-a",
                                                                          .raftEndpoint = "10.0.0.9:7100",
                                                                          .peerId = "10.0.0.9",
                                                                          .firstSeenSecondsAgo = 1,
                                                                          .attempts = 2,
                                                                          .claimsChanged = 7,
                                                                          .decision = EnrollmentDecision::Pending,
                                                                          .role = EnrollRole::Member,
                                                                          .publicKey = key,
                                                                          .rosterFingerprint = std::nullopt } } };

    auto const encoded = EncodeEnrollmentReport(report);
    auto const outer = WireFields::SplitAll(encoded);
    REQUIRE(outer.has_value());
    REQUIRE(Unwrap(outer).size() == 3);
    auto const rows = WireFields::SplitAll(Unwrap(outer)[2]);
    REQUIRE(rows.has_value());
    REQUIRE(Unwrap(rows).size() == 1);
    auto const parts = WireFields::SplitAll(Unwrap(rows).front());
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() == 10);

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

    // NINE -- the key's row without its fingerprint slot -- is refused, not padded: reading a
    // missing field would invent a value rather than omit a fact.
    auto const truncated = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 9 };
    CHECK_FALSE(reportWith(truncated).has_value());

    // And the exact ten is read, which is the control for the refusal above.
    auto const exact = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
    CHECK(reportWith(exact).has_value());
}

TEST_CASE("A node runtime record carries the enrollment window, and absent is not closed", "[wire][enrollment][nodestatus]")
{
    // **Absent is not zero, and here absent is not CLOSED.** A node running no
    // consensus has no window to report on, and a `Closed` there is a reassuring claim
    // about a thing that does not exist -- which is exactly the reading that stops an
    // operator looking.
    auto const silent = DecodeNodeRuntime(EncodeNodeRuntime(NodeRuntimeFields {}));
    REQUIRE(silent.has_value());
    CHECK_FALSE(Unwrap(silent).enrollment.has_value());
    CHECK_FALSE(Unwrap(silent).enrollmentPending.has_value());

    NodeRuntimeFields open {};
    open.enrollment = WireEnrollmentState::Open;
    open.enrollmentPending = 3;
    auto const back = DecodeNodeRuntime(EncodeNodeRuntime(open));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).enrollment == std::optional<WireEnrollmentState> { WireEnrollmentState::Open });
    CHECK(Unwrap(back).enrollmentPending == std::optional<std::uint32_t> { 3 });

    // Zero pending is a READING and not an absence: a window nobody has found yet says
    // zero, and a node with no window says nothing. Both are asserted, because one
    // optional renders them alike to anybody who only checks the engaged case.
    NodeRuntimeFields quiet {};
    quiet.enrollment = WireEnrollmentState::Closed;
    quiet.enrollmentPending = 0;
    auto const none = DecodeNodeRuntime(EncodeNodeRuntime(quiet));
    REQUIRE(none.has_value());
    CHECK(Unwrap(none).enrollment == std::optional<WireEnrollmentState> { WireEnrollmentState::Closed });
    CHECK(Unwrap(none).enrollmentPending == std::optional<std::uint32_t> { 0 });
}

TEST_CASE("An older peer's runtime record reads without the enrollment fields, and a newer one's surplus is skipped",
          "[wire][enrollment][nodestatus]")
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

// --- Live stats (#1399) -----------------------------------------------------

TEST_CASE("Only SUBSCRIBE may be answered with a push frame")
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

TEST_CASE("The live-stats wire bytes are pinned")
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

TEST_CASE("A SUBSCRIBE request round-trips and refuses a subject this build does not know")
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

TEST_CASE("A granted cadence is the request clamped to the subject floor and the ceiling")
{
    CHECK(GrantLiveCadence(LiveSubject::Node, 0) == std::chrono::milliseconds { 500 });
    CHECK(GrantLiveCadence(LiveSubject::Fleet, 200) == std::chrono::milliseconds { 1000 });
    CHECK(GrantLiveCadence(LiveSubject::Cache, 2000) == std::chrono::milliseconds { 2000 });
    CHECK(GrantLiveCadence(LiveSubject::Node, 3'600'000) == MaxLiveCadence);
    CHECK(LiveIdleBound(std::chrono::milliseconds { 500 }) == std::chrono::milliseconds { 1500 });
}

TEST_CASE("Every push kind round-trips through its own decoder")
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

    // The encoding is kept in a local: `SplitAll` hands back spans INTO it. Seven since #1364
    // appended the conditions list; the cuts below are still counted from the cordon's position.
    auto const encoded = EncodeLoad(cordoned);
    auto const parts = WireFields::SplitAll(encoded);
    REQUIRE(parts.has_value());
    REQUIRE(Unwrap(parts).size() == 7);
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

TEST_CASE("A node runtime record carries the cordon state, and absent is not serving", "[wire][cordon][nodestatus]")
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

TEST_CASE("The fleet-text wire bytes are pinned and the verb is a request with a reply")
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

TEST_CASE("A FLEET-TEXT request carries its keys and its token as the words typed")
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
          "[wire][consensus][nodestatus]")
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
          "[wire][consensus][nodestatus]")
{
    // **Why the field costs no wire version**, proved rather than inherited from the
    // enrollment case above: the decoder answers an index past the end as empty and ignores
    // a surplus. Three arities, because a decoder that expected EXACTLY fourteen would pass
    // the round trip and fail both of the others.
    NodeRuntimeFields sent {};
    sent.toolchainsServed = 4;
    sent.cordon = WireCordonState::Draining;
    sent.consensusEndpoint = "10.0.0.4:6680";
    sent.forgottenClients = 2;
    sent.consensusStanding = WireConsensusStanding::Learner;
    sent.conditions = std::vector { NodeConditionFields { .id = "scratch-root-unmappable",
                                                          .persistence = "latched",
                                                          .severity = "warning",
                                                          .state = "raised",
                                                          .detail = "no key",
                                                          .remedy = "name one" } };
    sent.identityPublicKey.emplace();
    sent.identityPublicKey->fill(std::byte { 0xA5 });
    sent.roster = NodeRosterFields { .version = 9, .voters = 3, .principals = 4, .revoked = 1, .certifiedUntilMillis = 42 };
    // Kept in a local: `SplitAll` hands back spans INTO it.
    auto const emitted = EncodeNodeRuntime(sent);
    auto const parts = WireFields::SplitAll(emitted);
    REQUIRE(parts.has_value());
    // Nineteen: thirteen that predate #1328, the endpoint it added, #1471's applied-tombstone
    // count, #1449's consensus standing, #1364's condition list, #178's identity key and the
    // roster #178 certifies. Pinned, since every cut below is counted from it and a record that
    // grew would move what "older" means -- which is how this case caught #1471's append, then
    // #1449's, #1364's and both of #178's, rather than letting any of them shift the cuts
    // silently. #1364 and #178 each appended behind the standing on their own; the integration
    // orders them, the conditions first.
    REQUIRE(Unwrap(parts).size() == 19);

    SECTION("thirteen fields, as a build before #1328 emits: both disengaged, and the cordon still read")
    {
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 13 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).consensusEndpoint.has_value());
        CHECK_FALSE(Unwrap(back).forgottenClients.has_value());
        // The field before it survives the cut, so the cut is where it was meant to be.
        CHECK(Unwrap(back).cordon == std::optional { WireCordonState::Draining });
        CHECK(Unwrap(back).toolchainsServed == 4);
    }

    SECTION("fourteen fields, as a build after #1328 and before #1471 emits: the count is absent")
    {
        // **The cut #1471 has to survive**, and the direction a version-bump argument gets
        // tested in only by accident: a peer that knows the consensus endpoint and has never
        // heard of the tombstone count. Its record is SHORTER, and the count must come back
        // "did not say" rather than taking the reply with it -- which is the whole claim that
        // appending to this record costs no wire version.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 14 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).forgottenClients.has_value());
        // And everything that build DID send is still read, so the cut removed one fact rather
        // than truncating the record.
        CHECK(Unwrap(back).consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
        CHECK(Unwrap(back).cordon == std::optional { WireCordonState::Draining });
    }

    SECTION("fifteen fields, as a build after #1471 and before #1449 emits: the standing is absent")
    {
        // The cut #1449 has to survive, for #1471's reason: a peer that has never heard of a
        // learner answers with a shorter record, and the standing comes back "did not say"
        // rather than taking the reply -- or the tombstone count before it -- with it.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 15 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).consensusStanding.has_value());
        CHECK_FALSE(Unwrap(back).conditions.has_value());
        auto const runtime = Unwrap(back);
        REQUIRE(runtime.forgottenClients.has_value());
        CHECK(Unwrap(runtime.forgottenClients) == 2);
    }

    SECTION("sixteen fields, as a build after #1449 and before #1364 emits: the conditions are absent")
    {
        // The cut #1364 has to survive: a peer that knows the standing and has never heard of
        // conditions. Its list must come back ABSENT -- a node too old to say, which every
        // renderer shows as its absent marker -- and never as an empty list, which would read as a
        // node with nothing raised.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 16 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).conditions.has_value());
        CHECK_FALSE(Unwrap(back).identityPublicKey.has_value());
        auto const runtime = Unwrap(back);
        REQUIRE(runtime.forgottenClients.has_value());
        CHECK(Unwrap(runtime.forgottenClients) == 2);
        CHECK(runtime.consensusStanding == std::optional { WireConsensusStanding::Learner });
    }

    SECTION("seventeen fields, as a build after #1364 and before #178 emits: the key is absent")
    {
        // The cut #178 has to survive, for #1449's reason: a node that has never held a key
        // answers with a shorter record, and the key comes back "did not say" rather than taking
        // the conditions before it -- or the reply -- with it.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 17 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).identityPublicKey.has_value());
        CHECK(Unwrap(back).consensusStanding == std::optional { WireConsensusStanding::Learner });
        CHECK(Unwrap(back).conditions == sent.conditions);
    }

    SECTION("eighteen fields, as a build after the key and before the roster emits: the roster is absent")
    {
        // A node holding no roster answers exactly so, and a record from before the field must
        // read the same: absent, never a roster of nobody.
        auto const older = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).begin() + 18 };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { older }));
        REQUIRE(back.has_value());
        CHECK_FALSE(Unwrap(back).roster.has_value());
        CHECK(Unwrap(back).identityPublicKey == sent.identityPublicKey);
    }

    SECTION("nineteen fields, this build: every fact engaged")
    {
        auto const current = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { current }));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
        auto const runtime = Unwrap(back);
        REQUIRE(runtime.forgottenClients.has_value());
        CHECK(Unwrap(runtime.forgottenClients) == 2);
        CHECK(runtime.consensusStanding == std::optional { WireConsensusStanding::Learner });
        CHECK(runtime.conditions == sent.conditions);
        CHECK(runtime.identityPublicKey == sent.identityPublicKey);
        CHECK(runtime.roster == sent.roster);
    }

    SECTION("twenty fields, from a build ahead of this one: the surplus is skipped")
    {
        auto ahead = std::vector<std::span<std::byte const>> { Unwrap(parts).begin(), Unwrap(parts).end() };
        auto const extra = AsBytes(std::string_view { "a fact from the future" });
        ahead.emplace_back(extra);
        auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { ahead }));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).consensusEndpoint == std::optional<std::string> { "10.0.0.4:6680" });
        auto const runtime = Unwrap(back);
        REQUIRE(runtime.forgottenClients.has_value());
        CHECK(Unwrap(runtime.forgottenClients) == 2);
        CHECK(runtime.consensusStanding == std::optional { WireConsensusStanding::Learner });
        CHECK(runtime.conditions == sent.conditions);
        CHECK(runtime.identityPublicKey == sent.identityPublicKey);
        CHECK(runtime.roster == sent.roster);
    }
}

TEST_CASE("An identity key travels as its 32 bytes, absent as nothing, and any other width refuses the record",
          "[wire][nodestatus][identity]")
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
    // a machine that holds no such key. The key is the eighteenth field, behind #1364's
    // conditions and ahead of the roster.
    for (auto const width: { std::size_t { 31 }, std::size_t { 33 } })
    {
        auto emitted = EncodeNodeRuntime(NodeRuntimeFields {});
        auto parts = Unwrap(WireFields::SplitAll(emitted));
        REQUIRE(parts.size() == 19);
        auto const wrong = std::vector<std::byte>(width, std::byte { 0x11 });
        parts[17] = wrong;
        CHECK_FALSE(DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts })).has_value());
    }
}

TEST_CASE("A consensus standing travels as its pinned byte, and one this build cannot name is skipped",
          "[wire][consensus][nodestatus][learner]")
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
    sent.forgottenClients = 3;
    auto emitted = EncodeNodeRuntime(sent);
    auto parts = Unwrap(WireFields::SplitAll(emitted));
    // Nineteen since #1364 and #178 appended the condition list, the identity key and the roster
    // behind the standing; the standing is still the sixteenth field, so the byte replaced below
    // is still the one under test.
    REQUIRE(parts.size() == 19);
    auto const unknown = std::array { std::byte { 0x7F } };
    parts[15] = unknown;
    auto const back = DecodeNodeRuntime(WireFields::Encode(WireFields::FieldList { parts }));
    REQUIRE(back.has_value());
    CHECK_FALSE(Unwrap(back).consensusStanding.has_value());
    CHECK(Unwrap(back).forgottenClients == std::optional<std::uint32_t> { 3 });
}

// --- Explaining an admission (#1471) ----------------------------------------

TEST_CASE("The explain-admission verb occupies the byte it was assigned, in the node family", "[wire][admission]")
{
    // The value as well as the name, for the cordon byte's reason: a consistent
    // renumbering keeps every in-tree test agreeing while a deployed CLI breaks.
    CHECK(static_cast<std::uint8_t>(Op::ExplainAdmission) == 0x1B);
    CHECK(std::ranges::count(OpTable, Op::ExplainAdmission, &OpDescriptor::code) == 1);

    // The node family, because the question is about THIS node's own fold rather than
    // about the cluster's agreed state -- so a node running no consensus still answers it.
    CHECK(FamilyOf(static_cast<std::uint8_t>(Op::ExplainAdmission)) == VerbFamily::Node);
    // Never pre-auth: the answer describes who this node admits, which is the one thing a
    // caller it does not admit must not be able to read.
    CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(Op::ExplainAdmission)));

    // And the bytes a client of another build reads back.
    CHECK(static_cast<std::uint8_t>(WireMembership::Outsider) == 0x01);
    CHECK(static_cast<std::uint8_t>(WireMembership::Member) == 0x02);
    CHECK(static_cast<std::uint8_t>(WireMembership::Forgotten) == 0x03);
    CHECK(WireMembershipRoute::FleetMemberList == 0x01);
    CHECK(WireMembershipRoute::ClusterMembers == 0x02);
    CHECK(WireMembershipRoute::ClientTombstone == 0x04);
    CHECK(WireMembershipRoute::OpenPolicy == 0x08);
    CHECK(WireMembershipRoute::ProvenKeyHolder == 0x10);
}

TEST_CASE("An explain-admission request carries exactly one host, and anything else is refused", "[wire][admission]")
{
    auto const frame = EncodeExplainAdmissionRequest("10.0.0.42");
    auto const header = DecodeRequestHeader(frame);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == 0x1B);
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    CHECK(DecodeExplainAdmissionPayload(payload) == std::optional<std::string> { "10.0.0.42" });

    // An EMPTY host is a legal field and is not the same as no field: the node decides
    // what to make of it, and a decoder that refused here would refuse a frame whose
    // arity is exactly right.
    CHECK(DecodeExplainAdmissionPayload(
              std::span<std::byte const> { EncodeExplainAdmissionRequest("") }.subspan(RequestHeaderSize))
          == std::optional<std::string> { "" });

    // Two fields is a different question arriving under this verb's name -- refused
    // rather than answered about the first of them.
    auto const first = AsBytes(std::string_view { "10.0.0.42" });
    auto const second = AsBytes(std::string_view { "10.0.0.43" });
    CHECK_FALSE(DecodeExplainAdmissionPayload(WireFields::Encode({ first, second })).has_value());
    CHECK_FALSE(DecodeExplainAdmissionPayload({}).has_value());
}

TEST_CASE("An admission explanation round-trips every verdict and the whole route set", "[wire][admission]")
{
    // A different route set per verdict, so an encoder that dropped either half cannot
    // agree with all three.
    for (auto const& sent: { AdmissionExplanationFields { .verdict = WireMembership::Member,
                                                          .decidedBy = WireMembershipRoute::FleetMemberList
                                                                       | WireMembershipRoute::ClusterMembers },
                             AdmissionExplanationFields { .verdict = WireMembership::Forgotten,
                                                          .decidedBy = WireMembershipRoute::ClientTombstone },
                             // The silence: refused, and no route claims authorship. Zero is the READING
                             // here rather than a missing field.
                             AdmissionExplanationFields { .verdict = WireMembership::Outsider, .decidedBy = 0 } })
    {
        auto const back = DecodeAdmissionExplanation(EncodeAdmissionExplanation(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back) == sent);
    }
}

TEST_CASE("An unknown VERDICT is refused and an unknown ROUTE is kept, which is not one rule twice", "[wire][admission]")
{
    auto const routes = WireFields::ToBigEndian<std::uint32_t>(WireMembershipRoute::FleetMemberList);

    SECTION("a verdict byte this build cannot name is refused, never read as a refusal nobody authored")
    {
        // Falling back to `Outsider` would report a host as refused-by-nobody on a build
        // that had learned a fourth answer -- which reads exactly like the healthy case.
        auto const unnamed = std::array { std::byte { 0x7F } };
        CHECK_FALSE(DecodeAdmissionExplanation(WireFields::Encode({ std::span<std::byte const> { unnamed },
                                                                    std::span<std::byte const> { routes } }))
                        .has_value());
        CHECK_FALSE(DecodeAdmissionExplanation({}).has_value());
    }

    SECTION("a route bit this build cannot name is KEPT, so authorship is not under-reported")
    {
        // The opposite decision, deliberately: the routes are a SET, so a bit this build
        // cannot name still says *something decided*. Dropping it would say fewer things
        // decided this than did, on a fleet mid-upgrade -- and the reader can tell,
        // because the bit it does not know is still there to count.
        constexpr auto ahead = std::uint32_t { 0x8000'0000 };
        auto const sent = AdmissionExplanationFields { .verdict = WireMembership::Member,
                                                       .decidedBy = WireMembershipRoute::FleetMemberList | ahead };
        auto const back = DecodeAdmissionExplanation(EncodeAdmissionExplanation(sent));
        REQUIRE(back.has_value());
        CHECK(Unwrap(back).decidedBy == sent.decidedBy);
        CHECK((Unwrap(back).decidedBy & ahead) == ahead);
    }

    SECTION("a verdict field that is not one byte is refused rather than read from its first")
    {
        auto const wide = std::array { std::byte { 0x02 }, std::byte { 0x02 } };
        CHECK_FALSE(DecodeAdmissionExplanation(
                        WireFields::Encode({ std::span<std::byte const> { wide }, std::span<std::byte const> { routes } }))
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
    CHECK_FALSE(IsMemberAdmission(Op::ClusterAdmitClient));
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
    // reads the byte, not the name. Version 11 made the admission three fields; 12 is this
    // build's, moved again by ENROLL (#178 PR 4), and the byte is pinned at what is SENT.
    CHECK(OpFieldCount(Op::ClusterAdmit) == 3);
    CHECK(OpFieldCount(Op::ClusterAdmitLearner) == 3);

    auto const request =
        ClusterAdmitRequest { .memberId = "n4", .raftEndpoint = "10.0.0.4:6680", .publicKey = std::nullopt };
    for (auto const& frame:
         { EncodeClusterAdmit<Op::ClusterAdmit>(request), EncodeClusterAdmit<Op::ClusterAdmitLearner>(request) })
    {
        REQUIRE(frame.size() > RequestHeaderSize);
        CHECK(std::to_integer<unsigned>(frame[0]) == 0xFC);
        CHECK(std::to_integer<unsigned>(frame[1]) == 13);

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

TEST_CASE("NODE-ANNOUNCE carries a voter's endorsement as its fourth field, empty when there is none", "[wire][roster]")
{
    // #178. The endorsement is opaque here -- the scheduler decodes and verifies it -- and the
    // arity is exact whether or not there is one: absent is a zero-length FOURTH field, never a
    // three-field payload an older reader would have accepted.
    CHECK(OpFieldCount(Op::NodeAnnounce) == 4);
    auto const endorsement = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x03 } };

    for (auto const& carried: { std::span<std::byte const> {}, std::span<std::byte const> { endorsement } })
    {
        auto const frame = EncodeNodeAnnounce(
            NodeAnnounceRequest { .endpoint = "10.0.0.2:6674", .capacity = {}, .load = {}, .endorsement = carried });
        REQUIRE(frame.size() > RequestHeaderSize);
        CHECK(std::to_integer<unsigned>(frame[1]) == 13);
        auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
        REQUIRE(WireFields::SplitExactly(payload, 4).has_value());

        auto const decoded = DecodeNodeAnnouncePayload(payload);
        REQUIRE(decoded.has_value());
        CHECK(std::ranges::equal(Unwrap(decoded).endorsement, carried));
    }
}

TEST_CASE("roster-expired is its own wire code, pinned as a byte", "[wire][roster]")
{
    // #178: a worker whose roster is absent or has lapsed refuses every grant with it. A client
    // reads it as any refused compile -- compile locally -- and an operator reads the worker's
    // counters for which of the two it was.
    CHECK(static_cast<std::uint8_t>(ErrorCode::RosterExpired) == 0x28);
    auto const* const row = Describe(ErrorCode::RosterExpired);
    REQUIRE(row != nullptr);
    CHECK(row->name == "roster-expired");
}

TEST_CASE("A node's roster travels in its runtime record, absent when it holds none", "[wire][roster]")
{
    // Field 18 of the nested record (#178). Absent and a roster of nobody are different
    // answers, and a consensus member's roster has no lapse to report -- absent at the field.
    auto runtime = NodeRuntimeFields {};
    SECTION("absent")
    {
        auto const decoded = DecodeNodeRuntime(EncodeNodeRuntime(runtime));
        REQUIRE(decoded.has_value());
        CHECK_FALSE(Unwrap(decoded).roster.has_value());
    }
    SECTION("a worker's, certified until an instant")
    {
        runtime.roster = NodeRosterFields {
            .version = 7, .voters = 3, .principals = 2, .revoked = 1, .certifiedUntilMillis = 1'767'225'600'000ULL
        };
        auto const decoded = DecodeNodeRuntime(EncodeNodeRuntime(runtime));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).roster == runtime.roster);
    }
    SECTION("a consensus member's, with no lapse")
    {
        runtime.roster = NodeRosterFields {
            .version = 7, .voters = 3, .principals = 0, .revoked = 0, .certifiedUntilMillis = std::nullopt
        };
        auto const decoded = DecodeNodeRuntime(EncodeNodeRuntime(runtime));
        REQUIRE(decoded.has_value());
        REQUIRE(Unwrap(decoded).roster.has_value());
        CHECK_FALSE(Unwrap(Unwrap(decoded).roster).certifiedUntilMillis.has_value());
    }
}

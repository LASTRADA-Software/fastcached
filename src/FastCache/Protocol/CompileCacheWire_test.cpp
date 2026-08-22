// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::CompileCacheWire;

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

/// Unwrap an optional for assertion, yielding a default-constructed value when
/// empty.
///
/// clang-tidy's optional analysis cannot see a `has_value()` guard through
/// Catch2's REQUIRE macro, so a direct `*x` or `x.value()` after one is reported
/// as an unchecked access. Going through `value_or` is provably safe, and the
/// preceding REQUIRE still fails the test first when the optional is empty — so
/// the default is never actually observed.
template <typename T>
[[nodiscard]] T Unwrap(std::optional<T> const& value)
{
    return value.value_or(T {});
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
    CHECK(CurrentVersion == 1);
    CHECK(MinSupportedVersion == 1);
    CHECK(RequestHeaderSize == 7);
    CHECK(ReplyHeaderSize == 5);

    CHECK(static_cast<std::uint8_t>(Op::Store) == 0x01);
    CHECK(static_cast<std::uint8_t>(Op::Fetch) == 0x02);

    CHECK(static_cast<std::uint8_t>(Status::Miss) == 0x00);
    CHECK(static_cast<std::uint8_t>(Status::Ok) == 0x01);
    CHECK(static_cast<std::uint8_t>(Status::Error) == 0x02);
}

TEST_CASE("EncodeFetch emits the specified bytes exactly")
{
    auto const frame = EncodeFetch("ab");

    // clang-format off: the grid IS the specification -- one wire field per row.
    auto const expected = Bytes({
        0xFC,                   // magic
        0x01,                   // version
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
        0x01,                               // version
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
        CHECK(row.fieldCount > 0);
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
        0xFC, 0x01, 0x03,       // magic, version, op=Auth
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

    auto const openVerbs = std::ranges::count_if(OpTable, [](auto const& row) { return row.preAuth; });
    CHECK(openVerbs == 1);
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
        auto const frame = EncodeRegister(RegisterRequest {
            .fingerprint = "gcc-13-abc", .endpoint = "10.0.0.1:6676", .slots = 8, .acceptedCodecs = { 2, 1 } });
        auto const decoded = DecodeRegisterPayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).fingerprint) == "gcc-13-abc");
        CHECK(AsStringView(Unwrap(decoded).endpoint) == "10.0.0.1:6676");
        CHECK(Unwrap(decoded).slots == 8);
        CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 2, 1 });
    }
    SECTION("HEARTBEAT")
    {
        auto const frame = EncodeHeartbeat("w7", 3);
        auto const decoded = DecodeHeartbeatPayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).workerId) == "w7");
        CHECK(Unwrap(decoded).inFlight == 3);
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
    SECTION("COMPILE")
    {
        auto const args = Bytes({ 0x01, 0x02 });
        auto const source = Bytes({ 0xAA, 0xBB, 0xCC });
        auto const frame = EncodeCompile(CompileRequest {
            .leaseToken = "l1", .fingerprint = "gcc-13-abc", .args = args, .source = source, .acceptedCodecs = { 1 } });
        auto const decoded = DecodeCompilePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).leaseToken) == "l1");
        CHECK(std::ranges::equal(Unwrap(decoded).args, args));
        CHECK(std::ranges::equal(Unwrap(decoded).source, source));
        CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 1 });
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
    CHECK(DecodeLeasePayload(payload).has_value());
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

TEST_CASE("A LEASE grant and a COMPILE result round-trip")
{
    SECTION("grant")
    {
        auto const payload =
            EncodeLeaseGrant(LeaseGrant { .endpoint = "10.0.0.2:6676", .leaseToken = "l42", .workerCodecs = { 2, 1 } });
        auto const decoded = DecodeLeaseGrant(payload);
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).endpoint) == "10.0.0.2:6676");
        CHECK(AsStringView(Unwrap(decoded).leaseToken) == "l42");
        // Relayed from the worker's registration so the client can pick a codec for
        // the preprocessed payload without a negotiation round trip.
        CHECK(Unwrap(decoded).workerCodecs == CodecList { 2, 1 });
    }
    SECTION("result")
    {
        auto const object = Bytes({ 0x7F, 0x45, 0x4C, 0x46 });
        auto const payload = EncodeCompileResult(
            CompileResult { .exitCode = 0, .object = object, .stdoutText = AsBytes("out"), .stderrText = AsBytes("err") });
        auto const decoded = DecodeCompileResult(payload);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).exitCode == 0);
        CHECK(std::ranges::equal(Unwrap(decoded).object, object));
        CHECK(AsStringView(Unwrap(decoded).stdoutText) == "out");
        CHECK(AsStringView(Unwrap(decoded).stderrText) == "err");
    }
    SECTION("a failed compile carries its diagnostics and no object")
    {
        auto const payload = EncodeCompileResult(
            CompileResult { .exitCode = 1, .object = {}, .stdoutText = {}, .stderrText = AsBytes("error: nope") });
        auto const decoded = DecodeCompileResult(payload);
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).exitCode == 1);
        CHECK(Unwrap(decoded).object.empty());
        CHECK(AsStringView(Unwrap(decoded).stderrText) == "error: nope");
    }
}

TEST_CASE("No distributed verb is reachable before authentication")
{
    // Causing a compiler to run on another machine is the last thing an
    // unauthenticated peer should reach.
    for (auto const op: { Op::Register, Op::Heartbeat, Op::Lease, Op::Compile })
    {
        INFO("op 0x" << static_cast<unsigned>(op));
        CHECK_FALSE(IsPreAuthAllowed(static_cast<std::uint8_t>(op)));
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

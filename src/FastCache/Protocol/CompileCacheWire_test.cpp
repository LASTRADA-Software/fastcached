// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
        .key = "k", .cohort = "", .srcRoot = "s", .buildTree = "b", .value = std::span<std::byte const> { value } });

    auto const expected = Bytes({
        0xFC,                               // magic
        0x01,                               // version
        0x01,                               // op = Store
        0x00, 0x00, 0x00, 0x19,             // payloadLength = 25 = (4+1) + (4+0) + (4+1) + (4+1) + (4+2)
        0x00, 0x00, 0x00, 0x01, 0x6B,       // key       = "k"
        0x00, 0x00, 0x00, 0x00,             // cohort    = "" (empty, still length-prefixed)
        0x00, 0x00, 0x00, 0x01, 0x73,       // srcRoot   = "s"
        0x00, 0x00, 0x00, 0x01, 0x62,       // buildTree = "b"
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
    CHECK(header->version == CurrentVersion);
    CHECK(header->opRaw == static_cast<std::uint8_t>(Op::Fetch));
    CHECK(header->payloadLength == 6);
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
        CHECK(header->opRaw == 0xEE);
        CHECK(FindOp(header->opRaw) == nullptr);
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
    CHECK(header->status == Status::Ok);
    CHECK(header->payloadLength == 3);

    reply[0] = std::byte { 0x7F };
    CHECK_FALSE(DecodeReplyHeader(std::span<std::byte const> { reply }.first(ReplyHeaderSize)).has_value());
}

// --- payload splitting -----------------------------------------------------

TEST_CASE("DecodeStorePayload round-trips every field, including an empty one")
{
    // An empty cohort is the routine case, not an edge one: the launcher stores
    // with no cohort whenever grouping is off, and the handler branches on it.
    auto const value = Bytes({ 0xDE, 0xAD, 0xBE, 0xEF });
    auto const frame = EncodeStore(StoreRequest { .key = "the-key",
                                                  .cohort = "",
                                                  .srcRoot = "/src",
                                                  .buildTree = "/build",
                                                  .value = std::span<std::byte const> { value } });

    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const view = DecodeStorePayload(payload);

    REQUIRE(view.has_value());
    CHECK(AsStringView(view->key) == "the-key");
    CHECK(view->cohort.empty());
    CHECK(AsStringView(view->srcRoot) == "/src");
    CHECK(AsStringView(view->buildTree) == "/build");
    CHECK(std::ranges::equal(view->value, value));
}

TEST_CASE("DecodeFetchPayload round-trips the key")
{
    auto const frame = EncodeFetch("the-key");
    auto const payload = std::span<std::byte const> { frame }.subspan(RequestHeaderSize);
    auto const key = DecodeFetchPayload(payload);

    REQUIRE(key.has_value());
    CHECK(AsStringView(*key) == "the-key");
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
        auto const fields = SplitFields(payload, 1);
        REQUIRE(fields.has_value());
        REQUIRE(fields->size() == 1);
        CHECK(AsStringView((*fields)[0]) == "ab");
    }
}

TEST_CASE("DecodeErrorPayload splits the code from the message")
{
    auto const reply = EncodeErrorReply(ErrorCode::PayloadTooLarge, "too big");
    auto const payload = std::span<std::byte const> { reply }.subspan(ReplyHeaderSize);
    auto const decoded = DecodeErrorPayload(payload);

    REQUIRE(decoded.has_value());
    CHECK(decoded->first == ErrorCode::PayloadTooLarge);
    CHECK(decoded->second == "too big");

    CHECK_FALSE(DecodeErrorPayload({}).has_value());
}

TEST_CASE("EncodeErrorReply falls back to the table's default message")
{
    auto const reply = EncodeErrorReply(ErrorCode::StorageWriteFailed);
    auto const decoded = DecodeErrorPayload(std::span<std::byte const> { reply }.subspan(ReplyHeaderSize));

    REQUIRE(decoded.has_value());
    CHECK(decoded->first == ErrorCode::StorageWriteFailed);
    CHECK(decoded->second == "storage write failed");
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

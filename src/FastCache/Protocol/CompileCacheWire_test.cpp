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
        auto const frame = EncodeHeartbeat(
            "w7",
            3,
            LoadFields { .cpuBusyPermille = 640, .availableMemoryBytes = 8589934592, .freeScratchBytes = 274877906944 });
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
    SECTION("COMPILE")
    {
        auto const args = Bytes({ 0x01, 0x02 });
        auto const source = Bytes({ 0xAA, 0xBB, 0xCC });
        auto const frame = EncodeCompile(CompileRequest { .leaseToken = "l1",
                                                          .fingerprint = "gcc-13-abc",
                                                          .args = args,
                                                          .source = source,
                                                          .acceptedCodecs = { 1 },
                                                          .sourceName = "Widget.cpp" });
        auto const decoded = DecodeCompilePayload(std::span { frame }.subspan(RequestHeaderSize));
        REQUIRE(decoded.has_value());
        CHECK(AsStringView(Unwrap(decoded).leaseToken) == "l1");
        CHECK(std::ranges::equal(Unwrap(decoded).args, args));
        CHECK(std::ranges::equal(Unwrap(decoded).source, source));
        CHECK(Unwrap(decoded).acceptedCodecs == CodecList { 1 });
        CHECK(AsStringView(Unwrap(decoded).sourceName) == "Widget.cpp");
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
            "w1", 0, LoadFields { .cpuBusyPermille = 0, .availableMemoryBytes = std::nullopt, .freeScratchBytes = 0 });
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

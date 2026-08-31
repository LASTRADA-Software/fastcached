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
    CHECK(CurrentVersion == 2);
    CHECK(MinSupportedVersion == 2);
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
        0x02,                   // version
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
        0x02,                               // version
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
        0xFC, 0x02, 0x03,       // magic, version, op=Auth
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

    auto const openVerbs = std::ranges::count_if(OpTable, [](auto const& row) { return row.preAuth.Allowed(); });
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

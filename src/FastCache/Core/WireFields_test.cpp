// SPDX-License-Identifier: Apache-2.0
//
// The length-prefixed field grammar, tested directly rather than only through
// the protocols that frame it. It was previously exercised only via
// CompileCacheWire's own cases, which is enough to catch a change that breaks
// that protocol and not enough to catch one that breaks a *second* consumer —
// which is precisely what lifting it into Core/ created.
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace std::string_view_literals;

namespace
{

/// Unwrap an optional for assertion, yielding a default-constructed value when
/// empty.
///
/// The same device the other test files use: clang-tidy's optional analysis
/// cannot see a `has_value()` guard through Catch2's REQUIRE macro, so a direct
/// `*x` after one reads as an unchecked access. `value_or` is provably safe, and
/// the preceding REQUIRE still fails the test first when the optional is empty.
/// @param value The optional to read.
/// @return Its value, or a default-constructed one.
template <typename T>
[[nodiscard]] T Unwrap(std::optional<T> const& value)
{
    return value.value_or(T {});
}

/// A field list built from text, since text is what most fields carry.
/// @param texts The field contents, in wire order.
/// @return Spans over `texts`; the caller must keep it alive.
[[nodiscard]] std::vector<std::span<std::byte const>> Fields(std::span<std::string_view const> texts)
{
    std::vector<std::span<std::byte const>> out;
    out.reserve(texts.size());
    for (auto const& text: texts)
        out.push_back(WireFields::AsBytes(text));
    return out;
}

} // namespace

TEST_CASE("A field list round-trips through the grammar", "[core][wirefields]")
{
    auto const texts = std::array { "alpha"sv, ""sv, "gamma"sv };
    auto const fields = Fields(texts);
    auto const encoded = WireFields::Encode(fields);

    // Four bytes of prefix per field, plus the contents.
    CHECK(encoded.size() == (4 + 5) + (4 + 0) + (4 + 5));

    auto const split = WireFields::SplitExactly(encoded, texts.size());
    REQUIRE(split.has_value());
    auto const parsed = Unwrap(split);
    REQUIRE(parsed.size() == texts.size());
    for (auto index = std::size_t { 0 }; index < texts.size(); ++index)
        CHECK(WireFields::AsStringView(parsed[index]) == texts[index]);
}

TEST_CASE("An empty field is not the same as an absent one", "[core][wirefields]")
{
    // The distinction the length prefix exists to preserve: a zero-length field
    // still occupies its four bytes, so a message with an optional field that
    // happens to be empty keeps its arity and stays decodable.
    auto const texts = std::array { ""sv, ""sv };
    auto const encoded = WireFields::Encode(Fields(texts));
    CHECK(encoded.size() == 8);

    auto const split = WireFields::SplitExactly(encoded, 2);
    REQUIRE(split.has_value());
    CHECK(Unwrap(split)[0].empty());
    CHECK(Unwrap(split)[1].empty());

    // ... and it is distinguishable from a payload holding one field.
    CHECK_FALSE(WireFields::SplitExactly(encoded, 1).has_value());
}

TEST_CASE("Encoding with no fields yields no bytes", "[core][wirefields]")
{
    auto const encoded = WireFields::Encode({});
    CHECK(encoded.empty());

    // And splitting nothing into nothing succeeds rather than failing: a message
    // whose payload is genuinely empty is well-formed.
    auto const split = WireFields::SplitExactly(encoded, 0);
    REQUIRE(split.has_value());
    CHECK(Unwrap(split).empty());
}

TEST_CASE("The length prefix is big-endian", "[core][wirefields]")
{
    // Pinned against a literal rather than a round trip. Host order would round
    // trip perfectly on one machine and produce a frame the peer cannot read,
    // which is the failure a round-trip test cannot see.
    auto const texts = std::array { "ab"sv };
    auto const encoded = WireFields::Encode(Fields(texts));

    REQUIRE(encoded.size() == 6);
    CHECK(encoded[0] == std::byte { 0x00 });
    CHECK(encoded[1] == std::byte { 0x00 });
    CHECK(encoded[2] == std::byte { 0x00 });
    CHECK(encoded[3] == std::byte { 0x02 });
    CHECK(encoded[4] == std::byte { 'a' });
    CHECK(encoded[5] == std::byte { 'b' });
}

TEST_CASE("SplitExactly rejects a payload that does not match its declared shape", "[core][wirefields]")
{
    auto const texts = std::array { "alpha"sv, "beta"sv };
    auto const encoded = WireFields::Encode(Fields(texts));

    SECTION("a trailing byte after the last field")
    {
        // Rejected rather than ignored. The frame's own declared length and
        // these per-field lengths are redundant on purpose, and a disagreement
        // between them means the sender and this parser do not agree on the
        // message -- which must be answered, not guessed past.
        auto padded = encoded;
        padded.push_back(std::byte { 0 });
        CHECK_FALSE(WireFields::SplitExactly(padded, 2).has_value());
    }

    SECTION("fewer fields than declared")
    {
        CHECK_FALSE(WireFields::SplitExactly(encoded, 3).has_value());
    }

    SECTION("more fields than declared")
    {
        CHECK_FALSE(WireFields::SplitExactly(encoded, 1).has_value());
    }

    SECTION("a truncated length prefix")
    {
        auto truncated = encoded;
        truncated.resize(encoded.size() - 6); // cuts into the second prefix
        CHECK_FALSE(WireFields::SplitExactly(truncated, 2).has_value());
    }

    SECTION("a length running past the end")
    {
        auto lying = encoded;
        lying[3] = std::byte { 0xFF };
        CHECK_FALSE(WireFields::SplitExactly(lying, 2).has_value());
    }
}

TEST_CASE("SplitAll reads however many fields are present", "[core][wirefields]")
{
    // The variable-arity case, which is what a repeated group needs -- a Raft
    // AppendEntries carries an entry list whose length is the payload itself.
    for (auto const count: { std::size_t { 0 }, std::size_t { 1 }, std::size_t { 5 } })
    {
        std::vector<std::string_view> texts(count, "x"sv);
        auto const encoded = WireFields::Encode(Fields(texts));

        auto const split = WireFields::SplitAll(encoded);
        REQUIRE(split.has_value());
        CHECK(Unwrap(split).size() == count);
    }
}

TEST_CASE("SplitAll still rejects a malformed payload", "[core][wirefields]")
{
    // Variable arity is not permissiveness: the per-field framing is checked
    // exactly as SplitExactly checks it, so a truncated tail is a decode failure
    // rather than a silently short list.
    auto const texts = std::array { "alpha"sv, "beta"sv };
    auto const encoded = WireFields::Encode(Fields(texts));

    SECTION("a truncated length prefix")
    {
        auto truncated = encoded;
        truncated.resize(encoded.size() - 6);
        CHECK_FALSE(WireFields::SplitAll(truncated).has_value());
    }

    SECTION("a length running past the end")
    {
        auto lying = encoded;
        lying[3] = std::byte { 0xFF };
        CHECK_FALSE(WireFields::SplitAll(lying).has_value());
    }
}

TEST_CASE("EncodeInto leaves room for a caller's frame header", "[core][wirefields]")
{
    // The property that keeps a protocol from encoding its payload separately
    // and then copying it in behind a header -- which on a compile-cache STORE
    // would mean an extra copy of a whole object file.
    auto const texts = std::array { "alpha"sv, "beta"sv };
    auto const fields = Fields(texts);

    constexpr std::size_t HeaderSize = 7;
    auto const payloadSize = WireFields::RequireEncodable(fields);

    std::vector<std::byte> frame(HeaderSize + payloadSize);
    frame[0] = std::byte { 0xFC };
    WireFields::EncodeInto(frame, HeaderSize, fields);

    CHECK(frame[0] == std::byte { 0xFC });

    // The payload behind the header decodes exactly as a standalone one does.
    auto const standalone = WireFields::Encode(fields);
    CHECK(std::vector<std::byte>(frame.begin() + static_cast<std::ptrdiff_t>(HeaderSize), frame.end()) == standalone);

    auto const split = WireFields::SplitExactly(std::span { frame }.subspan(HeaderSize), 2);
    REQUIRE(split.has_value());
    CHECK(WireFields::AsStringView(Unwrap(split)[0]) == "alpha");
}

TEST_CASE("RequireEncodable reports the exact size EncodeInto will write", "[core][wirefields]")
{
    // Stated as a property rather than a number, because the two being equal is
    // what makes a caller's single allocation correct. A RequireEncodable that
    // over-reported would leave uninitialised tail bytes inside a declared
    // length -- readable garbage on the wire.
    auto const texts = std::array { "a"sv, "bb"sv, "ccc"sv, ""sv };
    auto const fields = Fields(texts);
    CHECK(WireFields::RequireEncodable(fields) == WireFields::Encode(fields).size());
    CHECK(WireFields::RequireEncodable(fields) == WireFields::EncodedSize(fields));
}

TEST_CASE("A wire integer round-trips as a fixed-width field", "[core][wirefields]")
{
    SECTION("every supported width")
    {
        // The expected values are explicitly typed. An untyped literal is `int`,
        // and comparing one against `std::optional<std::uint8_t>` is a
        // signed/unsigned mismatch that MSVC reports from inside <optional>,
        // where it reads as a standard-library defect rather than as this line.
        CHECK(WireFields::FromBigEndian<std::uint8_t>(WireFields::ToBigEndian<std::uint8_t>(0x12)) == std::uint8_t { 0x12 });
        CHECK(WireFields::FromBigEndian<std::uint16_t>(WireFields::ToBigEndian<std::uint16_t>(0x1234))
              == std::uint16_t { 0x1234 });
        CHECK(WireFields::FromBigEndian<std::uint32_t>(WireFields::ToBigEndian<std::uint32_t>(0x1234'5678))
              == std::uint32_t { 0x1234'5678 });
        CHECK(WireFields::FromBigEndian<std::uint64_t>(WireFields::ToBigEndian<std::uint64_t>(0x0123'4567'89AB'CDEF))
              == std::uint64_t { 0x0123'4567'89AB'CDEF });
    }

    SECTION("the byte order is big-endian")
    {
        auto const bytes = WireFields::ToBigEndian<std::uint64_t>(0x0123'4567'89AB'CDEF);
        REQUIRE(bytes.size() == 8);
        CHECK(bytes[0] == std::byte { 0x01 });
        CHECK(bytes[7] == std::byte { 0xEF });
    }

    SECTION("a field of the wrong width is refused rather than misread")
    {
        // A peer's malformed frame, so a recoverable decode failure and never an
        // assertion. Reading a u64 out of a four-byte field would otherwise walk
        // off the end of the payload.
        auto const narrow = WireFields::ToBigEndian<std::uint32_t>(1);
        CHECK_FALSE(WireFields::FromBigEndian<std::uint64_t>(narrow).has_value());

        auto const wide = WireFields::ToBigEndian<std::uint64_t>(1);
        CHECK_FALSE(WireFields::FromBigEndian<std::uint32_t>(wide).has_value());

        CHECK_FALSE(WireFields::FromBigEndian<std::uint32_t>({}).has_value());
    }
}

TEST_CASE("Text survives the byte reinterpretation unchanged", "[core][wirefields]")
{
    // Including an embedded NUL, because a field is length-delimited rather than
    // NUL-terminated and a caller may legitimately put one in.
    auto const text = "a\0b"sv;
    REQUIRE(text.size() == 3);
    CHECK(WireFields::AsStringView(WireFields::AsBytes(text)) == text);
}

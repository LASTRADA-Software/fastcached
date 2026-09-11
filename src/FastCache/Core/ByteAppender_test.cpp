// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ByteAppender.hpp>
#include <FastCache/Core/ByteCursor.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

/// A buffer built from readable numbers rather than hex literals.
[[nodiscard]] std::vector<std::byte> Bytes(std::vector<unsigned> const& values)
{
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (auto const v: values)
        out.push_back(static_cast<std::byte>(v & 0xFFU));
    return out;
}

/// The bytes of a `std::string` buffer, so the two instantiations compare directly.
[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text)
{
    auto const view = AsBytes(text);
    return std::vector<std::byte> { view.begin(), view.end() };
}

} // namespace

TEST_CASE("ByteAppender writes wire integers big-endian", "[core][byteappender]")
{
    // The assertion that distinguishes: a round trip against `ByteCursor` passes
    // whatever byte order both halves agree on, so the ORDER is pinned against literal
    // bytes here and nowhere else. The `u32` writer is private -- a bare length or count
    // append is the defect this type exists to make unwritable -- so its order is
    // observed through the two public callers, a field's length prefix and a count.
    std::vector<std::byte> blob;
    ByteAppender appender { blob };
    appender.AppendByte(std::byte { 0x7F });
    appender.AppendU64(0x0102030405060708ULL);
    appender.AppendCount(3);
    appender.AppendField(std::string_view { "abc" });

    CHECK(blob == Bytes({ 0x7F, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 3, 0, 0, 0, 3, 'a', 'b', 'c' }));
}

TEST_CASE("ByteAppender and ByteCursor are the same grammar", "[core][byteappender]")
{
    // Necessary and, on its own, not sufficient -- see the byte-order case above.
    auto const second = Bytes({ 0xDE, 0xAD });
    std::vector<std::byte> blob;
    ByteAppender appender { blob };
    appender.AppendByte(std::byte { 0x11 });
    appender.AppendU64(~std::uint64_t { 0 });
    appender.AppendCount(2);
    appender.AppendField(std::string_view { "first" });
    appender.AppendField(std::span<std::byte const> { second });

    ByteCursor cursor { blob };
    std::uint8_t tag = 0;
    std::uint64_t wide = 0;
    std::uint32_t count = 0;
    std::string text;
    std::vector<std::byte> raw;
    REQUIRE(cursor.ReadU8(tag));
    REQUIRE(cursor.ReadU64(wide));
    REQUIRE(cursor.ReadCount(count, WireFields::FieldPrefixSize));
    REQUIRE(cursor.ReadField(text));
    REQUIRE(cursor.ReadFieldBytes(raw));

    CHECK(tag == 0x11);
    CHECK(wide == ~std::uint64_t { 0 });
    CHECK(count == 2U);
    CHECK(text == "first");
    CHECK(raw == second);
    CHECK(cursor.AtEnd());
}

TEST_CASE("ByteAppender writes the same bytes into either buffer spelling", "[core][byteappender]")
{
    // The reason it is a template at all: a `std::vector<std::byte>` value blob and a
    // `std::string` manifest are both encoded with it, and a divergence between the two
    // instantiations would be a format that depends on which container an encoder
    // happened to pick. Nothing else in the suite compares them.
    auto const fill = [](auto& out) {
        ByteAppender appender { out };
        appender.AppendByte(std::byte { 0x02 });
        appender.AppendU64(0xFEDCBA9876543210ULL);
        appender.AppendCount(1);
        appender.AppendField(std::string_view { "payload" });
        appender.AppendRaw(std::string_view { "tail" });
    };

    std::vector<std::byte> asBlob;
    std::string asText;
    fill(asBlob);
    fill(asText);

    CHECK(asBlob == Bytes(asText));
}

TEST_CASE("An empty field is a length prefix and nothing else", "[core][byteappender]")
{
    // A zero-length field is ordinary -- an empty consumer name, an empty region -- and
    // an empty span may legally carry a null `data()`, so the append must not reach for
    // it. Read back as well as pinned, because "wrote nothing at all" also produces an
    // empty payload.
    std::vector<std::byte> blob;
    ByteAppender appender { blob };
    appender.AppendRaw(std::span<std::byte const> {});
    appender.AppendField(std::string_view {});
    appender.AppendField(std::span<std::byte const> {});

    CHECK(blob == Bytes({ 0, 0, 0, 0, 0, 0, 0, 0 }));

    ByteCursor cursor { blob };
    std::string first;
    std::vector<std::byte> second;
    REQUIRE(cursor.ReadField(first));
    REQUIRE(cursor.ReadFieldBytes(second));
    CHECK(first.empty());
    CHECK(second.empty());
    CHECK(cursor.AtEnd());
}

TEST_CASE("A length that cannot be declared is refused rather than truncated", "[core][byteappender]")
{
    // The behaviour every hand-rolled site this replaced did NOT have: each cast its
    // size down to `u32`, which emits a field whose declared length disagrees with its
    // contents. Unreachable at the sizes this tree encodes, which is the argument for
    // closing it by construction rather than for leaving the cast for the next caller.
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t))
    {
        auto const tooLong = static_cast<std::size_t>(WireFields::MaxPayload) + 1;

        CHECK_THROWS_AS(WireFields::RequireFieldLength(tooLong), std::length_error);
        CHECK(WireFields::RequireFieldLength(static_cast<std::size_t>(WireFields::MaxPayload))
              == static_cast<std::uint32_t>(WireFields::MaxPayload));

        std::vector<std::byte> blob;
        ByteAppender appender { blob };
        CHECK_THROWS_AS(appender.AppendCount(tooLong), std::length_error);
        CHECK(blob.empty()); // refused before a byte was written, not half-written
    }
}

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ByteCursor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

} // namespace

TEST_CASE("ByteCursor reads the fixed-width fields big-endian", "[core][bytecursor]")
{
    auto const buffer = Bytes({ 0x7F, 0x00, 0x00, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 3 });
    ByteCursor cursor { buffer };

    std::uint8_t byte = 0;
    std::uint32_t word = 0;
    std::uint64_t wide = 0;
    REQUIRE(cursor.ReadU8(byte));
    REQUIRE(cursor.ReadU32(word));
    REQUIRE(cursor.ReadU64(wide));

    CHECK(byte == 0x7F);
    CHECK(word == 0x00000102U);
    CHECK(wide == 3U);
    CHECK(cursor.AtEnd());
    CHECK(cursor.Ok());
}

TEST_CASE("ByteCursor fails stickily and consumes nothing after a failure", "[core][bytecursor]")
{
    // A decoder may check every read or check `Ok()` once at the end; the second is
    // only safe if a later read cannot appear to succeed after an earlier failure.
    auto const buffer = Bytes({ 1, 2, 3 });
    ByteCursor cursor { buffer };

    std::uint32_t word = 0;
    CHECK_FALSE(cursor.ReadU32(word)); // three bytes cannot supply four
    CHECK_FALSE(cursor.Ok());

    // The byte that IS there is not handed out afterwards, and `Remaining()` reports
    // nothing rather than tempting a caller into a further read.
    std::uint8_t byte = 0;
    CHECK_FALSE(cursor.ReadU8(byte));
    CHECK(cursor.Remaining() == 0);
    CHECK_FALSE(cursor.AtEnd()); // failed is not finished
}

TEST_CASE("ByteCursor::ReadCount refuses a count the remaining bytes cannot supply", "[core][bytecursor][security]")
{
    // The reason the type exists (issues #241, #267, #269, #271). The count is read
    // and then checked against what is left, so a decoder cannot obtain one without
    // having stated what an element costs.

    SECTION("an impossible count is refused, and the cursor is left failed")
    {
        // A count of 0xFFFFFFFF with eight bytes behind it: at five bytes an element
        // the frame could carry one.
        auto const buffer = Bytes({ 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0 });
        ByteCursor cursor { buffer };

        std::uint32_t count = 0;
        CHECK_FALSE(cursor.ReadCount(count, 5));
        CHECK_FALSE(cursor.Ok());
    }

    SECTION("the boundary, from both sides")
    {
        // Ten bytes after the count, five bytes an element: two fit, three do not.
        auto const buffer = Bytes({ 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        ByteCursor two { buffer };
        std::uint32_t count = 0;
        REQUIRE(two.ReadCount(count, 5));
        CHECK(count == 2);

        auto const threeBuffer = Bytes({ 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        ByteCursor three { threeBuffer };
        CHECK_FALSE(three.ReadCount(count, 5));
    }

    SECTION("zero elements need no bytes")
    {
        auto const buffer = Bytes({ 0, 0, 0, 0 });
        ByteCursor cursor { buffer };
        std::uint32_t count = 1;
        REQUIRE(cursor.ReadCount(count, 5));
        CHECK(count == 0);
        CHECK(cursor.AtEnd());
    }

    SECTION("a truncated count field is refused before any bound is consulted")
    {
        auto const buffer = Bytes({ 0, 0 });
        ByteCursor cursor { buffer };
        std::uint32_t count = 0;
        CHECK_FALSE(cursor.ReadCount(count, 1));
    }
}

TEST_CASE("ByteCursor::ReadField reads a length-prefixed field and bounds its length", "[core][bytecursor]")
{
    SECTION("a well-formed field")
    {
        auto const buffer = Bytes({ 0, 0, 0, 2, 'h', 'i' });
        ByteCursor cursor { buffer };
        std::string text;
        REQUIRE(cursor.ReadField(text));
        CHECK(text == "hi");
        CHECK(cursor.AtEnd());
    }

    SECTION("a length that overruns the buffer")
    {
        // 0xFFFFFFFF bytes claimed, two present. The check is a subtraction from the
        // size, so it cannot be defeated by the sum wrapping on a 32-bit `size_t`.
        auto const buffer = Bytes({ 0xFF, 0xFF, 0xFF, 0xFF, 'h', 'i' });
        ByteCursor cursor { buffer };
        std::string text;
        CHECK_FALSE(cursor.ReadField(text));
        CHECK_FALSE(cursor.Ok());
    }
}

TEST_CASE("ByteCursor starts where it is told, and past the end is simply exhausted", "[core][bytecursor]")
{
    // Decoders that have already matched a fixed header hand the cursor their offset.
    auto const buffer = Bytes({ 0xFC, 0x01, 0, 0, 0, 7 });
    ByteCursor cursor { buffer, 2 };
    std::uint32_t word = 0;
    REQUIRE(cursor.ReadU32(word));
    CHECK(word == 7U);

    // An offset past the end is not undefined behaviour, it is an empty cursor.
    ByteCursor beyond { buffer, 999 };
    CHECK(beyond.Remaining() == 0);
    CHECK(beyond.AtEnd());
}

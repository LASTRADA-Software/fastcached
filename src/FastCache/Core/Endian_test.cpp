// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Endian.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace FastCache;

TEST_CASE("Endian readers map a fixed byte sequence to a fixed value", "[endian]")
{
    // The property that matters is not "matches this host" -- it is that a given
    // byte sequence decodes to the same number everywhere. Both readers feed
    // values that end up hashed into a compile-cache key (the little-endian one
    // through MurmurHash3's block loads, the big-endian one through KeyDigest's
    // length prefixes), and a key that decoded differently per host would not
    // fail: it would silently split a shared cache in two, with every machine
    // missing on every entry the others wrote.
    //
    // Spelling both expectations as literals is what makes that testable on a
    // little-endian host at all. Every machine CI runs on is little-endian, so
    // the byteswap branch is never executed -- but these assertions still pin the
    // contract that branch exists to uphold, and they would fail on a big-endian
    // port that got it wrong rather than passing vacuously.
    static constexpr std::array<std::byte, 8> bytes {
        std::byte { 0x01 }, std::byte { 0x23 }, std::byte { 0x45 }, std::byte { 0x67 },
        std::byte { 0x89 }, std::byte { 0xAB }, std::byte { 0xCD }, std::byte { 0xEF },
    };

    CHECK(ReadLittleEndian<std::uint64_t>(bytes) == 0xEFCD'AB89'6745'2301ULL);
    CHECK(ReadBigEndian<std::uint64_t>(bytes) == 0x0123'4567'89AB'CDEFULL);

    CHECK(ReadLittleEndian<std::uint32_t>(bytes) == 0x6745'2301U);
    CHECK(ReadBigEndian<std::uint32_t>(bytes) == 0x0123'4567U);

    CHECK(ReadLittleEndian<std::uint16_t>(bytes) == 0x2301U);
    CHECK(ReadBigEndian<std::uint16_t>(bytes) == 0x0123U);

    // A single byte has no order, so the two must agree.
    CHECK(ReadLittleEndian<std::uint8_t>(bytes) == 0x01U);
    CHECK(ReadBigEndian<std::uint8_t>(bytes) == 0x01U);
}

TEST_CASE("Endian conversions are their own inverse", "[endian]")
{
    for (auto const value:
         { std::uint64_t { 0 }, std::uint64_t { 1 }, std::uint64_t { 0x0123'4567'89AB'CDEFULL }, ~std::uint64_t { 0 } })
    {
        // LittleEndianToHost is its own inverse, which is why there is no
        // HostToLittleEndian to pair with it: it would be the same function under
        // a second name, and nothing calls it.
        CHECK(LittleEndianToHost(LittleEndianToHost(value)) == value);
        CHECK(BigEndianToHost(HostToBigEndian(value)) == value);
    }
}

TEST_CASE("A big-endian write round-trips through the matching read", "[endian]")
{
    // KeyDigest writes its length prefixes this way, so the pair has to agree.
    std::array<std::byte, 8> buffer {};
    WriteBigEndian<std::uint64_t>(buffer, 0xDEAD'BEEF'CAFE'F00DULL);
    CHECK(ReadBigEndian<std::uint64_t>(buffer) == 0xDEAD'BEEF'CAFE'F00DULL);

    // ... and the bytes it produced are the documented order, not the host's.
    CHECK(buffer[0] == std::byte { 0xDE });
    CHECK(buffer[7] == std::byte { 0x0D });
}

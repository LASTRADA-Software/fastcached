// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Crc32c.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

TEST_CASE("CRC32C of empty input is zero", "[crc32c]")
{
    REQUIRE(FastCache::Crc32c::Compute({}) == 0U);
}

TEST_CASE("CRC32C standard test vectors", "[crc32c]")
{
    // RFC 3720 / iSCSI test vectors for CRC-32C.
    // "123456789" -> 0xE3069283
    auto const sv = std::string_view { "123456789" };
    REQUIRE(FastCache::Crc32c::Compute(FastCache::AsBytes(sv)) == 0xE3069283U);
}

TEST_CASE("CRC32C streaming equals one-shot", "[crc32c]")
{
    auto const sv = std::string_view { "the quick brown fox jumps over the lazy dog" };
    auto const oneShot = FastCache::Crc32c::Compute(FastCache::AsBytes(sv));

    auto state = FastCache::Crc32c::kSeed;
    auto const first = FastCache::AsBytes(sv.substr(0, 16));
    auto const second = FastCache::AsBytes(sv.substr(16));
    FastCache::Crc32c::Update(state, first);
    FastCache::Crc32c::Update(state, second);
    REQUIRE(FastCache::Crc32c::Finalise(state) == oneShot);
}

TEST_CASE("CRC32C detects single-bit changes", "[crc32c]")
{
    std::string_view const a = "fastcached";
    std::string_view const b = "Fastcached";
    REQUIRE(FastCache::Crc32c::Compute(FastCache::AsBytes(a))
            != FastCache::Crc32c::Compute(FastCache::AsBytes(b)));
}

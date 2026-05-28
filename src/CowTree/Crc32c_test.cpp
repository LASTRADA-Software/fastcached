// SPDX-License-Identifier: Apache-2.0
#include <CowTree/Bytes.hpp>
#include <CowTree/Crc32c.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{

CowTree::BytesView AsBytes(std::string_view sv) noexcept
{
    return CowTree::AsBytes(sv);
}

} // namespace

TEST_CASE("Crc32c known vector for empty input", "[crc32c]")
{
    auto const crc = CowTree::Crc32c::Compute(AsBytes(""));
    REQUIRE(crc == 0u);
}

TEST_CASE("Crc32c known vector for ASCII 'a'", "[crc32c]")
{
    auto const crc = CowTree::Crc32c::Compute(AsBytes("a"));
    REQUIRE(crc == 0xC1D04330u);
}

TEST_CASE("Crc32c known vector for '123456789'", "[crc32c]")
{
    auto const crc = CowTree::Crc32c::Compute(AsBytes("123456789"));
    REQUIRE(crc == 0xE3069283u);
}

TEST_CASE("Crc32c streaming matches one-shot", "[crc32c]")
{
    std::string_view const text = "fastcached.cowtree";

    auto const oneShot = CowTree::Crc32c::Compute(AsBytes(text));

    auto state = CowTree::Crc32c::Seed;
    CowTree::Crc32c::Update(state, AsBytes(text.substr(0, 5)));
    CowTree::Crc32c::Update(state, AsBytes(text.substr(5)));
    auto const streamed = CowTree::Crc32c::Finalise(state);

    REQUIRE(oneShot == streamed);
}

TEST_CASE("Crc32c detects single-byte flips", "[crc32c]")
{
    std::array<std::byte, 16> buf {};
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<std::byte>(i);
    auto const base = CowTree::Crc32c::Compute({ buf.data(), buf.size() });

    for (std::size_t i = 0; i < buf.size(); ++i)
    {
        auto copy = buf;
        copy[i] = std::byte { static_cast<std::uint8_t>(static_cast<std::uint8_t>(copy[i]) ^ 0xFFu) };
        auto const flipped = CowTree::Crc32c::Compute({ copy.data(), copy.size() });
        REQUIRE(flipped != base);
    }
}

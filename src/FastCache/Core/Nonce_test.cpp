// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Nonce.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using namespace FastCache;

TEST_CASE("A nonce is every draw, big-endian, in order", "[core][nonce]")
{
    // Four draws whose every byte differs, so a draw written little-endian, one
    // written twice, or a slot left zero all show as a byte out of place.
    ScriptedRandomSource random { {
        0x0001020304050607ULL,
        0x08090A0B0C0D0E0FULL,
        0x1011121314151617ULL,
        0x18191A1B1C1D1E1FULL,
    } };

    auto const nonce = DrawNonce(random);

    CHECK(random.DrawCount() == NonceBytes / sizeof(std::uint64_t));
    for (std::size_t index = 0; auto const byte: nonce)
        CHECK(byte == static_cast<std::byte>(index++));
}

TEST_CASE("Two nonces from one source are two nonces", "[core][nonce]")
{
    // The property a nonce is FOR is that it does not repeat. A draw that ignored the
    // source -- a zeroed array returned as the nonce -- passes the byte-order case above
    // only if the script happened to be zero, so this asks the question directly.
    SystemRandomSource random { 0x5EED };
    CHECK(DrawNonce(random) != DrawNonce(random));
}

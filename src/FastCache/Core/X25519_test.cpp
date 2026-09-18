// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/X25519.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <vector>

#include <tests/HexBytes.hpp>

using namespace FastCache;
using FastCache::Testing::ArrayFromHex;
using FastCache::Testing::FromHex;

namespace
{

/// One of RFC 7748 §5.2's X25519 input/output vectors.
struct Rfc7748Vector
{
    std::string_view scalar;     ///< Input scalar.
    std::string_view coordinate; ///< Input u-coordinate.
    std::string_view output;     ///< Output u-coordinate.
};

/// RFC 7748 §5.2, the two X25519 vectors.
constexpr std::array Rfc7748Vectors {
    Rfc7748Vector {
        .scalar = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
        .coordinate = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
        .output = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
    },
    Rfc7748Vector {
        .scalar = "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
        .coordinate = "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
        .output = "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
    },
};

/// RFC 7748 §6.1's Diffie-Hellman test vector.
constexpr std::string_view AlicePrivate = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
constexpr std::string_view AlicePublic = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
constexpr std::string_view BobPrivate = "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
constexpr std::string_view BobPublic = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
constexpr std::string_view SharedK = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

/// @p secret as a byte vector, for comparing against a vector's hex.
/// @param secret A shared secret.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> BytesOf(SecureByteBuffer const& secret)
{
    return { secret.begin(), secret.end() };
}

} // namespace

TEST_CASE("X25519 matches the RFC 7748 section 5.2 vectors", "[core][crypto][x25519]")
{
    for (auto const& vector: Rfc7748Vectors)
    {
        CAPTURE(vector.scalar);
        auto const shared = X25519SharedSecret(FromHex(vector.scalar), ArrayFromHex<X25519KeyBytes>(vector.coordinate));
        REQUIRE(shared.has_value());
        CHECK(BytesOf(shared.value()) == FromHex(vector.output));
    }
}

TEST_CASE("X25519 matches the RFC 7748 section 5.2 iterated results", "[core][crypto][x25519]")
{
    // k and u start as the base point 9; each round k becomes X25519(k, u) and u the old k. The
    // RFC's million-iteration figure is left out: it costs minutes, and a thousand rounds already
    // feed every output back in as both a scalar and a point.
    auto k = ArrayFromHex<X25519KeyBytes>("0900000000000000000000000000000000000000000000000000000000000000");
    auto u = k;
    auto const round = [&k, &u] {
        auto const next = X25519SharedSecret(k, u);
        REQUIRE(next.has_value());
        u = k;
        std::ranges::copy(next.value(), k.begin());
    };

    round();
    CHECK(k == ArrayFromHex<X25519KeyBytes>("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"));
    for ([[maybe_unused]] auto const iteration: std::views::iota(1, 1000))
        round();
    CHECK(k == ArrayFromHex<X25519KeyBytes>("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"));
}

TEST_CASE("X25519 matches the RFC 7748 section 6.1 Diffie-Hellman exchange", "[core][crypto][x25519]")
{
    auto const alicePrivate = FromHex(AlicePrivate);
    auto const bobPrivate = FromHex(BobPrivate);

    auto const alicePublic = X25519PublicKeyFrom(alicePrivate);
    auto const bobPublic = X25519PublicKeyFrom(bobPrivate);
    REQUIRE(alicePublic.has_value());
    REQUIRE(bobPublic.has_value());
    CHECK(alicePublic.value() == ArrayFromHex<X25519KeyBytes>(AlicePublic));
    CHECK(bobPublic.value() == ArrayFromHex<X25519KeyBytes>(BobPublic));

    auto const aliceShared = X25519SharedSecret(alicePrivate, bobPublic.value());
    auto const bobShared = X25519SharedSecret(bobPrivate, alicePublic.value());
    REQUIRE(aliceShared.has_value());
    REQUIRE(bobShared.has_value());
    CHECK(BytesOf(aliceShared.value()) == FromHex(SharedK));
    CHECK(BytesOf(bobShared.value()) == FromHex(SharedK));
}

TEST_CASE("X25519SharedSecret refuses the all-zero result a low-order peer key produces", "[core][crypto][x25519][negative]")
{
    // The seven u-coordinates libsodium refuses as small-order: a clamped scalar is a multiple of
    // the cofactor, so times any of these it reaches the identity, whose u-coordinate is zero --
    // and this case is what shows that each one does. `p` and `p + 1` are 0 and 1 spelled
    // non-canonically, which X25519 reduces modulo p.
    constexpr std::array lowOrderPoints {
        std::string_view { "0000000000000000000000000000000000000000000000000000000000000000" }, // 0
        std::string_view { "0100000000000000000000000000000000000000000000000000000000000000" }, // 1
        std::string_view { "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800" }, // an order-8 point
        std::string_view { "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157" }, // the other order-8 point
        std::string_view { "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" }, // p - 1
        std::string_view { "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" }, // p
        std::string_view { "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" }, // p + 1
    };
    auto const secretKey = FromHex(AlicePrivate);
    for (auto const point: lowOrderPoints)
    {
        CAPTURE(point);
        auto const shared = X25519SharedSecret(secretKey, ArrayFromHex<X25519KeyBytes>(point));
        REQUIRE_FALSE(shared.has_value());
        CHECK(shared.error() == CryptoError::LowOrderPeerKey);
    }
}

TEST_CASE("X25519 refuses a secret key that is not 32 bytes", "[core][crypto][x25519]")
{
    auto const peer = ArrayFromHex<X25519KeyBytes>(BobPublic);
    for (auto const length: { std::size_t { 0 }, std::size_t { 31 }, std::size_t { 33 } })
    {
        CAPTURE(length);
        SecureByteBuffer const secretKey(length, std::byte { 0x42 });

        auto const publicKey = X25519PublicKeyFrom(secretKey);
        REQUIRE_FALSE(publicKey.has_value());
        CHECK(publicKey.error() == CryptoError::WrongKeyLength);

        auto const shared = X25519SharedSecret(secretKey, peer);
        REQUIRE_FALSE(shared.has_value());
        CHECK(shared.error() == CryptoError::WrongKeyLength);
    }
}

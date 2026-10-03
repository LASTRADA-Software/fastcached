// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/MonocypherBytes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

// One of the two first-party files allowed to include a Monocypher header (`ctest -R crypto-seam`).
#include <monocypher-ed25519.h>

namespace FastCache
{

namespace
{
    /// Monocypher's secret-key layout: the 32-byte seed, then the 32-byte public key.
    constexpr std::size_t MonocypherSecretKeyBytes = Ed25519SeedBytes + Ed25519PublicKeyBytes;

    /// An encoded point, 32 little-endian bytes: y, with the sign of x in bit 255.
    using PointEncoding = std::array<std::uint8_t, Ed25519PublicKeyBytes>;

    /// Bit 255 of an encoding, the sign of x, which is the top bit of its last byte.
    constexpr std::uint8_t SignBit = 0x80;

    /// The field prime p = 2^255 - 19, little-endian: the bound a canonical y stays below.
    constexpr PointEncoding FieldPrime { 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };

    /// Every encoding of a point of small order, sign bit CLEARED -- libsodium's blocklist, verbatim
    /// (`crypto_core/ed25519/ref10/ed25519_ref10.c`, `ge25519_has_small_order`).
    ///
    /// The eight points of the torsion subgroup have five y coordinates -- 0 (the two of order 4),
    /// 1 (the identity), p - 1 (order 2) and the two of order 8 -- and the two of those below 19
    /// have a second, non-canonical spelling, y + p, which Monocypher also decodes. Seven rows. That
    /// set was re-derived independently of this list (generating the subgroup from the order-8
    /// point in exact arithmetic, and decoding every y + p below 2^255), and the two agreed.
    constexpr std::array SmallOrderEncodings {
        // 0 (order 4)
        PointEncoding { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        // 1 (order 1)
        PointEncoding { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        // 2707385501144840649318225287225658788936804267575313519463743609750303402022 (order 8)
        PointEncoding { 0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
                        0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05 },
        // 55188659117513257062467267217118295137698188065244968500265048394206261417927 (order 8)
        PointEncoding { 0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
                        0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a },
        // p - 1 (order 2)
        PointEncoding { 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
        // p (= 0, order 4), non-canonical
        PointEncoding { 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
        // p + 1 (= 1, order 1), non-canonical
        PointEncoding { 0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
    };

    /// @p encoding as bytes, with the sign of x cleared: the y coordinate alone.
    /// @param encoding An encoded point.
    /// @return Its y coordinate, little-endian.
    [[nodiscard]] PointEncoding YCoordinateOf(std::span<std::byte const, Ed25519PublicKeyBytes> encoding) noexcept
    {
        auto y = PointEncoding {};
        std::ranges::transform(encoding, y.begin(), [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
        y.back() &= static_cast<std::uint8_t>(~SignBit);
        return y;
    }

    /// Why @p encoding is not a point a signature may rest on, or nothing.
    ///
    /// The one answer `Ed25519PublicKeyFaultOf` gives for a key and `Ed25519Verify` asks of A and of
    /// R. Variable-time, which is right: every input is public -- a key, or a signature's R.
    /// @param encoding An encoded point.
    /// @return `SmallOrder`, `NonCanonical`, or nullopt.
    [[nodiscard]] std::optional<PublicKeyFault> PointFaultOf(
        std::span<std::byte const, Ed25519PublicKeyBytes> encoding) noexcept
    {
        auto const y = YCoordinateOf(encoding);
        if (std::ranges::contains(SmallOrderEncodings, y))
            return PublicKeyFault::SmallOrder;
        // Little-endian, so the comparison runs from the most significant byte down.
        if (!std::ranges::lexicographical_compare(y | std::views::reverse, FieldPrime | std::views::reverse))
            return PublicKeyFault::NonCanonical;
        return std::nullopt;
    }
} // namespace

std::expected<Ed25519KeyPair, CryptoError> Ed25519KeyPair::FromSeed(std::span<std::byte const> seed)
{
    if (seed.size() != Ed25519SeedBytes)
        return std::unexpected(CryptoError::WrongKeyLength);

    // `crypto_ed25519_key_pair` WIPES the seed it is handed (monocypher-ed25519.c: "crypto_wipe(seed,
    // 32)"), and the caller's seed is only lent. So it gets a copy, in storage that is zeroed again
    // when it is released -- wiped twice over, which costs nothing.
    SecureByteBuffer seedCopy(seed.begin(), seed.end());
    SecureByteBuffer secretKey(MonocypherSecretKeyBytes);
    Ed25519PublicKey publicKey {};
    monocypher::crypto_ed25519_key_pair(
        Detail::MonocypherOut(secretKey), Detail::MonocypherOut(publicKey), Detail::MonocypherOut(seedCopy));
    return Ed25519KeyPair { std::move(secretKey), publicKey };
}

Ed25519KeyPair::Ed25519KeyPair(SecureByteBuffer secretKey, Ed25519PublicKey const& publicKey) noexcept:
    _secretKey { std::move(secretKey) },
    _publicKey { publicKey }
{
}

Ed25519Signature Ed25519KeyPair::Sign(std::span<std::byte const> message) const noexcept
{
    // Only a moved-from pair can fail this: `FromSeed` is the one constructor, and it sizes the key.
    assert(_secretKey.size() == MonocypherSecretKeyBytes);
    Ed25519Signature signature {};
    monocypher::crypto_ed25519_sign(
        Detail::MonocypherOut(signature), Detail::MonocypherIn(_secretKey), Detail::MonocypherIn(message), message.size());
    return signature;
}

std::optional<PublicKeyFault> Ed25519PublicKeyFaultOf(Ed25519PublicKey const& key) noexcept
{
    return PointFaultOf(key);
}

bool Ed25519Verify(Ed25519PublicKey const& publicKey,
                   std::span<std::byte const> message,
                   Ed25519Signature const& signature) noexcept
{
    // Before Monocypher, which answers the cofactored equation and so accepts a small-order A with a
    // small-order R for every message (see the header). R is the signature's first 32 bytes.
    if (PointFaultOf(publicKey).has_value()
        || PointFaultOf(std::span { signature }.first<Ed25519PublicKeyBytes>()).has_value())
        return false;

    // Monocypher answers 0 for a valid signature and -1 for anything else.
    return monocypher::crypto_ed25519_check(Detail::MonocypherIn(signature),
                                            Detail::MonocypherIn(publicKey),
                                            Detail::MonocypherIn(message),
                                            message.size())
           == 0;
}

std::string FormatEd25519PublicKey(Ed25519PublicKey const& key)
{
    return Base64UrlEncode(key);
}

std::expected<Ed25519PublicKey, PublicKeyFault> ParseEd25519PublicKey(std::string_view text)
{
    // The length first, because it is the fault an operator is likeliest to have made -- a key
    // cut short by a terminal or a paste -- and the sentence for it says what whole looks like.
    if (text.size() != Ed25519PublicKeyTextLength)
        return std::unexpected(PublicKeyFault::WrongLength);

    auto const decoded = Base64UrlDecode(text);
    if (!decoded.has_value())
        return std::unexpected(PublicKeyFault::NotBase64Url);

    // 43 canonical symbols are 32 bytes exactly, so this cannot be short; asserted rather than
    // branched on, because a branch here would be an arm nothing can reach.
    assert(decoded->size() == Ed25519PublicKeyBytes);
    Ed25519PublicKey key {};
    std::ranges::transform(*decoded, key.begin(), [](char c) { return static_cast<std::byte>(c); });
    if (auto const fault = Ed25519PublicKeyFaultOf(key); fault.has_value())
        return std::unexpected(*fault);
    return key;
}

} // namespace FastCache

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/MonocypherBytes.hpp>

#include <algorithm>
#include <cassert>
#include <utility>

// One of the two first-party files allowed to include a Monocypher header (`ctest -R crypto-seam`).
#include <monocypher-ed25519.h>

namespace FastCache
{

namespace
{
    /// Monocypher's secret-key layout: the 32-byte seed, then the 32-byte public key.
    constexpr std::size_t MonocypherSecretKeyBytes = Ed25519SeedBytes + Ed25519PublicKeyBytes;
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

bool Ed25519Verify(Ed25519PublicKey const& publicKey,
                   std::span<std::byte const> message,
                   Ed25519Signature const& signature) noexcept
{
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

std::expected<Ed25519PublicKey, PublicKeyTextFault> ParseEd25519PublicKey(std::string_view text)
{
    // The length first, because it is the fault an operator is likeliest to have made -- a key
    // cut short by a terminal or a paste -- and the sentence for it says what whole looks like.
    if (text.size() != Ed25519PublicKeyTextLength)
        return std::unexpected(PublicKeyTextFault::WrongLength);

    auto const decoded = Base64UrlDecode(text);
    if (!decoded.has_value())
        return std::unexpected(PublicKeyTextFault::NotBase64Url);

    // 43 canonical symbols are 32 bytes exactly, so this cannot be short; asserted rather than
    // branched on, because a branch here would be an arm nothing can reach.
    assert(decoded->size() == Ed25519PublicKeyBytes);
    Ed25519PublicKey key {};
    std::ranges::transform(*decoded, key.begin(), [](char c) { return static_cast<std::byte>(c); });
    return key;
}

} // namespace FastCache

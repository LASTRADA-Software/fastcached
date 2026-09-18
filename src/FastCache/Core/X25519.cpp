// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/MonocypherBytes.hpp>
#include <FastCache/Core/X25519.hpp>

#include <cstdint>

// One of the two first-party files allowed to include a Monocypher header (`ctest -R crypto-seam`).
#include <monocypher.h>

namespace FastCache
{

std::expected<X25519PublicKey, CryptoError> X25519PublicKeyFrom(std::span<std::byte const> secretKey)
{
    if (secretKey.size() != X25519KeyBytes)
        return std::unexpected(CryptoError::WrongKeyLength);
    X25519PublicKey publicKey {};
    monocypher::crypto_x25519_public_key(Detail::MonocypherOut(publicKey), Detail::MonocypherIn(secretKey));
    return publicKey;
}

std::expected<SecureByteBuffer, CryptoError> X25519SharedSecret(std::span<std::byte const> secretKey,
                                                                X25519PublicKey const& peerPublicKey)
{
    if (secretKey.size() != X25519KeyBytes)
        return std::unexpected(CryptoError::WrongKeyLength);

    SecureByteBuffer sharedSecret(X25519KeyBytes);
    monocypher::crypto_x25519(
        Detail::MonocypherOut(sharedSecret), Detail::MonocypherIn(secretKey), Detail::MonocypherIn(peerPublicKey));

    // Every byte is folded in whatever the outcome. Which way this goes is no secret -- only a
    // peer's chosen low-order point makes it zero -- but the fold costs nothing over an early exit.
    std::uint8_t anyBitSet = 0;
    for (auto const byte: sharedSecret)
        anyBitSet |= std::to_integer<std::uint8_t>(byte);
    if (anyBitSet == 0)
        return std::unexpected(CryptoError::LowOrderPeerKey);
    return sharedSecret;
}

} // namespace FastCache

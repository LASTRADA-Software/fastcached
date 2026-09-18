// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Hkdf.hpp>

#include <algorithm>
#include <ranges>

namespace FastCache
{

SecureByteBuffer HkdfSha256Extract(std::span<std::byte const> salt, std::span<std::byte const> inputKeyMaterial)
{
    auto digest = HmacSha256(salt, inputKeyMaterial);
    SecureByteBuffer pseudoRandomKey(digest.begin(), digest.end());
    SecureZero(digest.data(), digest.size());
    return pseudoRandomKey;
}

std::expected<SecureByteBuffer, CryptoError> HkdfSha256Expand(std::span<std::byte const> pseudoRandomKey,
                                                              std::span<std::byte const> info,
                                                              std::size_t outputBytes)
{
    if (pseudoRandomKey.size() < HkdfSha256HashBytes)
        return std::unexpected(CryptoError::PseudoRandomKeyTooShort);
    if (outputBytes == 0)
        return std::unexpected(CryptoError::EmptyOutput);
    if (outputBytes > HkdfSha256MaxOutputBytes)
        return std::unexpected(CryptoError::OutputTooLong);

    // T(0) is empty; T(i) = HMAC(PRK, T(i-1) | info | i), for the one-byte block number i from 1.
    // The bound above is what keeps that number within a byte.
    auto const blocks = (outputBytes + HkdfSha256HashBytes - 1) / HkdfSha256HashBytes;
    SecureByteBuffer outputKeyMaterial;
    outputKeyMaterial.reserve(outputBytes);
    SecureByteBuffer blockInput;
    blockInput.reserve(HkdfSha256HashBytes + info.size() + 1);
    Sha256::Digest block {};
    for (auto const blockNumber: std::views::iota(std::size_t { 1 }, blocks + 1))
    {
        blockInput.clear();
        if (blockNumber > 1)
            blockInput.insert(blockInput.end(), block.begin(), block.end());
        blockInput.insert(blockInput.end(), info.begin(), info.end());
        blockInput.push_back(static_cast<std::byte>(blockNumber));
        block = HmacSha256(pseudoRandomKey, blockInput);
        auto const take = std::min(HkdfSha256HashBytes, outputBytes - outputKeyMaterial.size());
        outputKeyMaterial.insert(outputKeyMaterial.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(take));
    }
    SecureZero(block.data(), block.size());
    return outputKeyMaterial;
}

std::expected<SecureByteBuffer, CryptoError> HkdfSha256(std::span<std::byte const> salt,
                                                        std::span<std::byte const> inputKeyMaterial,
                                                        std::span<std::byte const> info,
                                                        std::size_t outputBytes)
{
    return HkdfSha256Expand(HkdfSha256Extract(salt, inputKeyMaterial), info, outputBytes);
}

} // namespace FastCache

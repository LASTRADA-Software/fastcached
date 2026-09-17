// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../apps/fastcache-compile-node/ClusterKeySource.hpp"

#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/SecureBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Testing
{

/// @file NodeProofFakes.hpp
/// The two collaborators every cluster-key-proof case scripts.
///
/// Shared rather than written per file, because a fake is a shared helper too: the second copy
/// of a key source is the one that stops answering what the first one answers, and every case
/// here asserts a REFUSAL -- which a differently-scripted key produces just as convincingly.
/// Two test files drive these (`NodeProofResponder_test.cpp` and `FrameEndpoint_test.cpp`), one
/// of them through a real socket.

/// A cluster key that answers fixed bytes, or a fixed failure.
class ScriptedClusterKey final: public Node::IClusterKeySource
{
  public:
    /// @param key The bytes to answer with. **Empty means answer the failure below instead**,
    ///        which is how a key file that has stopped being readable is scripted -- the one
    ///        condition that is neither a caller's fault nor a wrong key.
    /// @param failure Why the key could not be read, for the empty case.
    explicit ScriptedClusterKey(std::string_view key, std::string failure = "open: no such file or directory"):
        _key { key },
        _failure { std::move(failure) }
    {
    }

    /// @copydoc Node::IClusterKeySource::ClusterKey
    [[nodiscard]] std::expected<SecureByteBuffer, std::string> ClusterKey() const override
    {
        if (_key.empty())
            return std::unexpected { _failure };
        SecureByteBuffer bytes;
        bytes.reserve(_key.size());
        for (auto const character: _key)
            bytes.push_back(static_cast<std::byte>(character));
        return bytes;
    }

  private:
    std::string _key;
    std::string _failure;
};

/// A randomness source whose draws are fixed, so a challenge is reproducible.
///
/// **What this buys is that a test can MINT the tag the server will expect** without reaching
/// inside the server to read the nonce it drew -- which is the only way to write this case
/// against the production seam rather than against a friend declaration.
///
/// A degenerate range still answers its one legal value, because `IRandomSource`'s contract
/// says equal bounds yield that value and a fake more permissive than the thing it stands for
/// is the shape this repository records.
class FixedRandomSource final: public IRandomSource
{
  public:
    /// @param draw What every non-degenerate range answers.
    explicit FixedRandomSource(std::uint64_t draw = 0x0123456789ABCDEFULL) noexcept:
        _draw { draw }
    {
    }

    /// @copydoc IRandomSource::UniformInRange
    [[nodiscard]] std::uint64_t UniformInRange(std::uint64_t lowInclusive, std::uint64_t highInclusive) override
    {
        return lowInclusive == highInclusive ? lowInclusive : _draw;
    }

  private:
    std::uint64_t _draw;
};

/// The bytes of @p text, for a key or a challenge a case spells out.
/// @param text The text.
/// @return Its bytes.
[[nodiscard]] inline std::vector<std::byte> ProofBytesOf(std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (auto const character: text)
        bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}

} // namespace FastCache::Testing

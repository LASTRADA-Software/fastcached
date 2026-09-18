// SPDX-License-Identifier: Apache-2.0
#include "ClusterKeySource.hpp"

#include <cstdio>
#include <format>
#include <memory>
#include <system_error>

namespace FastCache::Node
{

namespace
{
    /// The shortest key this node will accept.
    ///
    /// An HMAC key shorter than its own block is not wrong, but a key an operator
    /// can type in a hurry is one an attacker can guess -- and what a guessed key
    /// buys is admission to the fleet, which is object injection into everybody's
    /// build. Sixteen bytes is a low bar that any generated key clears without
    /// anybody thinking about it, and the refusal says how to produce one.
    constexpr std::size_t MinimumKeyBytes = 16;

    /// Whether a byte is trailing noise a key file did not mean to carry.
    /// @param byte The candidate.
    /// @return True when it may be trimmed.
    [[nodiscard]] constexpr bool IsTrailingNoise(std::byte byte) noexcept
    {
        auto const raw = static_cast<unsigned char>(byte);
        return raw == '\n' || raw == '\r' || raw == ' ' || raw == '\t';
    }
} // namespace

std::expected<SecureByteBuffer, std::string> ReadClusterKey(std::filesystem::path const& path)
{
    auto error = std::error_code {};
    auto const size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected { std::format("cannot read {}: {}", path.string(), error.message()) };

    // A `unique_ptr` over the handle for the reason every other `fopen` in this tree
    // has one: there are failure paths below, and a bare `fclose` at each is the one
    // that eventually gets forgotten.
    auto file = std::unique_ptr<std::FILE, int (*)(std::FILE*)> { std::fopen(path.string().c_str(), "rb"), &std::fclose };
    if (file == nullptr)
        return std::unexpected { std::format("cannot open {}", path.string()) };

    auto key = SecureByteBuffer(static_cast<std::size_t>(size));
    if (!key.empty() && std::fread(key.data(), 1, key.size(), file.get()) != key.size())
        return std::unexpected { std::format("cannot read {} in full", path.string()) };

    while (!key.empty() && IsTrailingNoise(key.back()))
        key.pop_back();

    if (key.size() < MinimumKeyBytes)
        return std::unexpected { std::format("{} holds {} byte(s) of key and at least {} are required. Generate one "
                                             "with `head -c 32 /dev/urandom | base64 > {}` and give every node in the "
                                             "cluster that same file",
                                             path.string(),
                                             key.size(),
                                             MinimumKeyBytes,
                                             path.string()) };

    return key;
}

std::expected<SecureByteBuffer, std::string> FileClusterKeySource::ClusterKey() const
{
    if (_path.empty())
        return std::unexpected { std::string { "this node holds no --cluster-key-file" } };
    return ReadClusterKey(_path);
}

} // namespace FastCache::Node

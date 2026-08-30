// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterIdentity.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

namespace FastCache::Cluster
{

namespace
{
    /// The two draws an identity is made of.
    ///
    /// Two rather than one because a 64-bit identity is 64 bits: birthday collision
    /// across the number of fleets an organisation runs is negligible either way,
    /// but the cost of the second draw is nothing and the cost of being wrong is a
    /// silent reintroduction of #322.
    constexpr std::size_t DrawCount = 2;

    /// One draw, hex encoded, fixed width.
    /// @param value The draw.
    /// @return Sixteen lowercase hex characters.
    [[nodiscard]] std::string HexOf(std::uint64_t value)
    {
        return std::format("{:016x}", value);
    }
} // namespace

std::string MintClusterId(IRandomSource& random)
{
    auto id = std::string {};
    id.reserve(ClusterIdLength);
    for (auto draw = std::size_t { 0 }; draw < DrawCount; ++draw)
        // The full range, so every bit is drawn. `UniformInRange` is inclusive at
        // both ends, which is what makes `max()` the right upper bound rather than
        // one short of it.
        id += HexOf(random.UniformInRange(0, std::numeric_limits<std::uint64_t>::max()));
    return id;
}

bool IsWellFormedClusterId(std::string_view id) noexcept
{
    if (id.size() != ClusterIdLength)
        return false;
    // Lowercase only, so one identity has exactly one spelling. An uppercase variant
    // would compare unequal to the same value and refuse a legitimate fleet, which
    // is the failure mode that reads as an attack.
    return std::ranges::all_of(id, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::expected<std::string, std::string> LoadOrMintClusterId(std::filesystem::path const& path, IRandomSource& random)
{
    auto error = std::error_code {};
    auto const status = std::filesystem::status(path, error);

    // A path that exists and is not a regular file is refused rather than
    // overwritten: it is somebody else's, and minting over it would destroy
    // whatever it is while looking like first-time setup.
    if (!error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))
        return std::unexpected { std::format("{} exists and is not a regular file", path.string()) };

    if (!error && std::filesystem::exists(status))
    {
        auto in = std::ifstream { path, std::ios::binary };
        if (!in)
            return std::unexpected { std::format("cannot read {}", path.string()) };
        auto stored = std::string {};
        in >> stored;

        // A file that exists and does not hold an identity is an ERROR, never a
        // reason to mint a replacement. Minting would re-identify a fleet whose
        // workers have pinned the old value -- turning a corrupted byte into the
        // whole fleet refusing work -- and it would destroy the evidence of what
        // went wrong on the way.
        if (!IsWellFormedClusterId(stored))
            return std::unexpected { std::format(
                "{} does not hold a fleet identity; refusing to mint a replacement over it", path.string()) };
        return stored;
    }

    auto const minted = MintClusterId(random);
    std::filesystem::create_directories(path.parent_path(), error);
    if (error && !std::filesystem::exists(path.parent_path()))
        return std::unexpected { std::format("cannot create {}: {}", path.parent_path().string(), error.message()) };

    auto out = std::ofstream { path, std::ios::binary | std::ios::trunc };
    if (!out)
        return std::unexpected { std::format("cannot create {}", path.string()) };
    out << minted << '\n';
    out.flush();

    // Checked rather than assumed: a full or read-only filesystem fails at the
    // flush, and a scheduler that believed it had stored an identity would mint a
    // different one on every restart -- which is the failure the persistence exists
    // to prevent, arriving silently.
    if (!out)
        return std::unexpected { std::format("cannot write {}", path.string()) };
    return minted;
}

} // namespace FastCache::Cluster

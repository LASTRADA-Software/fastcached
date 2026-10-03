// SPDX-License-Identifier: Apache-2.0
#include "RaftStoreArchiver.hpp"

#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/PeerText.hpp>
#include <FastCache/Platform/FileTrust.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <format>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// The names Windows reserves for devices in every directory, whatever follows a dot: a
    /// directory spelled one of them is the device, not a directory.
    constexpr auto ReservedDeviceNames = std::to_array<std::string_view>({
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    });

    /// Whether @p c may appear in an archived cluster's id.
    [[nodiscard]] constexpr bool IsArchiveIdCharacter(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    /// Whether @p name, ignoring case, is a reserved device name.
    [[nodiscard]] bool NamesADevice(std::string_view name) noexcept
    {
        return std::ranges::any_of(ReservedDeviceNames, [name](std::string_view device) {
            return std::ranges::equal(name, device, [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
            });
        });
    }

    /// The name of @p clusterId's @p ordinal'th archive: `<id>` for the first, `<id>.<n>` after it.
    [[nodiscard]] std::string ArchiveNameOf(std::string_view clusterId, unsigned ordinal)
    {
        return ordinal == 0 ? std::string { clusterId } : std::format("{}.{}", clusterId, ordinal);
    }

    /// Whether @p suffix is what follows the dot of an archive the archiver names: the staging
    /// suffix, or an ordinal from 1 below `MaxArchivesPerCluster` written without a leading zero.
    [[nodiscard]] bool IsArchiveSuffix(std::string_view suffix) noexcept
    {
        if (suffix == ArchiveStagingSuffix)
            return true;
        // Nine digits at most, so the value below cannot overflow before it is compared.
        constexpr std::size_t MaxOrdinalDigits = 9;
        if (suffix.empty() || suffix.front() == '0' || suffix.size() > MaxOrdinalDigits
            || !std::ranges::all_of(suffix, [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        auto ordinal = 0U;
        for (auto const c: suffix)
            ordinal = (ordinal * 10U) + static_cast<unsigned>(c - '0');
        return ordinal < MaxArchivesPerCluster;
    }

    /// The store files @p directory holds, in `StoreFileNames()` order.
    [[nodiscard]] std::expected<std::vector<std::string_view>, std::string> StoreFilesIn(
        std::filesystem::path const& directory)
    {
        auto present = std::vector<std::string_view> {};
        for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        {
            auto failure = std::error_code {};
            auto const exists = std::filesystem::exists(directory / name, failure);
            if (failure)
                return std::unexpected { std::format(
                    "cannot ask whether {} exists: {}", (directory / name).string(), failure.message()) };
            if (exists)
                present.push_back(name);
        }
        return present;
    }
} // namespace

bool IsArchivableClusterId(std::string_view clusterId) noexcept
{
    return !clusterId.empty() && clusterId.size() <= CompileCacheWire::MaxIdBytes
           && std::ranges::all_of(clusterId, IsArchiveIdCharacter) && !NamesADevice(clusterId);
}

bool IsArchiveDirectoryName(std::string_view name) noexcept
{
    auto const dot = name.find('.');
    if (dot == std::string_view::npos)
        return IsArchivableClusterId(name);
    return IsArchivableClusterId(name.substr(0, dot)) && IsArchiveSuffix(name.substr(dot + 1));
}

RaftStoreArchiver::RaftStoreArchiver(std::filesystem::path directory):
    _directory { std::move(directory) }
{
}

std::expected<void, std::string> RaftStoreArchiver::Archive(std::string_view clusterId)
{
    if (!IsArchivableClusterId(clusterId))
        return std::unexpected { std::format("cannot archive the consensus store in {}: the cluster id '{}' cannot "
                                             "name a directory, so nothing was moved",
                                             _directory.string(),
                                             BoundedPeerText(clusterId, CompileCacheWire::MaxIdBytes)) };

    auto const archive = _directory / ArchiveDirectoryName;
    auto const staging = archive / std::format("{}.{}", clusterId, ArchiveStagingSuffix);
    auto const inRoot = StoreFilesIn(_directory);
    if (!inRoot.has_value())
        return std::unexpected { std::format(
            "cannot archive the consensus store of cluster {}: {}", clusterId, inRoot.error()) };
    auto failure = std::error_code {};
    auto const staged = std::filesystem::exists(staging, failure);
    if (failure)
        return std::unexpected { std::format(
            "cannot ask whether {} exists to archive cluster {}: {}", staging.string(), clusterId, failure.message()) };
    // Nothing in the root and nothing staged: never written, or archived completely by an earlier run.
    if (inRoot->empty() && !staged)
        return {};

    // The archive first and the staging directory in it second, each its owner's alone from its first
    // instant: `CreateOwnerOnlyDirectory` gives a missing PARENT only what a plain create does.
    for (auto const& directory: { archive, staging })
        if (auto const created = CreateOwnerOnlyDirectory(directory); !created.has_value())
            return std::unexpected { std::format("cannot create {} to archive the consensus store of cluster {}: {}",
                                                 directory.string(),
                                                 clusterId,
                                                 created.error().message()) };

    auto const alreadyStaged = StoreFilesIn(staging);
    if (!alreadyStaged.has_value())
        return std::unexpected { std::format(
            "cannot archive the consensus store of cluster {}: {}", clusterId, alreadyStaged.error()) };
    for (auto const name: *inRoot)
    {
        auto const source = _directory / name;
        auto const destination = staging / name;
        if (std::ranges::contains(*alreadyStaged, name))
            return std::unexpected { std::format(
                "{} and {} both exist, so the consensus store of cluster {} cannot be archived without writing over "
                "one of them -- and either may be the only copy of something. Move one aside by hand; nothing was "
                "overwritten",
                source.string(),
                destination.string(),
                clusterId) };
        std::filesystem::rename(source, destination, failure);
        if (failure)
            return std::unexpected { std::format("cannot move {} to {} to archive the consensus store of cluster {}: {}",
                                                 source.string(),
                                                 destination.string(),
                                                 clusterId,
                                                 failure.message()) };
    }

    // Complete: into the first name no earlier archive of this cluster holds, never over one.
    for (auto const ordinal: std::views::iota(0U, MaxArchivesPerCluster))
    {
        auto const into = archive / ArchiveNameOf(clusterId, ordinal);
        auto const taken = std::filesystem::exists(into, failure);
        if (failure)
            return std::unexpected { std::format(
                "cannot ask whether {} exists to archive cluster {}: {}", into.string(), clusterId, failure.message()) };
        if (taken)
            continue;
        std::filesystem::rename(staging, into, failure);
        if (failure)
            return std::unexpected { std::format("cannot rename {} to {} to finish archiving cluster {}: {}",
                                                 staging.string(),
                                                 into.string(),
                                                 clusterId,
                                                 failure.message()) };
        return {};
    }
    return std::unexpected { std::format("{} already holds {} archives of cluster {}, so this one is left in {}; "
                                         "remove the ones nobody needs and start again",
                                         archive.string(),
                                         MaxArchivesPerCluster,
                                         clusterId,
                                         staging.string()) };
}

} // namespace FastCache::Node

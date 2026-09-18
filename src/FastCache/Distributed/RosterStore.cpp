// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Distributed/RosterStore.hpp>

#include <expected>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace FastCache::Distributed
{

FileRosterStore::FileRosterStore(std::filesystem::path path):
    _path { std::move(path) }
{
}

std::expected<void, std::string> FileRosterStore::Save(Cluster::PersistedRoster const& roster)
{
    auto const bytes = Cluster::EncodePersistedRoster(roster);
    auto const replaced = Consensus::ReplaceFileAtomically(_path, bytes);
    if (!replaced.has_value())
        return std::unexpected { replaced.error().context };
    return {};
}

std::expected<std::optional<Cluster::PersistedRoster>, std::string> LoadPersistedRoster(std::filesystem::path const& path)
{
    auto const bytes = Consensus::ReadFileIfPresent(path);
    if (!bytes.has_value())
        return std::unexpected { std::format("the roster this machine kept cannot be read: {}", bytes.error().context) };
    if (!bytes->has_value())
        return std::optional<Cluster::PersistedRoster> {};

    // The roster INSIDE the certificate as well, because a kept roster whose members cannot be
    // read would otherwise be held as nothing -- the anchors again, by the back door.
    auto persisted = Cluster::DecodePersistedRoster(**bytes).and_then(
        [](Cluster::PersistedRoster kept) -> std::expected<Cluster::PersistedRoster, ConsensusError> {
            if (auto const roster = Cluster::DecodeRoster(kept.certificate.roster); !roster.has_value())
                return std::unexpected { roster.error() };
            return kept;
        });
    if (!persisted.has_value())
        return std::unexpected { std::format(
            "{} holds the roster this machine adopted, and it is not one this build can use ({}); move it aside only "
            "if this machine should trust its --voter-key anchors again",
            path.string(),
            persisted.error().context) };
    return std::optional { *std::move(persisted) };
}

} // namespace FastCache::Distributed

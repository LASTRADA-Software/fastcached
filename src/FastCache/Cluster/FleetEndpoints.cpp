// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// The four bytes every fleet-endpoints file starts with.
    constexpr std::array<std::byte, 4> Magic { std::byte { 'F' }, std::byte { 'C' }, std::byte { 'F' }, std::byte { 'E' } };

    /// Where the format byte sits: straight after the magic.
    constexpr std::size_t FormatOffset = Magic.size();

    /// The fixed bytes ahead of the fields: the magic, then the format byte.
    constexpr std::size_t HeaderBytes = FormatOffset + 1;

    /// Fields one voter occupies: id, consensus endpoint, node endpoint.
    constexpr std::size_t VoterFields = 3;

    /// Text read out of a field, owning its characters.
    /// @param field The field.
    /// @return The text.
    [[nodiscard]] std::string OwnedText(std::span<std::byte const> field)
    {
        return std::string { WireFields::AsStringView(field) };
    }

    /// One voter, as its own nested record: id, raft endpoint, node endpoint.
    /// @param voter The voter.
    /// @return Its blob.
    [[nodiscard]] std::vector<std::byte> EncodeVoter(FleetEndpoint const& voter)
    {
        return WireFields::Encode({ WireFields::AsBytes(voter.id),
                                    WireFields::AsBytes(voter.raftEndpoint),
                                    WireFields::AsBytes(voter.nodeEndpoint) });
    }

    /// Read a blob `EncodeVoter` wrote.
    /// @param blob The voter's blob.
    /// @return The voter, or nothing when it is not its three fields.
    [[nodiscard]] std::optional<FleetEndpoint> DecodeVoter(std::span<std::byte const> blob)
    {
        auto const fields = WireFields::SplitExactly(blob, VoterFields);
        if (!fields.has_value())
            return std::nullopt;
        return FleetEndpoint { .id = OwnedText((*fields)[0]),
                               .raftEndpoint = OwnedText((*fields)[1]),
                               .nodeEndpoint = OwnedText((*fields)[2]) };
    }

    /// Encode the fields after the header: the cluster id, then one nested field per voter.
    /// @param endpoints What to encode.
    /// @return The encoded fields.
    [[nodiscard]] std::vector<std::byte> EncodeFields(FleetEndpoints const& endpoints)
    {
        std::vector<std::vector<std::byte>> voterBlobs;
        voterBlobs.reserve(endpoints.voters.size());
        for (auto const& voter: endpoints.voters)
            voterBlobs.push_back(EncodeVoter(voter));

        std::vector<std::span<std::byte const>> fields;
        fields.reserve(1 + voterBlobs.size());
        fields.push_back(WireFields::AsBytes(endpoints.clusterId));
        for (auto const& blob: voterBlobs)
            fields.emplace_back(blob);

        return WireFields::Encode(WireFields::FieldList { fields });
    }

    /// Read the fields after the header: the cluster id, then one nested field per voter.
    ///
    /// `SplitAll` rather than a fixed arity, because the voter count is not known in advance --
    /// the cluster id is whichever field comes first, and everything after it is a voter.
    /// @param body The bytes after the header.
    /// @return The endpoints, or nothing when the body is malformed.
    [[nodiscard]] std::optional<FleetEndpoints> DecodeFields(std::span<std::byte const> body)
    {
        auto const fields = WireFields::SplitAll(body);
        if (!fields.has_value() || fields->empty())
            return std::nullopt;

        FleetEndpoints endpoints;
        endpoints.clusterId = OwnedText((*fields)[0]);
        endpoints.voters.reserve(fields->size() - 1);
        for (auto const field: std::span { *fields }.subspan(1))
        {
            auto voter = DecodeVoter(field);
            if (!voter.has_value())
                return std::nullopt;
            endpoints.voters.push_back(*std::move(voter));
        }
        return endpoints;
    }

    /// The file's path in @p directory.
    /// @param directory The state directory.
    /// @return The path.
    [[nodiscard]] std::filesystem::path EndpointsPath(std::filesystem::path const& directory)
    {
        return directory / std::string { FleetEndpointsFileName };
    }
} // namespace

FleetEndpointsFile::FleetEndpointsFile(std::filesystem::path directory):
    _directory { std::move(directory) }
{
}

LoadedFleetEndpoints FleetEndpointsFile::Load()
{
    auto const path = EndpointsPath(_directory);
    auto const read = Consensus::ReadFileIfPresent(path);
    if (!read.has_value())
        // Whatever kept this from being read -- permissions, a disk that gave up -- is not a
        // claim about which build wrote it, so it is answered like damage rather than absence.
        return { .outcome = FleetEndpointsLoad::Unreadable, .endpoints = {} };
    if (!read->has_value())
        return { .outcome = FleetEndpointsLoad::Absent, .endpoints = {} };

    auto const& bytes = **read;
    if (bytes.size() < HeaderBytes || !std::ranges::equal(std::span { bytes }.first(Magic.size()), Magic))
        return { .outcome = FleetEndpointsLoad::Unreadable, .endpoints = {} };

    // The format byte, before anything else about the bytes is judged: a layout this build did
    // not write is intact, and reading it as this build's would report damage that is not there.
    auto const format = static_cast<std::uint8_t>(bytes[FormatOffset]);
    if (format > FleetEndpointsFormat)
    {
        auto const guard = std::scoped_lock { _mutex };
        _readOnly = true;
        return { .outcome = FleetEndpointsLoad::LaterBuild, .endpoints = {} };
    }
    if (format < FleetEndpointsFormat)
        return { .outcome = FleetEndpointsLoad::Unreadable, .endpoints = {} };

    auto decoded = DecodeFields(std::span { bytes }.subspan(HeaderBytes));
    if (!decoded.has_value())
        return { .outcome = FleetEndpointsLoad::Unreadable, .endpoints = {} };

    return { .outcome = FleetEndpointsLoad::Loaded, .endpoints = *std::move(decoded) };
}

std::expected<void, ConsensusError> FleetEndpointsFile::Save(FleetEndpoints const& endpoints)
{
    if (ReadOnly())
        return std::unexpected(UnsupportedFormatVersion(std::format(
            "{}: a later build wrote this file; it is kept and never overwritten", EndpointsPath(_directory).string())));

    // Its owner's alone when this creates it, as the state directory always is.
    if (auto const created = CreateOwnerOnlyDirectory(_directory); !created.has_value())
        return std::unexpected(
            StorageFailure(std::format("cannot create {}: {}", _directory.string(), created.error().message())));

    std::vector<std::byte> out;
    auto const fields = EncodeFields(endpoints);
    out.reserve(HeaderBytes + fields.size());
    out.insert(out.end(), Magic.begin(), Magic.end());
    out.push_back(static_cast<std::byte>(FleetEndpointsFormat));
    out.insert(out.end(), fields.begin(), fields.end());

    return Consensus::ReplaceFileAtomically(EndpointsPath(_directory), out, StateFile::FleetEndpoints);
}

bool FleetEndpointsFile::ReadOnly() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return _readOnly;
}

std::vector<std::string> RememberedSeeds(FleetEndpoints const& endpoints)
{
    std::vector<std::string> out;
    out.reserve(endpoints.voters.size());
    for (auto const& voter: endpoints.voters)
        out.push_back(voter.nodeEndpoint);
    return out;
}

} // namespace FastCache::Cluster

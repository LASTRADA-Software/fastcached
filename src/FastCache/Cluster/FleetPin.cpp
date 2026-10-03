// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cluster
{

std::string FormatPinnedFleet(PinnedFleet const& fleet)
{
    auto text = fleet.clusterId;
    auto separator = '@';
    for (auto const& key: fleet.voterKeys)
    {
        text += separator;
        text += FormatEd25519PublicKey(key);
        separator = ',';
    }
    return text;
}

std::string PinText(FleetPin const& pin)
{
    return pin.fleet.has_value() ? FormatPinnedFleet(*pin.fleet) : std::string { "none" };
}

std::expected<PinnedFleet, std::string> ParsePinnedFleet(std::string_view text)
{
    constexpr auto Remedy = std::string_view { "append @<key>[,<key>...], the voters' identity keys, exactly as "
                                               "`fastcache-cli node` prints the fleet-id on a machine of that fleet" };
    auto const at = text.find('@');
    auto const clusterId = text.substr(0, at);
    if (!IsMintedClusterId(clusterId))
        return std::unexpected { std::format(
            "'{}' is not a cluster id: one is {} lowercase hex digits", clusterId, 2 * ClusterIdBytes) };
    if (at == std::string_view::npos || at + 1 == text.size())
        return std::unexpected { std::format(
            "names cluster {} and no voter key, and a pin by name alone stops nobody who can hear a beacon -- every "
            "beacon carries the id; {}",
            clusterId,
            Remedy) };

    auto fleet = PinnedFleet { .clusterId = std::string { clusterId }, .voterKeys = {} };
    for (auto const piece: text.substr(at + 1) | std::views::split(','))
    {
        auto const spelled = std::string_view { piece.begin(), piece.end() };
        if (fleet.voterKeys.size() == MaxPinnedVoterKeys)
            return std::unexpected { std::format("names more than {0} voter keys; a pin needs only some of its "
                                                 "fleet's voters, so name at most {0}",
                                                 MaxPinnedVoterKeys) };
        auto key = ParseEd25519PublicKey(spelled);
        if (!key.has_value())
            return std::unexpected { std::format("voter key {} ('{}') is not a key: {}",
                                                 fleet.voterKeys.size() + 1,
                                                 spelled,
                                                 DescribePublicKeyFault(key.error())) };
        if (std::ranges::contains(fleet.voterKeys, *key))
            return std::unexpected { std::format("names voter key {} twice", spelled) };
        fleet.voterKeys.push_back(*key);
    }
    return fleet;
}

} // namespace FastCache::Cluster

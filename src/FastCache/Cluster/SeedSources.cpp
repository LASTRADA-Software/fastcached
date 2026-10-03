// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// Sort SRV targets by priority ascending, weight descending.
    ///
    /// A fixed order rather than RFC 2782's weighted random draw, so a seed walk is reproducible
    /// and testable: `stable_sort` so two targets that tie on both keep the order the resolver
    /// reported them in.
    /// @param srv The targets, as a resolver returned them.
    /// @return The same targets, ordered.
    [[nodiscard]] std::vector<SrvTarget> BySrvPreference(std::span<SrvTarget const> srv)
    {
        std::vector<SrvTarget> ordered { srv.begin(), srv.end() };
        std::ranges::stable_sort(ordered, [](SrvTarget const& left, SrvTarget const& right) {
            if (left.priority != right.priority)
                return left.priority < right.priority;
            return left.weight > right.weight;
        });
        return ordered;
    }
} // namespace

std::optional<std::string> NormalizeSeed(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    if (auto const dial = ParseDialEndpoint(text); dial.has_value())
        return FormatHostPort(dial->first, dial->second);

    // A text `SplitHostPort` reads as `host:port` but `ParseDialEndpoint` refused -- an empty
    // host or an unusable port -- names nobody, and is not reinterpreted as a bare hostname:
    // `:6674` and `office-a:0` must not silently become a host named `:6674` or `office-a:0`.
    if (SplitHostPort(text).has_value())
        return std::nullopt;

    return FormatHostPort(text, DefaultSeedPort);
}

std::string SrvQueryName(std::string_view dnsSuffix)
{
    if (dnsSuffix.empty())
        return {};
    return std::format("_fastcache._tcp.{}", dnsSuffix);
}

std::vector<SeedCandidate> OrderSeeds(std::span<std::string const> remembered,
                                      std::span<std::string const> flagSeeds,
                                      std::span<SrvTarget const> srv)
{
    auto const orderedSrv = BySrvPreference(srv);

    std::vector<SeedCandidate> out;
    std::vector<std::string> seen;

    // A seed already present under an earlier source keeps that source: normalizing two
    // spellings of the same endpoint to one candidate is what lets a remembered endpoint
    // outrank the same machine named again by a flag or a record.
    auto const add = [&out, &seen](std::string_view text, SeedSource source) {
        auto normalized = NormalizeSeed(text);
        if (!normalized.has_value())
            return;
        if (std::ranges::contains(seen, *normalized))
            return;
        seen.push_back(*normalized);
        out.push_back(SeedCandidate { .endpoint = *std::move(normalized), .source = source });
    };

    for (auto const source: Enumerators<SeedSource>())
    {
        switch (source)
        {
            case SeedSource::Remembered:
                for (auto const& text: remembered)
                    add(text, source);
                break;
            case SeedSource::FleetSeedFlag:
                for (auto const& text: flagSeeds)
                    add(text, source);
                break;
            case SeedSource::DnsSrv:
                for (auto const& target: orderedSrv)
                    add(FormatHostPort(target.host, target.port), source);
                break;
            case SeedSource::Last:
                break;
        }
    }
    return out;
}

} // namespace FastCache::Cluster

// SPDX-License-Identifier: Apache-2.0
#include "LiveStatsSources.hpp"

#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A key a role and a leader make together: either changing is a leadership change.
    [[nodiscard]] LiveFact LeadershipFact(std::uint8_t role, std::string const& leader)
    {
        return LiveFact { .key = std::format("{}|{}", role, leader), .detail = leader };
    }

    /// Sort facts by key, which `DiffLiveEvents` walks as sets.
    void SortByKey(std::vector<LiveFact>& facts)
    {
        std::ranges::sort(facts, {}, &LiveFact::key);
    }
} // namespace

NodeLiveStatsSources::NodeLiveStatsSources(NodeLiveStatsParts parts) noexcept:
    _parts { std::move(parts) }
{
}

std::optional<LiveCapture> NodeLiveStatsSources::Capture(Wire::LiveSubject subject) const
{
    switch (subject)
    {
        case Wire::LiveSubject::Cache:
            return CaptureCacheSubject(*_parts.metrics, _parts.snapshot(), _parts.surfaces.Span());
        case Wire::LiveSubject::Node: {
            auto const reading =
                EncodeStatsReading(CaptureStatsReading(*_parts.metrics, _parts.snapshot(), _parts.surfaces.Span()));
            auto const status = _parts.identity->Describe();
            auto const described = Wire::EncodeNodeStatus(status);
            return LiveCapture { .body = WireFields::Encode(
                                     { std::span<std::byte const> { reading }, std::span<std::byte const> { described } }),
                                 .probe = ProbeNodeStatus(status) };
        }
        case Wire::LiveSubject::Fleet: {
            if (!_parts.fleet.has_value())
                return std::nullopt;
            auto const snapshot = Distributed::CollectFleet(*_parts.fleet);
            // No section and no range, which no table can refuse.
            auto const document = AnswerFleetText(snapshot, _parts.history, {}, {});
            if (!document.has_value())
                return std::nullopt;
            auto const bytes = Wire::AsBytes(document->body);
            return LiveCapture { .body = { bytes.begin(), bytes.end() }, .probe = ProbeFleet(snapshot) };
        }
    }
    return std::nullopt;
}

std::expected<FleetTextDocument, FleetTextDeclined> NodeLiveStatsSources::FleetText(std::string_view section,
                                                                                    std::string_view range) const
{
    if (!_parts.fleet.has_value())
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet,
                                                   .detail = "this node runs no scheduler, so it has no fleet" });
    return AnswerFleetText(Distributed::CollectFleet(*_parts.fleet), _parts.history, section, range);
}

std::optional<LiveLeadership> NodeLiveStatsSources::Leadership() const
{
    if (!_parts.fleet.has_value())
        return std::nullopt;
    auto const& scheduler = *_parts.fleet->scheduler;
    return LiveLeadership { .leads = scheduler.Role() == Distributed::SchedulerRole::Leader,
                            .leaderEndpoint = scheduler.LeaderEndpoint() };
}

LiveEventProbe ProbeNodeStatus(Wire::NodeStatusFields const& status)
{
    LiveEventProbe probe;
    auto const& runtime = status.runtime;
    if (runtime.schedulerRole.has_value())
        probe.leadership = LeadershipFact(static_cast<std::uint8_t>(*runtime.schedulerRole), runtime.leaderEndpoint);
    // The STATE is the key and the counts are the words: a survey walking its candidates moves
    // `served` every few seconds, and an event per toolchain hashed would drown the one that says
    // the survey finished.
    if (runtime.toolchains.has_value())
        probe.survey =
            LiveFact { .key = std::format("{}", static_cast<std::uint8_t>(*runtime.toolchains)),
                       .detail = std::format("{} of {}", runtime.toolchainsServed, runtime.toolchainsDiscovered) };
    if (runtime.enrollment.has_value())
        probe.enrollment = LiveFact { .key = std::format("{}", static_cast<std::uint8_t>(*runtime.enrollment)),
                                      .detail = std::format("{} pending", runtime.enrollmentPending.value_or(0)) };
    return probe;
}

LiveEventProbe ProbeFleet(Distributed::FleetSnapshot const& snapshot)
{
    LiveEventProbe probe;
    if (snapshot.cluster.has_value())
    {
        for (auto const& member: snapshot.cluster->members)
            probe.members.push_back(LiveFact { .key = member.id, .detail = member.id });
        SortByKey(probe.members);
    }
    // Keyed per (endpoint, toolchain), which is what the registry holds: a machine adding a
    // toolchain registers, and so is an event.
    for (auto const& worker: snapshot.workers)
        probe.workers.push_back(LiveFact { .key = std::format("{}\x1f{}", worker.info.endpoint, worker.info.fingerprint),
                                           .detail = worker.info.endpoint });
    SortByKey(probe.workers);
    probe.leadership = LeadershipFact(static_cast<std::uint8_t>(snapshot.role), snapshot.leaderEndpoint);
    return probe;
}

} // namespace FastCache::Node

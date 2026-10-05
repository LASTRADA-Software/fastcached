// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/BeaconDestinations.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Cluster
{

namespace
{
    /// @param destinations Where beacons go.
    /// @return Them, as one line of text.
    [[nodiscard]] std::string Listed(std::vector<core::net::DatagramAddress> const& destinations)
    {
        auto text = std::string {};
        for (auto const& destination: destinations)
        {
            if (!text.empty())
                text += ", ";
            text += FormatHostPort(destination.host, destination.port);
        }
        return text;
    }

    /// @param interfaces What the machine reported.
    /// @return Each interface address, and why it was passed over.
    [[nodiscard]] std::string PassedOver(std::vector<Ipv4InterfaceAddress> const& interfaces)
    {
        auto text = std::string {};
        for (auto const& entry: interfaces)
        {
            if (!text.empty())
                text += "; ";
            auto const& row = BroadcastEligibilityRows[static_cast<std::size_t>(EligibilityOf(entry))];
            text += std::format("{} {}/{} ({}: {})",
                                entry.interfaceName.empty() ? std::string { "unnamed" } : entry.interfaceName,
                                FormatIpv4(entry.address),
                                static_cast<int>(entry.prefixLength),
                                row.word,
                                row.why);
        }
        return text.empty() ? std::string { "the machine reported no IPv4 interface at all" } : text;
    }
} // namespace

DirectedBroadcastDestinations::DirectedBroadcastDestinations(IInterfaceAddressSource const& source,
                                                             core::platform::IClock& clock,
                                                             std::uint16_t port,
                                                             ILogger& logger,
                                                             std::chrono::milliseconds refreshInterval):
    _source { source },
    _clock { clock },
    _logger { logger },
    _refreshInterval { refreshInterval },
    _port { port },
    _sampledAt { clock.now() }
{
    std::scoped_lock const guard { _mutex };
    Refresh();
}

std::vector<core::net::DatagramAddress> DirectedBroadcastDestinations::Destinations() const
{
    std::scoped_lock const guard { _mutex };
    // On an interval, never per beacon: see the class note.
    if (auto const now = _clock.now(); now - _sampledAt >= _refreshInterval)
    {
        _sampledAt = now;
        Refresh();
    }
    return _destinations;
}

void DirectedBroadcastDestinations::Refresh() const
{
    auto const now = _clock.now();
    auto const walked = _source.Ipv4Addresses();
    if (!walked.has_value())
    {
        // Not "no interface": the platform never said. The destinations stay what they were, and
        // the error is counted when it is new and said at most once an interval.
        _lastWalkFailure = now;
        if (_walkFailure != walked.error())
        {
            _walkFailure = walked.error();
            _walkFailures.Count();
        }
        if (auto const errors = _walkFailures.Due(now); errors.has_value())
            _logger.Logf(LogLevel::Warn,
                         "discovery: could not list this machine's interfaces ({}), so beacons go {} until they "
                         "can be (asked again every {}s; {} new error(s) since the last such line, said at most "
                         "every {}s)",
                         walked.error().message(),
                         _destinations.empty() ? std::string { "nowhere" } : "to " + Listed(_destinations),
                         std::chrono::duration_cast<std::chrono::seconds>(_refreshInterval).count(),
                         *errors,
                         WalkFailureReportInterval.count());
        return;
    }
    auto const& interfaces = *walked;
    auto next = std::vector<core::net::DatagramAddress> {};
    for (auto const& entry: interfaces)
    {
        if (EligibilityOf(entry) != BroadcastEligibility::Eligible)
            continue;
        auto destination = core::net::DatagramAddress { .host = FormatIpv4(DirectedBroadcastOf(entry)), .port = _port };
        // Two addresses on one subnet share one broadcast: one beacon reaches both.
        if (std::ranges::none_of(next, [&destination](core::net::DatagramAddress const& held) {
                return held.host == destination.host && held.port == destination.port;
            }))
            next.push_back(std::move(destination));
    }

    auto const unchanged = std::ranges::equal(
        next, _destinations, [](auto const& a, auto const& b) { return a.host == b.host && a.port == b.port; });
    auto const firstSample = !_reported;
    // The end of a failure says where beacons go, changed or not: the failure was said, and without
    // this its end would not be. Only after a whole interval with no failed walk, though, or a walk
    // that fails every other time would end and begin again at every refresh.
    auto recovered = false;
    if (_lastWalkFailure.has_value() && now - *_lastWalkFailure >= WalkFailureReportInterval)
    {
        recovered = true;
        _lastWalkFailure.reset();
        _walkFailure.reset();
        _walkFailures.Discard();
    }
    if (unchanged && !firstSample && !recovered)
        return;
    _reported = true;

    // Said on every change, and on the first answer, because where beacons go is what an operator
    // reading "no peer ever appeared" needs first -- and none at all is a warning with the remedy.
    if (next.empty())
        _logger.Logf(LogLevel::Warn,
                     "discovery: no interface to beacon on, so no other machine on any link hears this node until "
                     "one comes up (asked again every {}s). Passed over: {}. To beacon somewhere anyway, name it: "
                     "--discovery=<address>:{}; or, for a machine with no broadcast link, reach the fleet by "
                     "--fleet-seed=<host[:port]> or a DNS SRV record, _fastcache._tcp.<domain>",
                     std::chrono::duration_cast<std::chrono::seconds>(_refreshInterval).count(),
                     PassedOver(interfaces),
                     _port);
    else
        _logger.Logf(LogLevel::Info, "discovery: beaconing to {} -- each up interface's directed broadcast", Listed(next));
    _destinations = std::move(next);
}

} // namespace FastCache::Cluster

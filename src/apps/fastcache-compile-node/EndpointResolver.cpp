// SPDX-License-Identifier: Apache-2.0
#include "EndpointResolver.hpp"
#include "NodeDefaults.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// Whether a host event can mean this machine now routes from another address. PRIVATE.
    struct StaleRouteRow
    {
        HostEvent event;  ///< The event this row stands for.
        bool stalesRoute; ///< Whether the next refresh must probe again.
    };

    /// A resume is a machine that may have woken on another network; a suspend moves nothing yet.
    constexpr auto StalesRoute = EnumTable<HostEvent, StaleRouteRow> { {
        { .event = HostEvent::Suspending, .stalesRoute = false },
        { .event = HostEvent::Resumed, .stalesRoute = true },
        { .event = HostEvent::NetworkChanged, .stalesRoute = true },
    } };
    static_assert(RowsInEnumeratorOrder(StalesRoute, &StaleRouteRow::event),
                  "StalesRoute must hold one row per HostEvent, in enumerator order");

    /// What to say, once, when `--advertise` is pinned in a given mode; empty for nothing.
    struct PinnedNoticeRow
    {
        AdvertiseMode mode;      ///< The mode this row stands for.
        std::string_view notice; ///< A format taking the flag's value, or empty.
    };

    /// A pinned NAME is re-resolved by every peer, so it still follows a DNS update; a pinned
    /// LITERAL follows nothing, and an operator who typed one as a work-around should hear that
    /// roaming is off.
    constexpr auto PinnedNotices = EnumTable<AdvertiseMode, PinnedNoticeRow> { {
        { .mode = AdvertiseMode::Auto, .notice = {} },
        { .mode = AdvertiseMode::PinnedName, .notice = {} },
        { .mode = AdvertiseMode::PinnedLiteral,
          .notice = "--advertise {} is a literal address: this node will not follow network changes" },
    } };
    static_assert(RowsInEnumeratorOrder(PinnedNotices, &PinnedNoticeRow::mode),
                  "PinnedNotices must hold one row per AdvertiseMode, in enumerator order");

    /// Whether anything @p cfg publishes is derived from the route, so a probe can change it.
    /// @param cfg The configuration in force.
    /// @return True when either flag's mode row follows the route.
    [[nodiscard]] bool FollowsRoute(NodeConfig const& cfg) noexcept
    {
        return std::ranges::any_of(
            std::array { std::string_view { cfg.advertise }, std::string_view { cfg.raftSelf } },
            [](std::string_view value) { return AdvertiseModeRowFor(AdvertiseModeOf(value)).followsRoute; });
    }
} // namespace

bool IsMulticastHost(std::string_view host) noexcept
{
    auto const bare = UnmappedHost(host);
    auto const v6 = bare.contains(':');
    auto const first = bare.substr(0, bare.find(v6 ? ':' : '.'));
    unsigned value = 0;
    auto const [end, error] = std::from_chars(first.data(), first.data() + first.size(), value, v6 ? 16 : 10);
    if (first.empty() || error != std::errc {} || end != first.data() + first.size())
        return false;
    // `ff00::/8` is a whole first group of four digits starting `ff`; `224.0.0.0/4` is a first
    // octet of 224 to 239.
    constexpr auto V6GroupDigits = std::size_t { 4 };
    constexpr auto V6Multicast = 0xff00U;
    constexpr auto V4First = 224U;
    constexpr auto V4Last = 239U;
    return v6 ? first.size() == V6GroupDigits && value >= V6Multicast : value >= V4First && value <= V4Last;
}

bool IsUsableRouteHost(std::string_view host) noexcept
{
    return std::ranges::none_of(UnusableRouteHosts, [host](UnusableRouteHostRow const& row) { return row.applies(host); });
}

std::optional<std::string> ProbeRouteHost(IRouteProbe const& probe, std::span<std::string const> preferred)
{
    auto const ask = [&probe](std::string_view target) -> std::optional<std::string> {
        auto answer = probe.SourceFor(target);
        if (!answer.has_value() || !IsUsableRouteHost(*answer))
            return std::nullopt;
        return std::move(answer).value();
    };
    for (auto const& target: preferred)
        if (auto found = ask(target))
            return found;
    for (auto const target: DefaultRouteProbeTargets)
        if (auto found = ask(target))
            return found;
    return std::nullopt;
}

std::vector<std::string> SchedulerProbeTargets::Targets() const
{
    // A name is skipped: the probe resolves nothing. Repeats are asked once.
    std::vector<std::string> targets;
    for (auto const& endpoint: _schedulers.Current())
        if (auto const host = HostOfEndpoint(endpoint);
            IsIpLiteralHost(host) && std::ranges::find(targets, host) == targets.end())
            targets.emplace_back(host);
    return targets;
}

EndpointResolver::EndpointResolver(INodeConfigSource const& config,
                                   IRouteProbe const& probe,
                                   IProbeTargets const& targets,
                                   core::platform::IClock const& clock,
                                   RouteHostCell& routeHost,
                                   AnnouncedEndpoint& node,
                                   AnnouncedEndpoint& raft,
                                   IMetricsSink& metrics,
                                   ILogger& logger):
    _config { config },
    _probe { probe },
    _targets { targets },
    _clock { clock },
    _routeHost { routeHost },
    _metrics { metrics },
    _logger { logger },
    _published { {
        { .target = node,
          .derive = [](NodeConfig const& cfg) { return AdvertisedEndpoint(cfg); },
          .counter = IMetricsSink::Counter::NodeEndpointChanges,
          .what = "endpoint",
          .tellsWatchers = true },
        { .target = raft,
          .derive = [](NodeConfig const& cfg) { return RaftSelfEndpoint(cfg); },
          .counter = IMetricsSink::Counter::NodeRaftEndpointChanges,
          .what = "raft endpoint",
          .tellsWatchers = false },
    } }
{
    // Adopted, not moved: what a body is seeded with may be the START's route, and the cell holds
    // where the node has roamed since. Nobody reads either endpoint yet, so nothing is told.
    auto cfg = _config.Current();
    ApplyRouteHost(cfg, _routeHost.Current());
    for (auto const& row: _published)
        if (auto derived = row.derive(cfg); !derived.empty())
            row.target.Publish(std::move(derived));
}

void EndpointResolver::Refresh()
{
    auto const moved = [this] {
        auto const lock = std::scoped_lock { _refreshMutex };
        return RefreshLocked();
    }();
    if (!moved)
        return;
    // Outside the refresh lock: a watcher wakes a thread that may be about to refresh itself.
    auto const lock = std::scoped_lock { _watchMutex };
    for (auto* const watcher: _watchers)
        watcher->OnEndpointMoved();
}

void EndpointResolver::Watch(IEndpointMoveSink& sink)
{
    auto const lock = std::scoped_lock { _watchMutex };
    _watchers.push_back(&sink);
}

void EndpointResolver::Unwatch(IEndpointMoveSink& sink) noexcept
{
    auto const lock = std::scoped_lock { _watchMutex };
    std::erase(_watchers, &sink);
}

bool EndpointResolver::RefreshLocked()
{
    // Consumed whether or not a probe follows: a node whose flags follow no route must not leave
    // the flag raised, or the background wait would wake for it forever.
    auto const stale = _stale.exchange(false);
    auto cfg = _config.Current();
    NoticePinned(cfg.advertise);

    auto const now = _clock.now();
    auto const due = stale || !_lastProbe.has_value() || now - *_lastProbe >= RefreshInterval;
    if (due && FollowsRoute(cfg))
    {
        _lastProbe = now;
        // Nothing usable keeps the last route host: a machine that dropped off the network still
        // answers wherever it comes back, and an empty or loopback endpoint helps nobody. The probe
        // is stamped either way, so a lost route is asked again at the next event or interval.
        if (auto found = ProbeRouteHost(_probe, _targets.Targets()))
            _routeHost.Set(*std::move(found));
    }

    // The live route host, over whatever the snapshot carries: a reload candidate was shaped by the
    // same cell, but the snapshot a body started with was shaped by the START's probe.
    ApplyRouteHost(cfg, _routeHost.Current());
    auto told = false;
    for (auto const& row: _published)
        if (PublishIfMoved(row, cfg) && row.tellsWatchers)
            told = true;
    return told;
}

std::string EndpointResolver::RouteHost() const
{
    return _routeHost.Current();
}

void EndpointResolver::OnHostEvent(HostEvent event) noexcept
{
    if (!StalesRoute.at(static_cast<std::size_t>(event)).stalesRoute)
        return;
    {
        auto const lock = std::scoped_lock { _wakeMutex };
        _stale = true;
    }
    _wake.notify_all();
}

std::jthread EndpointResolver::Launch()
{
    return std::jthread { [this](std::stop_token const& stop) { Loop(stop); } };
}

void EndpointResolver::Loop(std::stop_token const& stop)
{
    while (!stop.stop_requested())
    {
        Refresh();
        // A named lock, because the stop-token `wait_for` takes it by non-const reference.
        std::unique_lock lock { _wakeMutex };
        (void) _wake.wait_for(lock, stop, RefreshInterval, [this] { return _stale.load(); });
    }
}

void EndpointResolver::NoticePinned(std::string_view advertise)
{
    auto const& row = PinnedNotices.at(static_cast<std::size_t>(AdvertiseModeOf(advertise)));
    if (row.notice.empty() || advertise == _pinnedNoticed)
        return;
    _pinnedNoticed = advertise;
    _logger.Log(LogLevel::Warn, std::vformat(row.notice, std::make_format_args(advertise)));
}

bool EndpointResolver::PublishIfMoved(Published const& row, NodeConfig const& cfg)
{
    auto derived = row.derive(cfg);
    auto const was = row.target.Current();
    if (derived.empty() || derived == was)
        return false;
    _logger.Logf(LogLevel::Info, "{} {} -> {}", row.what, was, derived);
    row.target.Publish(std::move(derived));
    _metrics.Increment(row.counter);
    return true;
}

} // namespace FastCache::Node

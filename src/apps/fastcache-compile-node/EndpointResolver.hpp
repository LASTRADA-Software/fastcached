// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LiveNodeConfig.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/RouteProbe.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file EndpointResolver.hpp
/// The one place that decides which address this node is reachable at right now, and the only
/// writer of the two endpoints it publishes (the `0xFC` one and the Raft one).

namespace FastCache::Node
{

/// Literal IP hosts worth probing before the default route (e.g. applied schedulers given as IPs).
///
/// A seam because which hosts matter is the fleet's answer at the moment of asking -- the schedulers
/// an applied state records move -- and a case asserts the probe ORDER against a fixed list.
class IProbeTargets
{
  public:
    IProbeTargets() = default;
    IProbeTargets(IProbeTargets const&) = delete;
    IProbeTargets(IProbeTargets&&) = delete;
    IProbeTargets& operator=(IProbeTargets const&) = delete;
    IProbeTargets& operator=(IProbeTargets&&) = delete;
    virtual ~IProbeTargets() = default;

    /// @return IP literals (no port, no brackets) to probe first, most preferred first; may be empty.
    [[nodiscard]] virtual std::vector<std::string> Targets() const = 0;
};

/// Production's: the IP-literal hosts of where this node registers (`ISchedulerEndpointSource`), so
/// a machine with several routes advertises the address its fleet actually reaches it from. For a
/// worker or a learner those are its fleet's voters; for a VOTER its own scheduler (loopback, which
/// no probe answer survives) and then the other voters its applied state records
/// (`AppliedSchedulers`), so a split-tunnel voter advertises the address its peers reach. A
/// scheduler named by a host NAME is skipped: the probe resolves nothing, and resolving one here
/// would put an unbounded DNS wait on every refresh.
class SchedulerProbeTargets final: public IProbeTargets
{
  public:
    /// @param schedulers Where this node registers, re-read at every call; must outlive this.
    explicit SchedulerProbeTargets(ISchedulerEndpointSource const& schedulers) noexcept:
        _schedulers { schedulers }
    {
    }

    /// @copydoc IProbeTargets::Targets
    [[nodiscard]] std::vector<std::string> Targets() const override;

  private:
    ISchedulerEndpointSource const& _schedulers;
};

class IEndpointMoveSink;

/// Something that re-derives the endpoints this node publishes, when it is due.
///
/// The heartbeat and the presence loop call it at the top of every round, so a round never
/// announces an address a network change has already invalidated -- whichever thread heard the
/// change first. A seam so a worker case can drive registrations with the endpoint it publishes
/// itself.
class IEndpointRefresh
{
  public:
    IEndpointRefresh() = default;
    IEndpointRefresh(IEndpointRefresh const&) = delete;
    IEndpointRefresh(IEndpointRefresh&&) = delete;
    IEndpointRefresh& operator=(IEndpointRefresh const&) = delete;
    IEndpointRefresh& operator=(IEndpointRefresh&&) = delete;
    virtual ~IEndpointRefresh() = default;

    /// Re-probe if stale and publish what moved. Cheap when nothing is due; safe from any thread.
    virtual void Refresh() = 0;

    /// @param sink Told whenever the `0xFC` endpoint is published moved, from now on; must outlive
    ///        its watch, and is watched at most once.
    virtual void Watch(IEndpointMoveSink& sink) = 0;

    /// @param sink Told nothing once this returns -- a notification to it already in flight on
    ///        another thread has finished by then.
    virtual void Unwatch(IEndpointMoveSink& sink) noexcept = 0;
};

/// Told that the `0xFC` endpoint was published moved, on the publishing thread.
///
/// The worker's half of a move: the registrations must follow at once, because the scheduler goes
/// on granting for the endpoint they were filed under until they do. A sink returns promptly -- it
/// wakes a thread of its own -- and never calls back into the publisher.
class IEndpointMoveSink
{
  public:
    IEndpointMoveSink() = default;
    IEndpointMoveSink(IEndpointMoveSink const&) = delete;
    IEndpointMoveSink(IEndpointMoveSink&&) = delete;
    IEndpointMoveSink& operator=(IEndpointMoveSink const&) = delete;
    IEndpointMoveSink& operator=(IEndpointMoveSink&&) = delete;
    virtual ~IEndpointMoveSink() = default;

    /// The published `0xFC` endpoint moved.
    virtual void OnEndpointMoved() noexcept = 0;
};

/// A watch, ended when this is destroyed. Declared AFTER the sink it names, so the sink outlives it.
class EndpointMoveWatch
{
  public:
    /// @param refresh What publishes. @param sink What is told.
    EndpointMoveWatch(IEndpointRefresh& refresh, IEndpointMoveSink& sink):
        _refresh { refresh },
        _sink { sink }
    {
        _refresh.Watch(_sink);
    }

    ~EndpointMoveWatch()
    {
        _refresh.Unwatch(_sink);
    }

    EndpointMoveWatch(EndpointMoveWatch const&) = delete;
    EndpointMoveWatch(EndpointMoveWatch&&) = delete;
    EndpointMoveWatch& operator=(EndpointMoveWatch const&) = delete;
    EndpointMoveWatch& operator=(EndpointMoveWatch&&) = delete;

  private:
    IEndpointRefresh& _refresh;
    IEndpointMoveSink& _sink;
};

/// The route host this process last probed, shared by every body's resolver (its writer) and by
/// every reload candidate (`ReloadBasis::routeHost`, its reader).
///
/// Owned by `main` for the process rather than by one resolver, because a resolver lives as long
/// as a serving body and the reloader outlives every body: a candidate shaped without the live
/// route host derives a different endpoint from the running node's, and the startup table would
/// then judge an address nobody advertises.
class RouteHostCell
{
  public:
    /// @param initial What the start probed; empty for none.
    explicit RouteHostCell(std::string initial = {}):
        _host { std::move(initial) }
    {
    }

    /// @return The last usable route host, empty when none was ever found.
    [[nodiscard]] std::string Current() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _host;
    }

    /// @param host The newly probed route host; never empty (a failed probe keeps the last).
    void Set(std::string host)
    {
        auto const lock = std::scoped_lock { _mutex };
        _host = std::move(host);
    }

  private:
    mutable std::mutex _mutex;
    std::string _host;
};

/// Why a probed source address is refused as this node's route host. PRIVATE: never transmitted.
enum class UnusableRouteHost : std::uint8_t
{
    Loopback,    ///< Reaches only this machine.
    LinkLocal,   ///< `169.254/16` or `fe80::/10`: names a different machine on every link.
    Unspecified, ///< The wildcard: names no machine at all.
    Multicast,   ///< A group, never one machine.
    Last,
};

/// One reason to refuse a probed address, and the predicate that raises it.
struct UnusableRouteHostRow
{
    UnusableRouteHost reason; ///< The reason this row stands for.
    /// @param host A probed address, unbracketed. @return True to refuse it.
    bool (*applies)(std::string_view host);
};

/// Whether @p host is an IPv4 (`224.0.0.0/4`) or IPv6 (`ff00::/8`) multicast literal.
/// @param host An address, unbracketed; the IPv4-mapped form is unmapped first.
/// @return True for a multicast group.
[[nodiscard]] bool IsMulticastHost(std::string_view host) noexcept;

/// What a route probe may never yield as this node's address: every row asked of every result.
///
/// **This is the guard the withheld-name protection relies on.** `AdvertisedNameWithheld` stands
/// down once `NodeConfig::routeHost` is engaged, so a loopback route host would be advertised to the
/// fleet unchallenged -- a peer told to dial it reaches itself.
inline constexpr EnumTable<UnusableRouteHost, UnusableRouteHostRow> UnusableRouteHosts { {
    { .reason = UnusableRouteHost::Loopback, .applies = [](std::string_view host) { return IsLoopbackHost(host); } },
    { .reason = UnusableRouteHost::LinkLocal, .applies = [](std::string_view host) { return IsLinkLocalHost(host); } },
    { .reason = UnusableRouteHost::Unspecified,
      .applies = [](std::string_view host) { return IsWildcardHost(UnmappedHost(host)); } },
    { .reason = UnusableRouteHost::Multicast, .applies = [](std::string_view host) { return IsMulticastHost(host); } },
} };

static_assert(RowsInEnumeratorOrder(UnusableRouteHosts, &UnusableRouteHostRow::reason),
              "UnusableRouteHosts must hold one row per UnusableRouteHost, in order");

/// @param host A probed source address.
/// @return True when no row of `UnusableRouteHosts` refuses it.
[[nodiscard]] bool IsUsableRouteHost(std::string_view host) noexcept;

/// Ask @p probe for the address this machine routes from: @p preferred first, then
/// `DefaultRouteProbeTargets`; the first usable answer wins.
/// @param probe The kernel, or a fake.
/// @param preferred Literal targets to try before the default route.
/// @return The route host, or nullopt when no target yielded a usable address.
[[nodiscard]] std::optional<std::string> ProbeRouteHost(IRouteProbe const& probe, std::span<std::string const> preferred);

/// The sole publisher of this node's endpoints: the `0xFC` one every registration and lease check
/// reads, and the Raft one consensus advertises.
///
/// **Probing is gated, deriving is not.** A probe runs when an event marked the route stale,
/// `RefreshInterval` has passed since the last one, or the preferred targets moved since it (a
/// voter's arrive with the state it applies, after the start probed the default route); every `Refresh` re-derives both
/// endpoints from the configuration in force with the last route host applied, so a reload that re-pins
/// `--advertise` is published at the next refresh and a reload that changes nothing keeps the
/// roamed address. A probe that finds nothing usable keeps the last route host: this never
/// publishes an empty or loopback address because the network went away.
///
/// **Construction adopts, it does not move.** Both endpoints are set from the configuration and
/// the cell's live route host when this is built -- silently, uncounted -- so a body reformed after
/// a roam starts at the address the node already advertises rather than "moving" back to it from
/// the start's.
///
/// **A published `0xFC` move tells every watcher at once** (`Watch`): the worker's heartbeat wakes
/// and re-registers, rather than leaving the scheduler granting for the old endpoint for up to a
/// heartbeat interval.
class EndpointResolver final: public IHostEventSink, public IEndpointRefresh
{
  public:
    /// How often a route is re-probed when no event says it moved: the safety net under a
    /// watcher that could not start or a change it did not report.
    static constexpr std::chrono::seconds RefreshInterval { 30 };

    /// @param config   The configuration in force (LiveNodeConfig / reloader snapshot source).
    /// @param probe    Route probe.
    /// @param targets  Preferred targets (may return none).
    /// @param clock    What `RefreshInterval` is measured against.
    /// @param routeHost Where the last route host is kept for the process; seeds the first
    ///        derivation, so a body that starts offline keeps the address the last one probed.
    /// @param node     Published 0xFC endpoint; set from the live derivation here, uncounted.
    /// @param raft     Published Raft endpoint; set from the live derivation here, uncounted.
    /// @param metrics  Where each published move is counted.
    /// @param logger   Where each move, and a pinned literal, is said.
    /// Every reference is borrowed and must outlive this.
    EndpointResolver(INodeConfigSource const& config,
                     IRouteProbe const& probe,
                     IProbeTargets const& targets,
                     core::platform::IClock const& clock,
                     RouteHostCell& routeHost,
                     AnnouncedEndpoint& node,
                     AnnouncedEndpoint& raft,
                     IMetricsSink& metrics,
                     ILogger& logger);

    ~EndpointResolver() override = default;
    EndpointResolver(EndpointResolver const&) = delete;
    EndpointResolver(EndpointResolver&&) = delete;
    EndpointResolver& operator=(EndpointResolver const&) = delete;
    EndpointResolver& operator=(EndpointResolver&&) = delete;

    /// Re-probe if stale (event seen, or older than RefreshInterval) and publish what moved.
    void Refresh() override;

    /// @return The last probed route host, empty when none was ever found.
    [[nodiscard]] std::string RouteHost() const;

    /// Marks the route stale for the events `StalesRoute` names and wakes `Launch`'s thread;
    /// never probes on the delivering thread.
    /// @param event What the host said.
    void OnHostEvent(HostEvent event) noexcept override;

    /// @copydoc IEndpointRefresh::Watch
    void Watch(IEndpointMoveSink& sink) override;

    /// @copydoc IEndpointRefresh::Unwatch
    void Unwatch(IEndpointMoveSink& sink) noexcept override;

    /// Refresh on a thread of this resolver's own: at once, then whenever an event marks the route
    /// stale or `RefreshInterval` passes.
    /// @return The thread; destroying it stops and joins it. Must not outlive this resolver.
    [[nodiscard]] std::jthread Launch();

  private:
    /// One endpoint this resolver publishes: where, derived how, counted and said as what.
    struct Published
    {
        AnnouncedEndpoint& target;                    ///< The published value.
        std::string (*derive)(NodeConfig const& cfg); ///< Its derivation from a configuration.
        IMetricsSink::Counter counter;                ///< Counted on each move.
        std::string_view what;                        ///< Named in the move's log line.
        bool tellsWatchers;                           ///< Whether a move is told to `Watch`ers.
    };

    /// The background thread's body.
    void Loop(std::stop_token const& stop);

    /// Say once that a literal `--advertise` turns roaming off.
    void NoticePinned(std::string_view advertise);

    /// Publish @p row's derivation from @p cfg when it is non-empty and moved.
    /// @return True when it moved.
    bool PublishIfMoved(Published const& row, NodeConfig const& cfg);

    /// The refresh itself, under `_refreshMutex`.
    /// @return True when a row that tells watchers moved.
    bool RefreshLocked();

    INodeConfigSource const& _config;
    IRouteProbe const& _probe;
    IProbeTargets const& _targets;
    core::platform::IClock const& _clock;
    RouteHostCell& _routeHost;
    IMetricsSink& _metrics;
    ILogger& _logger;
    std::array<Published, 2> _published;

    /// Held for a whole refresh: the heartbeat, the presence loop and this resolver's own thread
    /// all refresh, and a probe and its publication are one step.
    std::mutex _refreshMutex;
    std::optional<core::platform::SteadyTimePoint> _lastProbe; ///< Guarded by `_refreshMutex`.
    std::vector<std::string> _lastTargets;                     ///< What it asked; guarded by `_refreshMutex`.
    std::string _pinnedNoticed;                                ///< Guarded by `_refreshMutex`.

    /// Set by an event, consumed by the next refresh. Written under `_wakeMutex` so the
    /// background thread's wait cannot miss it.
    std::atomic<bool> _stale { false };
    std::mutex _wakeMutex;
    std::condition_variable_any _wake;

    /// Told of each `0xFC` move; held while they are told, so `Unwatch` waits out a notification.
    std::mutex _watchMutex;
    std::vector<IEndpointMoveSink*> _watchers; ///< Guarded by `_watchMutex`.
};

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#include "EndpointResolver.hpp"
#include "LiveNodeConfig.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/RouteProbe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// The default route's IPv4 target, which every case answers through unless it says otherwise.
std::string const DefaultRoute { DefaultRouteProbeTargets.front() };

/// A route probe whose answers a case scripts per target, recording what it was asked.
class FakeRouteProbe final: public IRouteProbe
{
  public:
    /// Answer @p target with @p answer from now on.
    /// @param target An IP literal.
    /// @param answer What the kernel would say.
    void Answer(std::string target, std::expected<std::string, RouteProbeError> answer)
    {
        auto const lock = std::scoped_lock { _mutex };
        _answers.insert_or_assign(std::move(target), std::move(answer));
    }

    /// @copydoc IRouteProbe::SourceFor
    [[nodiscard]] std::expected<std::string, RouteProbeError> SourceFor(std::string_view target) const override
    {
        auto const lock = std::scoped_lock { _mutex };
        _asked.emplace_back(target);
        auto const found = _answers.find(target);
        if (found == _answers.end())
            return std::unexpected { RouteProbeError::NoRoute };
        return found->second;
    }

    /// @return Every target asked, in order.
    [[nodiscard]] std::vector<std::string> Asked() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _asked;
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, std::expected<std::string, RouteProbeError>, std::less<>> _answers;
    mutable std::vector<std::string> _asked;
};

/// A configuration source a case replaces, as an accepted reload would.
class FixedConfig final: public INodeConfigSource
{
  public:
    /// @param cfg What is in force until `Replace`.
    explicit FixedConfig(NodeConfig cfg):
        _cfg { std::move(cfg) }
    {
    }

    /// @copydoc INodeConfigSource::Current
    [[nodiscard]] NodeConfig Current() const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return _cfg;
    }

    /// @param cfg The configuration in force from now on.
    void Replace(NodeConfig cfg)
    {
        auto const lock = std::scoped_lock { _mutex };
        _cfg = std::move(cfg);
    }

  private:
    mutable std::mutex _mutex;
    NodeConfig _cfg;
};

/// Preferred targets a case fixes.
class FixedTargets final: public IProbeTargets
{
  public:
    /// @copydoc IProbeTargets::Targets
    [[nodiscard]] std::vector<std::string> Targets() const override
    {
        return targets;
    }

    std::vector<std::string> targets; ///< What `Targets` answers.
};

/// Scheduler endpoints a case fixes.
class FixedSchedulers final: public ISchedulerEndpointSource
{
  public:
    /// @param endpoints What `Current` answers.
    explicit FixedSchedulers(std::vector<std::string> endpoints):
        _endpoints { std::move(endpoints) }
    {
    }

    /// @copydoc ISchedulerEndpointSource::Current
    [[nodiscard]] std::vector<std::string> Current() const override
    {
        return _endpoints;
    }

  private:
    std::vector<std::string> _endpoints;
};

/// A node binding the wildcard on its Node and Raft ports, named `box.lan`, as the start shaped it:
/// with @p routeHost probed (or none).
/// @param routeHost What the start's probe found; empty for nothing.
/// @return The configuration.
[[nodiscard]] NodeConfig WildcardNode(std::string routeHost)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeListen = "0.0.0.0:6674";
    ApplyHostNames(cfg, NodeHostNames { .fqdn = "box.lan", .dnsSuffix = "lan", .withheld = {} });
    ApplyRouteHost(cfg, std::move(routeHost));
    return cfg;
}

/// Everything a resolver borrows, seeded as `main` seeds it: the start's probe applied to the
/// configuration, the cell and both published endpoints.
struct ResolverRig
{
    /// @param startedAt What the start's probe found; empty for nothing (then the name stands in).
    /// @param roamedTo What the process's cell holds when the resolver is built; defaults to
    ///        @p startedAt, a first body. Another value is a body reformed after a roam.
    explicit ResolverRig(std::string const& startedAt = "10.0.0.5", std::optional<std::string> const& roamedTo = {}):
        config { WildcardNode(startedAt) },
        routeHost { roamedTo.value_or(startedAt) },
        node { AdvertisedEndpoint(config.Current()) },
        raft { RaftSelfEndpoint(config.Current()) }
    {
    }

    FakeRouteProbe probe;
    FixedTargets targets;
    FixedConfig config;
    core::platform::ManualClock clock;
    RouteHostCell routeHost;
    AnnouncedEndpoint node;
    AnnouncedEndpoint raft;
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    EndpointResolver resolver { config, probe, targets, clock, routeHost, node, raft, metrics, logger };

    /// @return How often the node endpoint moved.
    [[nodiscard]] std::uint64_t NodeMoves() const
    {
        return metrics.Read(IMetricsSink::Counter::NodeEndpointChanges);
    }

    /// @return How often the Raft endpoint moved.
    [[nodiscard]] std::uint64_t RaftMoves() const
    {
        return metrics.Read(IMetricsSink::Counter::NodeRaftEndpointChanges);
    }

    /// @param level The level to count.
    /// @param fragment Text the record must contain.
    /// @return How many records match.
    [[nodiscard]] std::size_t Logged(LogLevel level, std::string_view fragment) const
    {
        auto const records = logger.Snapshot();
        return static_cast<std::size_t>(std::ranges::count_if(records, [level, fragment](auto const& record) {
            return record.level == level && record.message.contains(fragment);
        }));
    }
};

} // namespace

TEST_CASE("The resolver publishes the routed address at its first refresh", "[node][endpoint]")
{
    // Started offline: the name stands in, and the first route found replaces it.
    ResolverRig rig { "" };
    REQUIRE(rig.node.Current() == "box.lan:6674");
    rig.probe.Answer(DefaultRoute, "10.0.0.5");

    rig.resolver.Refresh();

    CHECK(rig.node.Current() == "10.0.0.5:6674");
    CHECK(rig.raft.Current() == "10.0.0.5:6680");
    CHECK(rig.resolver.RouteHost() == "10.0.0.5");
    CHECK(rig.NodeMoves() == 1);
    CHECK(rig.RaftMoves() == 1);
    CHECK(rig.Logged(LogLevel::Info, "endpoint box.lan:6674 -> 10.0.0.5:6674") == 1);
    CHECK(rig.Logged(LogLevel::Info, "raft endpoint box.lan:6680 -> 10.0.0.5:6680") == 1);
}

TEST_CASE("A network change makes the next refresh re-probe and publish the move", "[node][endpoint]")
{
    ResolverRig rig;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    rig.resolver.Refresh();
    REQUIRE(rig.node.Current() == "10.0.0.5:6674");
    REQUIRE(rig.NodeMoves() == 0);

    rig.probe.Answer(DefaultRoute, "192.168.7.2");
    rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
    // The event marks the route stale and nothing more: the probe is the refresh's, never the
    // delivering thread's.
    CHECK(rig.node.Current() == "10.0.0.5:6674");
    rig.resolver.Refresh();

    CHECK(rig.node.Current() == "192.168.7.2:6674");
    CHECK(rig.raft.Current() == "192.168.7.2:6680");
    CHECK(rig.NodeMoves() == 1);
    CHECK(rig.RaftMoves() == 1);
    CHECK(rig.Logged(LogLevel::Info, "endpoint 10.0.0.5:6674 -> 192.168.7.2:6674") == 1);
}

TEST_CASE("Without a network change a refresh inside the interval does not re-probe", "[node][endpoint]")
{
    ResolverRig rig;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    rig.resolver.Refresh();
    REQUIRE(rig.probe.Asked().size() == 1);

    rig.probe.Answer(DefaultRoute, "192.168.7.2");
    rig.clock.advance(EndpointResolver::RefreshInterval - std::chrono::seconds { 1 });
    rig.resolver.Refresh();

    CHECK(rig.probe.Asked().size() == 1);
    CHECK(rig.node.Current() == "10.0.0.5:6674");
}

TEST_CASE("A refresh after the interval re-probes without any event", "[node][endpoint]")
{
    ResolverRig rig;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    rig.resolver.Refresh();

    rig.probe.Answer(DefaultRoute, "192.168.7.2");
    rig.clock.advance(EndpointResolver::RefreshInterval);
    rig.resolver.Refresh();

    CHECK(rig.probe.Asked().size() == 2);
    CHECK(rig.node.Current() == "192.168.7.2:6674");
    CHECK(rig.NodeMoves() == 1);
}

TEST_CASE("A resume re-probes and a suspend does not", "[node][endpoint]")
{
    ResolverRig rig;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    rig.resolver.Refresh();

    rig.resolver.OnHostEvent(HostEvent::Suspending);
    rig.resolver.Refresh();
    CHECK(rig.probe.Asked().size() == 1);

    // A machine that woke may have woken on another network.
    rig.resolver.OnHostEvent(HostEvent::Resumed);
    rig.resolver.Refresh();
    CHECK(rig.probe.Asked().size() == 2);
}

TEST_CASE("With no usable route the resolver keeps the last published endpoint", "[node][endpoint]")
{
    // `AdvertisedNameWithheld` stands down once a route host is engaged, so a loopback one would be
    // advertised unchallenged: every unusable answer is refused, and refused answers keep the LAST
    // route host rather than falling back to the name.
    auto const answers = std::array<std::expected<std::string, RouteProbeError>, 8> {
        std::unexpected { RouteProbeError::NoRoute },
        "127.0.0.1",
        "::1",
        "169.254.3.3",
        "fe80::1",
        "0.0.0.0",
        "224.0.0.251",
        "ff02::1",
    };
    for (auto const& answer: answers)
    {
        INFO((answer.has_value() ? *answer : std::string { "no route" }));
        ResolverRig rig;
        rig.probe.Answer(DefaultRoute, "10.0.0.5");
        rig.resolver.Refresh();

        for (auto const target: DefaultRouteProbeTargets)
            rig.probe.Answer(std::string { target }, answer);
        rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
        rig.resolver.Refresh();

        CHECK(rig.node.Current() == "10.0.0.5:6674");
        CHECK(rig.raft.Current() == "10.0.0.5:6680");
        CHECK(rig.resolver.RouteHost() == "10.0.0.5");
        CHECK(rig.NodeMoves() == 0);
        // Both default targets were asked: an unusable IPv4 answer is not the end of the probe.
        CHECK(rig.probe.Asked().size() == 1 + DefaultRouteProbeTargets.size());
    }
}

TEST_CASE("A route host is refused for each reason in its table, and nothing else is", "[node][endpoint]")
{
    struct Case
    {
        std::string_view host; ///< A probed answer.
        bool usable;           ///< Whether it may become the route host.
    };
    auto const cases = std::array {
        Case { .host = "127.0.0.1", .usable = false },
        Case { .host = "127.4.5.6", .usable = false },
        Case { .host = "::1", .usable = false },
        Case { .host = "::ffff:127.0.0.1", .usable = false },
        Case { .host = "169.254.3.3", .usable = false },
        Case { .host = "fe80::1", .usable = false },
        Case { .host = "febf::1", .usable = false },
        Case { .host = "0.0.0.0", .usable = false },
        Case { .host = "::", .usable = false },
        Case { .host = "", .usable = false },
        Case { .host = "224.0.0.251", .usable = false },
        Case { .host = "239.255.255.250", .usable = false },
        Case { .host = "ff02::1", .usable = false },
        Case { .host = "::ffff:224.0.0.1", .usable = false },
        Case { .host = "10.0.0.5", .usable = true },
        Case { .host = "192.168.7.2", .usable = true },
        Case { .host = "223.255.255.255", .usable = true },
        Case { .host = "240.0.0.1", .usable = true },
        Case { .host = "2001:db8::5", .usable = true },
        Case { .host = "fd00::9", .usable = true },
        Case { .host = "ff::1", .usable = true },
    };
    for (auto const& [host, usable]: cases)
    {
        INFO(host);
        CHECK(IsUsableRouteHost(host) == usable);
    }
}

TEST_CASE("A pinned literal advertise never moves and is reported once", "[node][endpoint]")
{
    ResolverRig rig;
    auto pinned = WildcardNode("10.0.0.5");
    pinned.advertise = "10.9.9.9:6674";
    rig.config.Replace(pinned);
    rig.probe.Answer(DefaultRoute, "192.168.7.2");

    rig.resolver.Refresh();
    rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
    rig.resolver.Refresh();
    rig.resolver.Refresh();

    CHECK(rig.node.Current() == "10.9.9.9:6674");
    CHECK(rig.Logged(LogLevel::Warn,
                     "--advertise 10.9.9.9:6674 is a literal address: this node will not follow "
                     "network changes")
          == 1);
    // `--raft-self` is still `auto`, so the route is still probed for it.
    CHECK(rig.raft.Current() == "192.168.7.2:6680");

    SECTION("pinned on both flags, the route is never probed at all")
    {
        // The mode table's `followsRoute` column decides that, read for both flags.
        ResolverRig both;
        auto bothPinned = WildcardNode("10.0.0.5");
        bothPinned.advertise = "10.9.9.9:6674";
        bothPinned.raftSelf = "10.9.9.9";
        both.config.Replace(bothPinned);
        both.resolver.OnHostEvent(HostEvent::NetworkChanged);
        both.resolver.Refresh();
        CHECK(both.probe.Asked().empty());
        CHECK(both.node.Current() == "10.9.9.9:6674");
        CHECK(both.raft.Current() == "10.9.9.9:6680");
    }
}

TEST_CASE("A preferred target is probed before the default route", "[node][endpoint]")
{
    ResolverRig rig;
    rig.targets.targets = { "10.20.0.1" };
    rig.probe.Answer("10.20.0.1", "10.20.0.7");
    rig.probe.Answer(DefaultRoute, "10.0.0.5");

    rig.resolver.Refresh();

    CHECK(rig.node.Current() == "10.20.0.7:6674");
    CHECK(rig.probe.Asked() == std::vector<std::string> { "10.20.0.1" });

    SECTION("and a preferred target with no usable answer falls through to it")
    {
        rig.probe.Answer("10.20.0.1", "127.0.0.1");
        rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
        rig.resolver.Refresh();
        CHECK(rig.node.Current() == "10.0.0.5:6674");
    }
}

TEST_CASE("A reload that changes nothing keeps the roamed endpoint", "[node][endpoint]")
{
    ResolverRig rig;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    rig.resolver.Refresh();
    rig.probe.Answer(DefaultRoute, "192.168.7.2");
    rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
    rig.resolver.Refresh();
    REQUIRE(rig.node.Current() == "192.168.7.2:6674");

    // A NEW snapshot equal to the one the node started with -- route host and all, as the start
    // shaped it. The roamed route wins over the snapshot's.
    rig.config.Replace(WildcardNode("10.0.0.5"));
    rig.resolver.Refresh();

    CHECK(rig.node.Current() == "192.168.7.2:6674");
    CHECK(rig.raft.Current() == "192.168.7.2:6680");
    CHECK(rig.NodeMoves() == 1);
}

TEST_CASE("What is published is the DERIVED endpoint, never the flag", "[node][endpoint][advertise]")
{
    // Wrong silently in both directions: missed, the fleet keeps leasing an address nobody answers;
    // fired spuriously, every worker re-registers because somebody saved a file.
    ResolverRig rig;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    rig.resolver.Refresh();
    REQUIRE(rig.node.Current() == "10.0.0.5:6674");

    SECTION("a reload spelling out the endpoint already in force moves nothing")
    {
        auto spelled = WildcardNode("10.0.0.5");
        spelled.advertise = "10.0.0.5:6674";
        rig.config.Replace(spelled);
        rig.resolver.Refresh();
        CHECK(rig.node.Current() == "10.0.0.5:6674");
        CHECK(rig.NodeMoves() == 0);
    }

    SECTION("a reload re-pinning it to a name publishes the move")
    {
        auto named = WildcardNode("10.0.0.5");
        named.advertise = "build.lan:6674";
        rig.config.Replace(named);
        rig.resolver.Refresh();
        CHECK(rig.node.Current() == "build.lan:6674");
        CHECK(rig.NodeMoves() == 1);
        // The Raft endpoint still follows the route: only `--advertise` was pinned.
        CHECK(rig.raft.Current() == "10.0.0.5:6680");
        CHECK(rig.RaftMoves() == 0);
    }
}

TEST_CASE("An empty route host leaves the configuration with none", "[node][endpoint][config]")
{
    // One representation for "no route": a probe that found nothing must never leave an engaged
    // empty host behind, which the withheld-name rules would read as a route.
    auto cfg = WildcardNode("10.0.0.5");
    REQUIRE(cfg.routeHost.has_value());
    ApplyRouteHost(cfg, "");
    CHECK_FALSE(cfg.routeHost.has_value());
    CHECK(AdvertisedEndpoint(cfg) == "box.lan:6674");
}

TEST_CASE("The preferred targets are the IP literals among where this node registers", "[node][endpoint]")
{
    FixedSchedulers const schedulers { { "10.0.0.1:6675", "sched.lan:6675", "[fd00::1]:6675", "10.0.0.1:6676" } };
    SchedulerProbeTargets const targets { schedulers };
    CHECK(targets.Targets() == std::vector<std::string> { "10.0.0.1", "fd00::1" });
}

TEST_CASE("A network change re-probes on the resolver's own thread", "[node][endpoint][host-events]")
{
    ResolverRig rig { "" };
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    {
        auto const loop = rig.resolver.Launch();
        REQUIRE(Testing::WaitUntil(
            "the resolver's first refresh",
            [&rig] { return rig.node.Current() == "10.0.0.5:6674"; },
            [&rig] { return rig.node.Current(); }));

        rig.probe.Answer(DefaultRoute, "192.168.7.2");
        rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
        REQUIRE(Testing::WaitUntil(
            "the resolver's thread to publish the move",
            [&rig] { return rig.node.Current() == "192.168.7.2:6674"; },
            [&rig] { return rig.node.Current(); }));
    }
    CHECK(rig.NodeMoves() == 2);
}

TEST_CASE("A body built after a roam adopts the live route without counting a move", "[node][endpoint]")
{
    // A reformed body's configuration still carries the START's route host; the process's cell
    // holds where the node has roamed since. The fleet already knows the roamed address, so building
    // the body moves nothing: no counter, no log line, and no reader ever sees the start's address.
    ResolverRig rig { "10.0.0.5", "192.168.7.2" };
    CHECK(rig.node.Current() == "192.168.7.2:6674");
    CHECK(rig.raft.Current() == "192.168.7.2:6680");

    rig.probe.Answer(DefaultRoute, "192.168.7.2");
    rig.resolver.Refresh();

    CHECK(rig.node.Current() == "192.168.7.2:6674");
    CHECK(rig.NodeMoves() == 0);
    CHECK(rig.RaftMoves() == 0);
    CHECK(rig.Logged(LogLevel::Info, "endpoint") == 0);
}

namespace
{
/// Counts how often it was told the published endpoint moved.
class CountingMoveSink final: public IEndpointMoveSink
{
  public:
    void OnEndpointMoved() noexcept override
    {
        ++told;
    }

    std::atomic<int> told { 0 }; ///< How often.
};
} // namespace

TEST_CASE("A published 0xFC move is told to every watcher, and a Raft-only move is not", "[node][endpoint]")
{
    ResolverRig rig;
    CountingMoveSink sink;
    rig.probe.Answer(DefaultRoute, "10.0.0.5");
    {
        EndpointMoveWatch const watch { rig.resolver, sink };
        rig.resolver.Refresh();
        CHECK(sink.told == 0);

        // `--advertise` pinned to a name: the route moves only the Raft endpoint.
        auto named = WildcardNode("10.0.0.5");
        named.advertise = "build.lan:6674";
        rig.config.Replace(named);
        rig.resolver.Refresh();
        CHECK(sink.told == 1);
        rig.probe.Answer(DefaultRoute, "192.168.7.2");
        rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
        rig.resolver.Refresh();
        CHECK(rig.raft.Current() == "192.168.7.2:6680");
        CHECK(sink.told == 1);
    }
    // Unwatched: told nothing more.
    rig.config.Replace(WildcardNode("10.0.0.5"));
    rig.resolver.Refresh();
    CHECK(rig.node.Current() == "192.168.7.2:6674");
    CHECK(sink.told == 1);
}

TEST_CASE("A node that dials in or runs no consensus moves no Raft endpoint and counts none", "[node][endpoint]")
{
    // A learner's mode closes the Raft port, and a node with no formation runs no consensus: nobody
    // dials either, so a roam re-derives no Raft endpoint for them and the counter stays where it was.
    auto const shapes = std::vector<std::pair<char const*, std::function<void(NodeConfig&)>>> {
        { "learner",
          [](NodeConfig& cfg) {
              auto record = Testing::Unwrap(cfg.formation);
              record.mode = Cluster::NodeMode::Learner;
              cfg.formation = std::move(record);
          } },
        { "no consensus", [](NodeConfig& cfg) { cfg.formation.reset(); } },
    };
    for (auto const& [name, shape]: shapes)
    {
        INFO(name);
        ResolverRig rig;
        auto shaped = WildcardNode("10.0.0.5");
        shape(shaped);
        REQUIRE(RaftSelfEndpoint(shaped).empty());
        rig.config.Replace(shaped);
        rig.probe.Answer(DefaultRoute, "192.168.7.2");
        rig.resolver.OnHostEvent(HostEvent::NetworkChanged);
        rig.resolver.Refresh();
        CHECK(rig.node.Current() == "192.168.7.2:6674");
        CHECK(rig.raft.Current() == "10.0.0.5:6680"); // what it started with, never re-derived
        CHECK(rig.RaftMoves() == 0);
    }
}

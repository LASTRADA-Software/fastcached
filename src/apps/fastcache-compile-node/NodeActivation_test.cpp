// SPDX-License-Identifier: Apache-2.0
#include "NodeActivation.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeSurfaces.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// The port the packaged `.socket` unit opens, which is not `--listen-node`'s default: a node that
/// advertised the default would send every client to a port nothing answers.
constexpr std::uint16_t ActivatedPort = 6676;

/// A configuration this machine routes from @p routeHost with, as the start shapes it.
/// @param routeHost The probed route host.
/// @return The configuration, nothing typed.
[[nodiscard]] NodeConfig RoutedFrom(std::string routeHost)
{
    NodeConfig cfg;
    ApplyRouteHost(cfg, std::move(routeHost));
    return cfg;
}

} // namespace

TEST_CASE("An activated wildcard socket advertises the routed address on its port", "[node][activation]")
{
    auto cfg = RoutedFrom("10.0.0.9");
    REQUIRE(AdoptActivatedBind(cfg, BoundEndpoint { .host = "0.0.0.0", .port = ActivatedPort }).has_value());

    CHECK(AdvertisedEndpoint(cfg) == "10.0.0.9:6676");
    CHECK(AdvertiseModeOf(cfg.advertise) == AdvertiseMode::Auto);

    // The surface row is what every other reader of the bind asks, so it reads the socket too.
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    REQUIRE_FALSE(node.empty());
    CHECK(node.front().port == ActivatedPort);
}

TEST_CASE("An activated IPv6 wildcard socket advertises the routed address on its port", "[node][activation]")
{
    auto cfg = RoutedFrom("10.0.0.9");
    REQUIRE(AdoptActivatedBind(cfg, BoundEndpoint { .host = "::", .port = ActivatedPort }).has_value());

    CHECK(cfg.nodeListen == "[::]:6676");
    CHECK(AdvertisedEndpoint(cfg) == "10.0.0.9:6676");
}

TEST_CASE("An activated socket bound to one address advertises that address", "[node][activation]")
{
    auto cfg = RoutedFrom("10.0.0.9");
    REQUIRE(AdoptActivatedBind(cfg, BoundEndpoint { .host = "192.168.1.4", .port = ActivatedPort }).has_value());

    auto const advertised = AdvertisedEndpoint(cfg);
    CHECK(advertised == "192.168.1.4:6676");
    CHECK(AdvertiseModeOf(advertised) == AdvertiseMode::PinnedLiteral);
}

TEST_CASE("An explicit advertise still wins under activation", "[node][activation]")
{
    auto cfg = RoutedFrom("10.0.0.9");
    cfg.advertise = "build.example:7000";
    REQUIRE(AdoptActivatedBind(cfg, BoundEndpoint { .host = "0.0.0.0", .port = ActivatedPort }).has_value());

    CHECK(AdvertisedEndpoint(cfg) == "build.example:7000");
}

TEST_CASE("An activated bind is adopted as a value and never as a typed flag", "[node][activation]")
{
    // The rules reading `nodeListenExplicit` judge what the operator promised; a unit's
    // `ListenStream=` is not a promise this command line made.
    auto cfg = RoutedFrom("10.0.0.9");
    REQUIRE(AdoptActivatedBind(cfg, BoundEndpoint { .host = "0.0.0.0", .port = ActivatedPort }).has_value());

    CHECK(cfg.nodeListen == "0.0.0.0:6676");
    CHECK_FALSE(cfg.nodeListenExplicit);
}

TEST_CASE("A socket that will not say where it listens is refused and nothing is adopted", "[node][activation]")
{
    auto cfg = RoutedFrom("10.0.0.9");
    auto const before = cfg.nodeListen;

    auto const adopted = AdoptActivatedBind(cfg, std::optional<BoundEndpoint> {});
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error().cause == NodeRefusalCause::HandedOverListeners);
    CHECK(adopted.error().reason == UnreadableActivatedBindRefusal().reason);
    CHECK(adopted.error().reason.starts_with("--advertise is required under socket activation"));
    CHECK(cfg.nodeListen == before);
}

TEST_CASE("A socket bound to no dialable address is refused and nothing is adopted", "[node][activation]")
{
    auto cfg = RoutedFrom("10.0.0.9");
    auto const before = cfg.nodeListen;

    for (auto const& bound: { BoundEndpoint { .host = "0.0.0.0", .port = 0 }, BoundEndpoint { .host = "", .port = 6676 } })
    {
        auto const adopted = AdoptActivatedBind(cfg, std::optional { bound });
        REQUIRE_FALSE(adopted.has_value());
        CHECK(adopted.error().cause == NodeRefusalCause::HandedOverListeners);
        CHECK(cfg.nodeListen == before);
    }
}

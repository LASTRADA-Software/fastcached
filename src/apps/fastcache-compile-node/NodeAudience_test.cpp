// SPDX-License-Identifier: Apache-2.0
//
// Which endpoints a machine ticket may name to be spent at this node. This is the check that stops a
// ticket captured on its way to node A being spent at node B, and until it had a file of its own a
// `NodeAudience` accepting every audience left all 790 node cases green.
#include "NodeAnnounce.hpp"
#include "NodeAudience.hpp"
#include "NodeConfig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <tests/LocalityFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// This node as the cases below describe it: advertised as `office.corp:6674`, named `office-pc`,
/// its `Node` surface on 6674 and 6680, and `10.0.0.5` an address of this machine.
struct Office
{
    AnnouncedEndpoint announced { "office.corp:6674" };
    Testing::ThisMachineIs locality { "10.0.0.5" };
    NodeAudience audience { announced, "office-pc", { 6674, 6680 }, locality };
};

/// One audience a ticket might name, and whether this node is it.
struct AudienceRow
{
    std::string_view audience; ///< What the ticket names.
    bool matches;              ///< Whether this node answers to it.
    std::string_view why;      ///< Which rule decides.
};

constexpr auto AudienceRows = std::to_array<AudienceRow>({
    { .audience = "office.corp:6674", .matches = true, .why = "the advertised endpoint" },
    { .audience = "OFFICE.corp:6674", .matches = true, .why = "a name compares as DNS does, without case" },
    { .audience = "office-pc:6680", .matches = true, .why = "this machine's host name, at a Node port" },
    { .audience = "10.0.0.5:6674", .matches = true, .why = "an address of this machine, at a Node port" },
    { .audience = "other.corp:6674", .matches = false, .why = "another machine: a ticket for node B" },
    { .audience = "10.0.0.9:6674", .matches = false, .why = "an address of another machine" },
    { .audience = "office.corp:7000", .matches = false, .why = "the right machine at a port this node does not serve" },
    { .audience = "office-pc:9999", .matches = false, .why = "the host name at a port this node does not serve" },
    { .audience = "127.0.0.1:6674", .matches = false, .why = "loopback names every node" },
    { .audience = "localhost:6674", .matches = false, .why = "the loopback name names every node" },
    { .audience = "0.0.0.0:6674", .matches = false, .why = "a wildcard names every node" },
    { .audience = "office.corp", .matches = false, .why = "no port: not a dial endpoint" },
    { .audience = "", .matches = false, .why = "nothing at all" },
});

} // namespace

TEST_CASE("A ticket names this node only by an endpoint this node answers to", "[node][ticket][audience]")
{
    Office node;
    for (auto const& row: AudienceRows)
    {
        INFO(row.audience << ": " << row.why);
        CHECK(node.audience.Matches(row.audience) == row.matches);
    }
}

TEST_CASE("A node advertised on loopback or a wildcard is not spendable there by that name", "[node][ticket][audience]")
{
    // The advertised endpoint joins the names, but the rule that refuses a name every node answers
    // to is `AudienceMatches`' and is not widened by what this node calls itself.
    for (auto const* advertised: { "127.0.0.1:6674", "0.0.0.0:6674", "localhost:6674" })
    {
        INFO(advertised);
        AnnouncedEndpoint const announced { advertised };
        Testing::ThisMachineIs const locality { "127.0.0.1" };
        NodeAudience const audience { announced, {}, { 6674 }, locality };
        CHECK_FALSE(audience.Matches(advertised));
    }
}

TEST_CASE("A ticket for the endpoint a node moved away from stops matching the moment it moves", "[node][ticket][audience]")
{
    // `--advertise` is reloadable, and the audience reads it per call: a ticket minted for the new
    // endpoint must be spendable the moment the registration names it, and one for the old endpoint
    // must not be, since another node may be answering there now.
    Office node;
    REQUIRE(node.audience.Matches("office.corp:6674"));
    REQUIRE_FALSE(node.audience.Matches("build.corp:7100"));

    node.announced.Publish("build.corp:7100");
    CHECK(node.audience.Matches("build.corp:7100"));
    CHECK_FALSE(node.audience.Matches("office.corp:6674"));
    // The configured names and ports are untouched by the move.
    CHECK(node.audience.Matches("office-pc:6680"));
}

TEST_CASE("The ports a ticket may name are the ones the Node surface binds", "[node][ticket][audience]")
{
    // The row `--print-surfaces` prints and the listener binds, so the audience cannot name a port
    // the listener does not serve, nor miss one it does.
    NodeConfig defaults;
    CHECK(NodeAudience::PortsOf(defaults) == std::vector<std::uint16_t> { 6674 });

    NodeConfig moved;
    moved.nodeListen = "0.0.0.0:6680";
    CHECK(NodeAudience::PortsOf(moved) == std::vector<std::uint16_t> { 6680 });

    // And a port this node does not bind is not one a ticket may name here: a node built from the
    // moved configuration refuses a ticket for the default port.
    AnnouncedEndpoint const announced { "office.corp:6680" };
    Testing::ThisMachineIs const locality { "10.0.0.5" };
    NodeAudience const audience { announced, "office-pc", NodeAudience::PortsOf(moved), locality };
    CHECK(audience.Matches("office-pc:6680"));
    CHECK_FALSE(audience.Matches("office-pc:6674"));
}

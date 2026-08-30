// SPDX-License-Identifier: Apache-2.0
#include "NodeSurfaces.hpp"

#include <FastCache/Cli/Options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Node;

TEST_CASE("Every surface names flags the parser accepts", "[node][surfaces]")
{
    // The half the type system cannot see. `RowsInEnumeratorOrder` proves a row
    // EXISTS for every surface and sits at its own index; nothing in the type system
    // can tell whether the spelling inside it is one this binary would accept. A row
    // naming `--listen-metrics` compiles perfectly and produces a firewall worksheet
    // citing a flag the parser refuses.
    //
    // Walked off `NodeOptions()` rather than a list written out here, for the reason
    // every table in this tree is: a hand-written list is updated by the same person
    // who forgot the row.
    for (auto const& row: NodeSurfaceTable())
    {
        for (auto const& flag: FlagsOf(row))
        {
            INFO("surface: " << row.name << ", flag: " << flag);
            CHECK(std::ranges::any_of(
                NodeOptions(), [flag](auto const& option) { return option.primary == flag || option.alias == flag; }));
        }
    }
}

TEST_CASE("Every surface's spec reaches a real field", "[node][surfaces]")
{
    // The other thing a stale row gets wrong: a member pointer that compiles but
    // points at a field the flag no longer fills. Asserted by driving the flag
    // through the parser and reading the row's own pointer back -- so the row is
    // checked against the parser rather than against a second copy of the mapping,
    // which would be the fifth place again in test form.
    for (auto const& row: NodeSurfaceTable())
    {
        if (row.spec == nullptr)
            continue;

        INFO("surface: " << row.name);
        auto const flag = FlagsOf(row).front();
        // A value every listen grammar accepts and no default equals, so a row whose
        // pointer lands on the wrong field reads back something else.
        auto const argument = std::string { flag } + "=10.11.12.13:6699";
        std::vector<char const*> const argv { argument.c_str() };

        NodeConfig cfg;
        auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
        REQUIRE(flow.has_value());
        CHECK(cfg.*row.spec == "10.11.12.13:6699");
    }
}

TEST_CASE("A surface's grammar accepts what its shape advertises", "[node][surfaces]")
{
    // A shape string an operator is shown in a refusal, and a predicate that decides
    // it -- two statements of one rule, so they are checked against each other. The
    // bare-port case is the one that differs between the two grammars and the one a
    // reader is most likely to get wrong, since it is exactly what separates a bound
    // surface from a beacon.
    for (auto const& row: NodeSurfaceTable())
    {
        if (row.parses == nullptr)
            continue;

        INFO("surface: " << row.name << ", shape: " << row.shape);
        CHECK(row.parses("10.11.12.13:6699"));
        CHECK_FALSE(row.parses("not-an-endpoint"));

        // `[<address>:]<port>` promises a bare port is accepted; `<address>:<port>`
        // promises it is not. The shape is what an operator reads, so it is the
        // thing the predicate has to agree with.
        auto const bracketed = row.shape.starts_with("[");
        CHECK(row.parses("6699") == bracketed);
    }
}

TEST_CASE("A default host is present exactly when a bare port could take one", "[node][surfaces]")
{
    // Being off until named and taking loopback for a bare port are independent
    // facts, and this keeps them so. Three surfaces are BOTH -- served only once an
    // operator names an address, and on loopback or the wildcard the moment that
    // address is a bare port -- so a reader who collapses them loses the second,
    // which for the admin row is the input to its credential rule rather than a
    // firewall detail. The first half is `resolve`'s to answer; only the second is
    // a column, which is why there is no `presence` beside `hostOrigin`.
    for (auto const& row: NodeSurfaceTable())
    {
        INFO("surface: " << row.name);
        switch (row.hostOrigin)
        {
            case HostOrigin::DefaultConstant:
            case HostOrigin::Fixed:
                CHECK_FALSE(row.defaultHost.empty());
                break;
            case HostOrigin::OperatorFlag:
                // Its host is a flag, so there is no fallback to record and an empty
                // column is the honest answer rather than a missing one.
                CHECK(row.defaultHost.empty());
                break;
        }
    }
}

TEST_CASE("A default configuration serves the two surfaces that are on", "[node][surfaces]")
{
    // Asked of the resolver rather than of a column restating it. A `presence`
    // column was written and deleted: it said what `resolve(NodeConfig{})` already
    // says, and it said it *wrongly* for raft, whose address an operator can name
    // and still get no port. The named surfaces are spelled out here so the case
    // fails if a row's default changes, rather than comparing the resolver against
    // a second copy of its own answer.
    NodeConfig const cfg;

    std::vector<std::string_view> served;
    for (auto const& row: NodeSurfaceTable())
        if (!row.resolve(cfg).empty())
            served.push_back(row.name);

    CHECK(served == std::vector<std::string_view> { "compile", "cache" });

    auto const compile = RowFor(NodeSurface::Compile).resolve(cfg);
    REQUIRE(compile.size() == 1);
    CHECK(compile.front().host == "0.0.0.0");
    CHECK(compile.front().port == 6676);

    // The default an operator reads off the startup line, and the address
    // `fastcache-cc` looks for when nobody sets `FASTCACHE_ADDR`.
    auto const cache = RowFor(NodeSurface::Cache).resolve(cfg);
    REQUIRE(cache.size() == 1);
    CHECK(cache.front().host == "127.0.0.1");
    CHECK(cache.front().port == 6674);
}

TEST_CASE("A bare port takes its own surface's default host", "[node][surfaces]")
{
    // The asymmetry, asserted rather than described. It is the anti-leeching rule:
    // a scheduler no peer can dial does nothing, while a cache any host can dial is
    // this machine's entire build output served to strangers. A worksheet that got
    // this backwards would tell an operator a surface is loopback-only when it is
    // open to the network.
    NodeConfig cfg;
    cfg.cacheListen = "6699";
    cfg.schedulerListen = "6699";
    cfg.adminListen = "6699";
    cfg.raftListen = "6699";
    cfg.nodeId = "n1";

    auto const hostOf = [&cfg](NodeSurface surface) {
        auto const endpoints = RowFor(surface).resolve(cfg);
        REQUIRE(endpoints.size() == 1);
        return endpoints.front().host;
    };

    CHECK(hostOf(NodeSurface::Cache) == "127.0.0.1");
    CHECK(hostOf(NodeSurface::Admin) == "127.0.0.1");
    CHECK(hostOf(NodeSurface::Scheduler) == "0.0.0.0");
    CHECK(hostOf(NodeSurface::Raft) == "0.0.0.0");
}

TEST_CASE("Discovery binds the wildcard whatever address it announces to", "[node][surfaces]")
{
    // The row that is neither one endpoint nor one flag, and the one an operator is
    // least likely to get right unaided.
    NodeConfig cfg;
    cfg.discoveryAddress = "255.255.255.255:6681";

    auto const beaconOnly = RowFor(NodeSurface::Discovery).resolve(cfg);
    REQUIRE(beaconOnly.size() == 1);
    // NOT 255.255.255.255. The address is where beacons are sent; the socket binds
    // the wildcard unconditionally, and reading the announce address as a bind
    // address would put a broadcast address on a firewall worksheet.
    CHECK(beaconOnly.front().host == "0.0.0.0");
    CHECK(beaconOnly.front().port == 6681);
    CHECK(beaconOnly.front().role == "beacon");

    // The second endpoint, which is why the row resolves to a list. A node answers
    // challenges on a port only it holds, so an operator who opened the beacon port
    // and not this one hears every beacon and completes no handshake.
    cfg.discoveryReplyPort = 6682;
    auto const both = RowFor(NodeSurface::Discovery).resolve(cfg);
    REQUIRE(both.size() == 2);
    CHECK(both[1].port == 6682);
    CHECK(both[1].role == "reply");
}

TEST_CASE("Discovery is the only surface that is not TCP", "[node][surfaces]")
{
    // Stated as a test because it is the fact a worksheet is wrong without: six rows
    // and five correct firewall rules leaves a beacon that reaches nobody, which
    // presents as a fleet that never forms rather than as a firewall mistake.
    for (auto const& row: NodeSurfaceTable())
    {
        INFO("surface: " << row.name);
        CHECK((row.protocol == SurfaceProtocol::Udp) == (row.surface == NodeSurface::Discovery));
    }
}

TEST_CASE("Raft binds nothing until consensus is turned on", "[node][surfaces]")
{
    // `--listen-raft` is configuration, `--node-id` is the switch. A worksheet
    // listing the raft port for a node running no consensus would have an operator
    // open a port nothing will ever bind.
    NodeConfig cfg;
    cfg.raftListen = "0.0.0.0:6680";

    CHECK(RowFor(NodeSurface::Raft).resolve(cfg).empty());

    cfg.nodeId = "n1";
    CHECK(RowFor(NodeSurface::Raft).resolve(cfg).size() == 1);
}

TEST_CASE("The worksheet describes this configuration, not the defaults", "[node][surfaces]")
{
    // The failure this flag exists to prevent, asserted at the parse. `--help` and
    // `--version` stop parsing because they ignore the rest of the command line;
    // `--print-surfaces` REPORTS on it, so stopping made
    // `--print-surfaces --listen-scheduler=6675` print a node that was never
    // configured -- a worksheet silently describing a different machine from the one
    // the operator asked about.
    std::vector<char const*> const argv { "--print-surfaces", "--listen-scheduler=6675" };

    NodeConfig cfg;
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
    REQUIRE(flow.has_value());
    CHECK(cfg.printSurfaces);
    CHECK(cfg.schedulerListen == "6675");

    CHECK(RenderSurfaces(cfg).contains("0.0.0.0:6675"));
}

TEST_CASE("The worksheet never prints an announce address as a bind address", "[node][surfaces]")
{
    // The sharpest failure this table can produce. `--discovery=255.255.255.255:6681`
    // is an ordinary thing to write, and reading its host as the bind address would
    // put a BROADCAST address on a firewall worksheet -- in the row an operator is
    // least likely to question, because it is also the only UDP one.
    NodeConfig cfg;
    cfg.discoveryAddress = "255.255.255.255:6681";
    cfg.discoveryReplyPort = 6682;

    auto const sheet = RenderSurfaces(cfg);
    CHECK_FALSE(sheet.contains("255.255.255.255"));
    CHECK(sheet.contains("0.0.0.0:6681"));
    // Both sockets, because opening the beacon port and not the reply port gives a
    // node that hears every beacon and completes no handshake.
    CHECK(sheet.contains("0.0.0.0:6682"));
    CHECK(sheet.contains("UDP"));
}

TEST_CASE("A surface that is off is named, with what would turn it on", "[node][surfaces]")
{
    // Omitting it would read as a surface this build does not have, which an operator
    // cannot tell from one they simply did not switch on.
    auto const sheet = RenderSurfaces(NodeConfig {});
    CHECK(sheet.contains("--listen-scheduler"));
    CHECK(sheet.contains("--admin-listen"));

    // And the compile port's caveat travels with the sheet rather than living only in
    // the documentation: its flags describe nothing under socket activation, which is
    // the one way this list can be wrong about a port it does print.
    CHECK(sheet.contains(".socket"));
}

TEST_CASE("The compile port's note says what its flags stop describing", "[node][surfaces]")
{
    // Not a spelling check on prose -- the assertion is that the row carries the
    // caveat at all. `--bind` and `--port` are read by nothing under socket
    // activation, so a worksheet printing them without saying so sends an operator
    // to open a port the node is not on, and the surface they actually need is one
    // nothing printed. The general rule is in distributed-compilation.md; this is
    // the third consumer of the same flag to meet it.
    CHECK(RowFor(NodeSurface::Compile).note.contains("--advertise"));
    CHECK(RowFor(NodeSurface::Compile).note.contains(".socket"));
}

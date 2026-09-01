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
        if (row.grammar.parses == nullptr)
            continue;

        INFO("surface: " << row.name << ", shape: " << row.grammar.shape);
        CHECK(row.grammar.parses("10.11.12.13:6699"));
        CHECK_FALSE(row.grammar.parses("not-an-endpoint"));

        // `[<address>:]<port>` promises a bare port is accepted; `<address>:<port>`
        // promises it is not. The shape is what an operator reads, so it is the
        // thing the predicate has to agree with.
        auto const bracketed = row.grammar.shape.starts_with("[");
        CHECK(row.grammar.parses("6699") == bracketed);
    }
}

TEST_CASE("A surface resolving from its own spec has a default host to resolve against", "[node][surfaces]")
{
    // What survives of a deleted column. There WAS a `HostOrigin` enum naming which of
    // three mechanisms supplied each row's host; nothing in production read it, its
    // only test asserted it agreed with the column it was derived from, and its own
    // doc had gone stale against its own table. That is the third column here to fail
    // the same test, after `presence` and the `explicitBit` never added.
    //
    // The fact worth keeping is narrower and is a build failure rather than a case: a
    // row that resolves from its spec needs a spec and a default host, which
    // `NodeSurfaces.cpp` `static_assert`s. What is left for runtime is that the two
    // rows carrying neither are exactly the two whose resolution is their own code.
    for (auto const& row: NodeSurfaceTable())
    {
        INFO("surface: " << row.name);
        if (row.defaultHost.empty())
            // Two rows, and for opposite reasons. The compile port's host is `--bind`
            // -- a flag an operator sets, never a fallback a bare port takes -- so an
            // empty column is the honest answer. The node port HAS a default and it
            // depends on the configuration (loopback on a worker, the wildcard on a
            // scheduler), which one constant cannot hold: `NodeListenDefaultHost`
            // decides it, and a value here would be a second author of that rule.
            CHECK(row.surface == NodeSurface::Node);
    }

    CHECK(RowFor(NodeSurface::Node).defaultHost.empty());
    CHECK_FALSE(RowFor(NodeSurface::Discovery).defaultHost.empty());
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
        if (!row.Resolve(cfg).empty())
            served.push_back(row.name);

    // ONE protocol surface on a default configuration, where there were two. The
    // dedicated compile port is gone and its verbs arrive here (#290 stage 3).
    CHECK(served == std::vector<std::string_view> { "node" });

    // The default an operator reads off the startup line, and the address
    // `fastcache-cc` looks for when nobody sets `FASTCACHE_ADDR`.
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    REQUIRE(node.size() == 1);
    CHECK(node.front().host == "127.0.0.1");
    CHECK(node.front().port == 6674);
}

TEST_CASE("A bare port takes its own surface's default host", "[node][surfaces]")
{
    // The asymmetry, asserted rather than described. It is the anti-leeching rule:
    // a scheduler no peer can dial does nothing, while a cache any host can dial is
    // this machine's entire build output served to strangers. A worksheet that got
    // this backwards would tell an operator a surface is loopback-only when it is
    // open to the network.
    NodeConfig cfg;
    cfg.nodeListen = "6699";
    cfg.adminListen = "6699";
    cfg.raftListen = "6699";
    cfg.nodeId = "n1";

    auto const hostOf = [&cfg](NodeSurface surface) {
        auto const endpoints = RowFor(surface).Resolve(cfg);
        REQUIRE(endpoints.size() == 1);
        return endpoints.front().host;
    };

    CHECK(hostOf(NodeSurface::Node) == "127.0.0.1");
    CHECK(hostOf(NodeSurface::Admin) == "127.0.0.1");
    CHECK(hostOf(NodeSurface::Raft) == "0.0.0.0");
}

TEST_CASE("The node port's default host follows whether this node schedules", "[node][surfaces]")
{
    // The asymmetry SURVIVED the merge rather than being resolved by it (#290), and
    // this is where. Two surfaces pulled one address in opposite directions -- a
    // scheduler no peer can dial does nothing, a cache every host can dial is this
    // machine's whole build output served to strangers -- so the one listener keeps
    // both answers and picks between them on `--serve-scheduler`.
    //
    // A worksheet that got this backwards would tell an operator a port is
    // loopback-only when it faces the network, which is a security misstatement
    // rather than an untidy one.
    NodeConfig worker;
    worker.nodeListen = "6699";
    auto const workerEndpoints = RowFor(NodeSurface::Node).Resolve(worker);
    REQUIRE(workerEndpoints.size() == 1);
    CHECK(workerEndpoints.front().host == "127.0.0.1");

    auto scheduling = worker;
    scheduling.serveScheduler = true;
    auto const schedulingEndpoints = RowFor(NodeSurface::Node).Resolve(scheduling);
    REQUIRE(schedulingEndpoints.size() == 1);
    CHECK(schedulingEndpoints.front().host == "0.0.0.0");

    // A host the operator TYPED wins over both, which is what makes the default a
    // default rather than a policy.
    auto named = scheduling;
    named.nodeListen = "127.0.0.1:6699";
    auto const namedEndpoints = RowFor(NodeSurface::Node).Resolve(named);
    REQUIRE(namedEndpoints.size() == 1);
    CHECK(namedEndpoints.front().host == "127.0.0.1");
}

TEST_CASE("Every node that runs binds the 0xFC port", "[node][surfaces]")
{
    // **The inverse of what this case asserted before #290 stage 3**, and the change is
    // deliberate rather than a test relaxed to pass. It used to read "a node that
    // neither caches nor schedules binds no 0xFC port", which was true exactly while a
    // worker had a compile port of its own. Stage 3 retires that port, so a dispatched
    // compile arrives here and a node holding neither component still has to open it --
    // it is the only port it has left.
    //
    // What that older case protected is still protected, one layer down: a node with no
    // cache tier builds no tier, and its FETCH verbs are refused by the component that
    // owns them rather than by the socket being absent.
    NodeConfig worker;
    worker.cacheMemoryBytes = 0;
    worker.cacheDir.clear();
    REQUIRE_FALSE(worker.serveScheduler);
    CHECK(RowFor(NodeSurface::Node).Resolve(worker).size() == 1);

    // And neither component changes the answer any more, which is the whole point:
    // the port is the node's, not the tier's.
    auto caching = worker;
    caching.cacheMemoryBytes = 64ULL * 1024ULL * 1024ULL;
    CHECK(RowFor(NodeSurface::Node).Resolve(caching).size() == 1);

    auto scheduling = worker;
    scheduling.serveScheduler = true;
    CHECK(RowFor(NodeSurface::Node).Resolve(scheduling).size() == 1);

    // The one remaining way to serve no 0xFC port: no address to bind. That is an
    // operator naming nothing, not a component being absent.
    auto unnamed = worker;
    unnamed.nodeListen.clear();
    CHECK(RowFor(NodeSurface::Node).Resolve(unnamed).empty());
}

TEST_CASE("Discovery binds the wildcard whatever address it announces to", "[node][surfaces]")
{
    // The row that is neither one endpoint nor one flag, and the one an operator is
    // least likely to get right unaided.
    NodeConfig cfg;
    cfg.discoveryAddress = "255.255.255.255:6681";

    auto const beaconOnly = RowFor(NodeSurface::Discovery).Resolve(cfg);
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
    auto const both = RowFor(NodeSurface::Discovery).Resolve(cfg);
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

    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty());

    cfg.nodeId = "n1";
    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).size() == 1);
}

TEST_CASE("The worksheet describes this configuration, not the defaults", "[node][surfaces]")
{
    // The failure this flag exists to prevent, asserted at the parse. `--help` and
    // `--version` stop parsing because they ignore the rest of the command line;
    // `--print-surfaces` REPORTS on it, so stopping made
    // `--print-surfaces --listen-node=6675` print a node that was never configured
    // -- a worksheet silently describing a different machine from the one the operator
    // asked about.
    std::vector<char const*> const argv { "--print-surfaces", "--serve-scheduler", "--listen-node=6675" };

    NodeConfig cfg;
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
    REQUIRE(flow.has_value());
    CHECK(cfg.printSurfaces);
    CHECK(cfg.nodeListen == "6675");

    // The wildcard rather than loopback, which is the second flag doing its other job:
    // it decides where a bare port lands as well as whether the verbs are served.
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
    CHECK(sheet.contains("--admin-listen"));

    // And the compile port's caveat travels with the sheet rather than living only in
    // the documentation: its flags describe nothing under socket activation, which is
    // the one way this list can be wrong about a port it does print.
    CHECK(sheet.contains(".socket"));

    // The PRIMARY flag alone, never every flag the row carries. Discovery is the row
    // where that matters: `--discovery-reply-port` is optional, and printing it beside
    // `--discovery` reads as two flags an operator must set to hear a beacon at all.
    CHECK(sheet.contains("not served; set --discovery\n"));
}

TEST_CASE("A surface that is configured and still off is not answered 'set the flag'", "[node][surfaces]")
{
    // The two ways a row goes unserved that are NOT "you did not ask for it", and the
    // two an operator actually meets -- because `--print-surfaces` runs before
    // `StartupPolicyRejection`, deliberately, so the map is available exactly while a
    // port is still wrong. Told to "set --listen-raft", somebody who had just written
    // it goes looking at the flag rather than at `--node-id`.
    NodeConfig raft;
    raft.raftListen = "6680";

    auto const waiting = RenderSurfaces(raft);
    CHECK_FALSE(waiting.contains("set --listen-raft"));
    CHECK(waiting.contains("see the raft note below"));
    // And the note it points at is the one that names the switch.
    CHECK(RowFor(NodeSurface::Raft).note.contains("--node-id"));

    // A malformed value is echoed in the shape the row advertises -- the same sentence
    // `StartupPolicyRejection` produces from the same columns a moment later, rather
    // than an instruction to set a flag that is already set.
    NodeConfig typo;
    typo.nodeListen = "not-a-port";

    auto const wrong = RenderSurfaces(typo);
    CHECK(wrong.contains("--listen-node=not-a-port is not [<address>:]<port>"));
    CHECK_FALSE(wrong.contains("set --listen-node"));
}

TEST_CASE("The 0xFC row's note says what socket activation does to it", "[node][surfaces]")
{
    // Not a spelling check on prose -- the assertion is that the row carries the
    // caveat at all. It moved here with the verbs: under socket activation the unit
    // owns the address, so a worksheet printing this row's flag without saying so
    // sends an operator to open a port the node is not on, and the surface they
    // actually need is one nothing printed. The general rule is in
    // distributed-compilation.md.
    //
    // **This case was built as a forcing function and did not force**, which is
    // worth recording where the next person will meet it. While activation was
    // unwired the note said the surface did NOT serve an inherited descriptor, and
    // the comment here claimed both halves were asserted -- but the two `contains`
    // below match `--advertise` and `.socket`, which the note carries in either
    // state. The note could have flipped from "not served" to "served", or back,
    // without a single assertion moving. A comment describing a guarantee the
    // assertions do not make is the same defect as a refusal test that only asks
    // `has_value()`, and it is the third time in this ticket that the obvious
    // assertion turned out to be satisfied by the wrong thing.
    //
    // So the state is asserted, not merely the vocabulary. #464 gave the reactor
    // listeners `Adopt`, this row's surface serves an activated descriptor now, and
    // reverting either without saying so here fails.
    auto const& note = RowFor(NodeSurface::Node).note;

    // The vocabulary, which is what a worksheet needs: an operator reading this row
    // must be told the unit owns the address and that --advertise is what clients
    // are given.
    CHECK(note.contains("--advertise"));
    CHECK(note.contains(".socket"));

    // And the STATE, which is what the vocabulary cannot carry.
    CHECK(note.contains("is served on this surface"));
    CHECK_FALSE(note.contains("NOT yet served"));
}

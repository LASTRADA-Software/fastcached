// SPDX-License-Identifier: Apache-2.0
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/NodeFormationFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{

/// The documented worker command line, with an admin surface asked to serve TLS two ways.
///
/// **A configuration with exactly one thing wrong**, which is what #582's cases need: a
/// fixture broken two ways would assert nothing about which rule answered. It used to be the
/// documented scheduler line minus the cluster's pre-shared key, which #178 retired; then a
/// worker admitting everybody AND a list, which went with `--fleet-member` when admission came
/// to follow the machine. `--tls-self-signed` beside `--tls-cert` is a `StartupPolicyRejection`
/// row that opens no port of its own, so the worksheet cannot differ between it and its repair
/// (`WithoutTheContradiction`). Every other field is present.
/// @return The config.
[[nodiscard]] NodeConfig WorkerServingTlsTwoWays()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.advertise = "worker-01.internal:6674";
    cfg.advertiseExplicit = true;
    cfg.toolchains = { "/usr/bin/g++" };
    cfg.clusterDir = "cluster";
    cfg.adminListen = "127.0.0.1:9100";
    cfg.tlsSelfSigned = true;
    cfg.tlsCertFile = "admin.pem";
    cfg.tlsKeyFile = "admin.key";
    return cfg;
}

/// @param cfg A `WorkerServingTlsTwoWays` configuration.
/// @return It with the named certificate dropped, so the generated one is the only one asked for.
[[nodiscard]] NodeConfig WithoutTheContradiction(NodeConfig cfg)
{
    cfg.tlsCertFile.clear();
    cfg.tlsKeyFile.clear();
    return cfg;
}

} // namespace

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

        auto cfg = Testing::FirstStart(NodeConfig {});
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
        // Every row has one now. The node port's used to depend on the configuration --
        // loopback on a worker, the wildcard on a scheduler -- and was left empty for a
        // function to decide; since every node is a fleet participant it is the wildcard.
        CHECK_FALSE(row.defaultHost.empty());
    }

    CHECK(RowFor(NodeSurface::Node).defaultHost == NodeSurfaceDefaultHost);
    CHECK_FALSE(RowFor(NodeSurface::Discovery).defaultHost.empty());
}

TEST_CASE("A node with no flags listens on the wildcard node port and the raft port and runs discovery",
          "[node][surfaces][formation][defaults]")
{
    // Asked of the resolver rather than of a column restating it. A `presence`
    // column was written and deleted: it said what `resolve(NodeConfig{})` already
    // says, and it said it *wrongly* for raft, whose address an operator can name
    // and still get no port. The named surfaces are spelled out here so the case
    // fails if a row's default changes, rather than comparing the resolver against
    // a second copy of its own answer.
    auto const cfg = Testing::FirstStart(NodeConfig {});

    std::vector<std::string_view> served;
    for (auto const& row: NodeSurfaceTable())
        if (!row.Resolve(cfg).empty())
            served.push_back(row.name);

    // The node port, consensus and discovery: a node with no flags is a one-voter cluster
    // of itself that beacons on its segment. The dedicated compile port is gone and its
    // verbs arrive on the node port (#290 stage 3).
    CHECK(served == std::vector<std::string_view> { "node", "raft", "discovery" });

    // The default an operator reads off the startup line: the wildcard, on the port
    // `fastcache-cc` looks for when nobody sets `FASTCACHE_ADDR`.
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    REQUIRE(node.size() == 1);
    CHECK(node.front().host == "0.0.0.0");
    CHECK(node.front().port == DefaultNodePort);
    CHECK(cfg.raftListen == DefaultRaftListen);
    CHECK(cfg.discoveryAddress == DefaultDiscoveryAddress);
    auto const raft = RowFor(NodeSurface::Raft).Resolve(cfg);
    REQUIRE(raft.size() == 1);
    CHECK(raft.front().host == "0.0.0.0");
    CHECK(raft.front().port == DefaultRaftPort);
}

TEST_CASE("A bare port takes its own surface's default host", "[node][surfaces]")
{
    // The asymmetry, asserted rather than described. It is the anti-leeching rule:
    // a scheduler no peer can dial does nothing, while a cache any host can dial is
    // this machine's entire build output served to strangers. A worksheet that got
    // this backwards would tell an operator a surface is loopback-only when it is
    // open to the network.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeListen = "6699";
    cfg.adminListen = "6699";
    cfg.raftListen = "6699";
    cfg.nodeId = "n1";

    auto const hostOf = [&cfg](NodeSurface surface) {
        auto const endpoints = RowFor(surface).Resolve(cfg);
        REQUIRE(endpoints.size() == 1);
        return endpoints.front().host;
    };

    CHECK(hostOf(NodeSurface::Node) == "0.0.0.0");
    CHECK(hostOf(NodeSurface::Admin) == "127.0.0.1");
    CHECK(hostOf(NodeSurface::Raft) == "0.0.0.0");
}

TEST_CASE("The node port's default host is the wildcard whether or not this node schedules", "[node][surfaces]")
{
    // The asymmetry the merge kept (#290) -- loopback on a worker, the wildcard on a
    // scheduler -- went with the zero-config defaults: every node is a fleet participant,
    // and a worker bound to loopback advertises an address nobody else can dial. What keeps
    // a cache this machine's alone is admission, not the socket (#287).
    auto worker = Testing::FirstStart(NodeConfig {});
    worker.nodeListen = "6699";
    worker.raftListen.clear();
    worker.raftListenExplicit = true;
    REQUIRE_FALSE(ServesScheduler(worker));
    auto const workerEndpoints = RowFor(NodeSurface::Node).Resolve(worker);
    REQUIRE(workerEndpoints.size() == 1);
    CHECK(workerEndpoints.front().host == "0.0.0.0");

    auto scheduling = Testing::FirstStart(NodeConfig {});
    scheduling.nodeListen = "6699";
    REQUIRE(ServesScheduler(scheduling));
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
    auto worker = Testing::FirstStart(NodeConfig {});
    worker.cacheMemoryBytes = 0;
    worker.cacheDir.clear();
    worker.raftListen.clear();
    worker.raftListenExplicit = true;
    REQUIRE_FALSE(ServesScheduler(worker));
    CHECK(RowFor(NodeSurface::Node).Resolve(worker).size() == 1);

    // And neither component changes the answer any more, which is the whole point:
    // the port is the node's, not the tier's.
    auto caching = worker;
    caching.cacheMemoryBytes = 64ULL * 1024ULL * 1024ULL;
    CHECK(RowFor(NodeSurface::Node).Resolve(caching).size() == 1);

    auto scheduling = worker;
    scheduling.raftListen = std::string { DefaultRaftListen };
    scheduling.raftListenExplicit = false;
    REQUIRE(ServesScheduler(scheduling));
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
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.discoveryAddress = "255.255.255.255:6681";

    auto const unpinned = RowFor(NodeSurface::Discovery).Resolve(cfg);
    REQUIRE(unpinned.size() == 2);
    // NOT 255.255.255.255. The address is where beacons are sent; the socket binds
    // the wildcard unconditionally, and reading the announce address as a bind
    // address would put a broadcast address on a firewall worksheet.
    CHECK(unpinned.front().host == "0.0.0.0");
    CHECK(unpinned.front().port == 6681);
    CHECK(unpinned.front().portKind == SurfacePortKind::Fixed);
    CHECK(unpinned.front().role == "beacon");

    // The second endpoint, which is why the row resolves to a list. A node answers
    // challenges on a port only it holds, so an operator who opened the beacon port
    // and not this one hears every beacon and completes no handshake. It is there
    // UNPINNED too, as a port the kernel chooses: left out, it was a socket no
    // firewall rule and no worksheet line covered.
    CHECK(unpinned[1].host == "0.0.0.0");
    CHECK(unpinned[1].port == 0);
    CHECK(unpinned[1].portKind == SurfacePortKind::KernelChosen);
    CHECK(unpinned[1].role == "reply");

    cfg.discoveryReplyPort = 6682;
    auto const pinned = RowFor(NodeSurface::Discovery).Resolve(cfg);
    REQUIRE(pinned.size() == 2);
    CHECK(pinned[1].port == 6682);
    CHECK(pinned[1].portKind == SurfacePortKind::Fixed);
    CHECK(pinned[1].role == "reply");
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

TEST_CASE("Raft binds exactly when --listen-raft is given", "[node][surfaces]")
{
    // `--listen-raft` IS the switch since #1022, and this row is where that is
    // decided -- `RunsConsensus` reads it here rather than off `cfg.raftListen`, so a
    // worksheet and the mode cannot disagree about whether the port is served.
    //
    // BOTH directions, because the row's gate moved rather than went away: it used to
    // return nothing unless `--node-id` was given, and a test that only drove the
    // fully-configured node would pass under either gate.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.raftListen = "0.0.0.0:6680";
    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).size() == 1);

    // An id with no port names a node in a cluster this process is not opening a door
    // for, so nothing is bound and the worksheet says so. Under the old gate this was
    // the served case, which is what makes it the one worth asserting. The port is named
    // empty, since it is on by default.
    auto named = Testing::FirstStart(NodeConfig {});
    named.nodeId = "n1";
    named.raftListen.clear();
    CHECK(RowFor(NodeSurface::Raft).Resolve(named).empty());
}

TEST_CASE("The worksheet describes this configuration, not the defaults", "[node][surfaces]")
{
    // The failure this flag exists to prevent, asserted at the parse. `--help` and
    // `--version` stop parsing because they ignore the rest of the command line;
    // `--print-surfaces` REPORTS on it, so stopping made
    // `--print-surfaces --listen-node=6675` print a node that was never configured
    // -- a worksheet silently describing a different machine from the one the operator
    // asked about.
    std::vector<char const*> const argv { "--print-surfaces", "--listen-node=6675" };

    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
    REQUIRE(flow.has_value());
    CHECK(cfg.printSurfaces);
    CHECK(cfg.nodeListen == "6675");

    // The wildcard rather than loopback, because this node's mode serves a scheduler: that
    // decides where a bare port lands as well as whether the verbs are served.
    REQUIRE(ServesScheduler(cfg));
    CHECK(RenderSurfaces(cfg).contains("0.0.0.0:6675"));
}

TEST_CASE("The worksheet never prints an announce address as a bind address", "[node][surfaces]")
{
    // The sharpest failure this table can produce. `--discovery=255.255.255.255:6681`
    // is an ordinary thing to write, and reading its host as the bind address would
    // put a BROADCAST address on a firewall worksheet -- in the row an operator is
    // least likely to question, because it is also the only UDP one.
    auto cfg = Testing::FirstStart(NodeConfig {});
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

TEST_CASE("The surface worksheet carries a verdict and prints either way", "[node][surfaces]")
{
    // **`--print-surfaces` printed the map and exited 0 for a configuration the node
    // then refuses to run** (#582). The flag prints the RESOLVED configuration, so an
    // operator reaches for it before writing a unit file -- and it answered "fine" to
    // a command line with no chance of starting.
    //
    // Both halves are asserted, and the second is not decoration: a fix that made the
    // flag refuse everything would satisfy the first on its own.

    SECTION("a configuration that would not start is printed AND refused")
    {
        // The documented worker line, asking for a generated certificate AND naming one -- a
        // `StartupPolicyRejection` row.
        auto cfg = WorkerServingTlsTwoWays();

        auto const report = ReportSurfaces(cfg);

        // Printed in FULL, which is the feature rather than a concession: an operator
        // reaches for this BECAUSE something is wrong, and withholding the worksheet
        // until the configuration is valid withholds it when it is wanted.
        CHECK(report.text.contains("0.0.0.0:6674")); // the RESOLVED endpoint, which is what the flag exists to print
        CHECK_FALSE(report.text.empty());

        // And refused, which is what the exit code follows from.
        REQUIRE(report.refusal.has_value());
        CHECK(Unwrap(report.refusal).starts_with("--tls-self-signed and --tls-cert contradict each other"));

        // **The table's own words, byte for byte.** A second phrasing here would be a
        // second thing to be wrong, and an operator who met one sentence from this flag
        // and a different one at boot would be matching two messages instead of reading
        // one. This is the assertion that fails if somebody later reformats it.
        CHECK(Unwrap(report.refusal) == Unwrap(StartupPolicyRejection(cfg)));
    }

    SECTION("a configuration that would start is printed and NOT refused")
    {
        // The control, and the ticket names it explicitly: without it, a flag that
        // always fails satisfies the section above. On a build without TLS the generated
        // certificate is refused as well (`TlsUnavailableRefusal`), so there the control asks
        // for no TLS at all.
        auto cfg = WithoutTheContradiction(WorkerServingTlsTwoWays());
        if constexpr (!BuildServesTls)
            cfg.tlsSelfSigned = false;

        auto const report = ReportSurfaces(cfg);
        CHECK(report.text.contains("0.0.0.0:6674")); // the RESOLVED endpoint, which is what the flag exists to print
        INFO(report.refusal.value_or("<none>"));
        CHECK_FALSE(report.refusal.has_value());
    }

    SECTION("the worksheet is identical either way")
    {
        // The map does not change shape according to the verdict: judging and rendering
        // are two answers about one configuration, and a reader comparing a broken run
        // with a fixed one should see the map differ only where the configuration does.
        auto const broken = WorkerServingTlsTwoWays();
        auto const fixed = WithoutTheContradiction(broken);

        // Both serve the admin surface over TLS on the one port, and which certificate it presents
        // appears in no surface row -- which is what makes this comparison exact rather than
        // approximate.
        CHECK(ReportSurfaces(broken).text == ReportSurfaces(fixed).text);
    }
}

TEST_CASE("The worksheet and the install and the start refuse a cross-flag rule in one sentence",
          "[node][surfaces][verdict]")
{
    // A report from a real installation said `--print-surfaces` accepted -- printed the map and
    // exited 0 for -- a line the service then refused at every start. It does not reproduce: the
    // worksheet's verdict IS the startup table's, and the binary exits 2 on such a line. What
    // reported 0 was the deploy script's `| Select-Object -First 4`, which stops the pipeline
    // before PowerShell records the native command's exit code, so `$LASTEXITCODE` still held the
    // previous command's 0.
    //
    // Kept as the pin that the three judgements of one command line cannot drift apart: parsed
    // exactly as a registration spells it, through the parser `main` uses. The reported line named
    // `--serve-scheduler`, which is gone -- the formation record decides -- so the pin rides a
    // cross-flag rule that is still a row: a TYPED `--discovery` on a node that runs no consensus.
    auto const line = [](char const* raftListen) {
        return std::vector<char const*> {
            "--listen-node=127.0.0.1:6674",
            raftListen,
            "--discovery=255.255.255.255:6681",
            "--advertise=127.0.0.1:6674",
            "--admin-listen=0.0.0.0:6677",
            "--dashboard",
            R"(--dashboard-token-file=D:\.token)",
            "--cache-disk=68719476736",
            R"(--cache-dir=C:\ProgramData\fastcache-node\cache)",
        };
    };

    auto const judge = [](std::vector<char const*> const& argv) {
        NodeConfig cfg;
        auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
        INFO((flow.has_value() ? std::string {} : flow.error().ToString()));
        REQUIRE(flow.has_value());
        // As `main` shapes it after the parse: the mode a first start mints, which is what decides
        // whether consensus runs at all.
        Testing::ShapeAsFirstStart(cfg);
        struct Verdicts
        {
            std::optional<std::string> worksheet; ///< What `--print-surfaces` exits on.
            std::optional<std::string> install;   ///< What `--install-service` exits on.
            std::optional<std::string> start;     ///< What a start exits on.
        };
        return Verdicts { .worksheet = ReportSurfaces(cfg).refusal,
                          .install = NodeInstallRejection(cfg),
                          .start = StartupPolicyRejection(cfg) };
    };

    SECTION("the line with consensus turned off: all three refuse, in the start's own words")
    {
        auto const verdicts = judge(line("--listen-raft="));
        REQUIRE(verdicts.start.has_value());
        CHECK(Unwrap(verdicts.start) == DiscoveryNeedsConsensusRefusal);
        CHECK(verdicts.worksheet == verdicts.start);
        CHECK(verdicts.install == verdicts.start);
    }

    SECTION("the line with its cluster of one: all three accept")
    {
        // The control: without it a worksheet that refused everything would satisfy the section
        // above. The refusal's remedy taken literally -- a consensus port, with an address peers
        // can dial, since a typed `--discovery` beaconing one that reaches only this machine is
        // refused too -- plus the `--cluster-dir` an INSTALL alone demands of a consensus node
        // (`NodeServiceRejection`), since a service's working directory is not the installing shell's.
        auto corrected = line("--listen-raft=0.0.0.0:6680");
        corrected.push_back("--raft-self=10.0.0.5");
        corrected.push_back(R"(--cluster-dir=C:\ProgramData\fastcache-node\cluster)");
        auto const verdicts = judge(corrected);
        INFO(verdicts.start.value_or("<none>"));
        INFO(verdicts.install.value_or("<none>"));
        CHECK_FALSE(verdicts.start.has_value());
        CHECK_FALSE(verdicts.worksheet.has_value());
        CHECK_FALSE(verdicts.install.has_value());
    }
}

TEST_CASE("A surface that is off is named, with what would turn it on", "[node][surfaces]")
{
    // Omitting it would read as a surface this build does not have, which an operator
    // cannot tell from one they simply did not switch on.
    auto const sheet = RenderSurfaces(Testing::FirstStart(NodeConfig {}));
    CHECK(sheet.contains("--admin-listen"));

    // And the compile port's caveat travels with the sheet rather than living only in
    // the documentation: its flags describe nothing under socket activation, which is
    // the one way this list can be wrong about a port it does print.
    CHECK(sheet.contains(".socket"));

    // The PRIMARY flag alone, never every flag the row carries. Discovery is the row
    // where that matters: `--discovery-reply-port` is optional, and printing it beside
    // `--discovery` reads as two flags an operator must set to hear a beacon at all.
    // Discovery is on by default, so it is turned off to be asked.
    auto quiet = Testing::FirstStart(NodeConfig {});
    quiet.discoveryAddress.clear();
    CHECK(RenderSurfaces(quiet).contains("not served; set --discovery\n"));
}

TEST_CASE("A surface that is configured and still off is not answered 'set the flag'", "[node][surfaces]")
{
    // The ways a row goes unserved that are NOT "you did not ask for it", and they are
    // what an operator actually meets -- because `--print-surfaces` runs before
    // `StartupPolicyRejection`, deliberately, so the map is available exactly while a
    // port is still wrong.
    //
    // **The case this was written for is gone, and saying so is the assertion.** Told
    // to "set --listen-raft", somebody who had just written it went looking at the
    // flag rather than at `--node-id`; #1022 made `--listen-raft` the switch, so that
    // configuration now SERVES. Asserting the served endpoint rather than deleting the
    // case is what keeps the old behaviour from coming back unremarked -- the wrong
    // answer and the right one differ by one row's `resolve`.
    auto raft = Testing::FirstStart(NodeConfig {});
    raft.raftListen = "6680";

    auto const waiting = RenderSurfaces(raft);
    CHECK_FALSE(waiting.contains("set --listen-raft"));
    CHECK_FALSE(waiting.contains("see the raft note below"));
    CHECK(waiting.contains("0.0.0.0:6680"));
    // And the note names the switch, which is the formation record's mode rather than this row's
    // flag -- the flag only says where, and an empty one closes the port.
    CHECK(RowFor(NodeSurface::Raft).note.contains("mode opens the port"));
    CHECK(RowFor(NodeSurface::Raft).note.contains("an empty --listen-raft= closes it"));

    // A row with no spec text at all still gets the first answer, which is the one
    // that IS right for it.
    auto silent = Testing::FirstStart(NodeConfig {});
    silent.raftListen.clear();
    CHECK(RenderSurfaces(silent).contains("set --listen-raft"));

    // A malformed value is echoed in the shape the row advertises -- the same sentence
    // `StartupPolicyRejection` produces from the same columns a moment later, rather
    // than an instruction to set a flag that is already set.
    auto typo = Testing::FirstStart(NodeConfig {});
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

    // Including what the advertised endpoint is under activation: read off the socket, since
    // `Node::AdoptActivatedBind` -- `--advertise` is no longer required there.
    CHECK(note.contains("reads the address and port off the socket"));
    CHECK_FALSE(note.contains("required under activation"));
}

TEST_CASE("Every surface states what a bind failure does, and why", "[node][surfaces][bind]")
{
    // The verdict alone would have been satisfied by what every row already did --
    // all four refuse, and did before #352. What was missing was the sentence, and
    // for one surface it was missing everywhere: raft's bind site records only why
    // the listener is a reactor one and why `IsBound()` beats a null check, and the
    // only statement covering its fatality lived inside DISCOVERY's comment, in
    // another file, as a parenthetical about "the other two surfaces".
    //
    // So the reason is asserted as well as the policy. The table `static_assert`s
    // both; this case is what makes the requirement readable, and what fails loudly
    // if somebody weakens the compile-time guard to get a build through.
    for (auto const& row: NodeSurfaceTable())
    {
        INFO("surface " << row.name);
        CHECK(row.bindFailure != BindFailurePolicy::Unstated);
        CHECK_FALSE(row.bindFailureReason.empty());
    }
}

TEST_CASE("A surface that refuses a bind failure passes its own message through", "[node][surfaces][bind]")
{
    // The opener's sentence reaches the operator unchanged. It names the REMEDY
    // rather than the diagnosis (#229) -- "the usual cause is a fastcached holding
    // that port" is worth more at three in the morning than any rationale -- so the
    // row's reason must not be appended to it.
    CapturingLogger logger;
    auto const judged = JudgeBindFailure(RowFor(NodeSurface::Node), "cannot bind 0.0.0.0:6674 (in use)", logger);

    REQUIRE_FALSE(judged.has_value());
    CHECK(judged.error() == "cannot bind 0.0.0.0:6674 (in use)");

    // And nothing is logged: the caller refuses, and main.cpp is what reports it.
    // Logging here as well would print the same failure twice.
    CHECK(logger.Snapshot().empty());
}

TEST_CASE("A surface that tolerates a bind failure warns and carries on", "[node][surfaces][bind]")
{
    // **The clause a test over the real table cannot satisfy.** Asserting that
    // today's four surfaces refuse passes forever once somebody adds a tolerant
    // fifth -- which is the failure this ticket exists to prevent, one level up. So
    // the seam takes a ROW rather than a `NodeSurface`, and this hands it a shape no
    // production row has.
    SurfaceRow tolerant = RowFor(NodeSurface::Admin);
    tolerant.bindFailure = BindFailurePolicy::Tolerate;
    tolerant.bindFailureReason = "a stand-in reason, so the warning has something to carry";

    CapturingLogger logger;
    auto const judged = JudgeBindFailure(tolerant, "cannot bind 127.0.0.1:6677 (in use)", logger);

    CHECK(judged.has_value());

    // Snapshotted once: it returns by value, so calling it per assertion would
    // compare four different copies and read as though it were one.
    auto const records = logger.Snapshot();

    // Warn, not info: the operator asked for a surface and is not getting it.
    REQUIRE(records.size() == 1);
    CHECK(records.front().level == LogLevel::Warn);

    // The reason travels HERE and only here -- there is no refusal to carry it, and
    // a tolerated failure is exactly the case where somebody later asks why the node
    // thought this was survivable.
    CHECK(records.front().message.contains("a stand-in reason"));
    CHECK(records.front().message.contains("cannot bind 127.0.0.1:6677 (in use)"));
    CHECK(records.front().message.contains("continuing without it"));
}

TEST_CASE("A row with no stated policy is refused rather than assumed", "[node][surfaces][bind]")
{
    // Unreachable through the table, which refuses `Unstated` at compile time. It is
    // reachable by a caller that builds a row by hand and leaves the column out, and
    // the answer there is a refusal ABOUT THE CALLER rather than about the surface --
    // so the message says so instead of silently picking the safe-looking verdict.
    SurfaceRow unstated = RowFor(NodeSurface::Raft);
    unstated.bindFailure = BindFailurePolicy::Unstated;

    CapturingLogger logger;
    auto const judged = JudgeBindFailure(unstated, "cannot bind 0.0.0.0:6680 (in use)", logger);

    REQUIRE_FALSE(judged.has_value());
    CHECK(judged.error().contains("no bind-failure policy stated"));
}

TEST_CASE("Raft's reason is about the quorum, not about this node", "[node][surfaces][bind]")
{
    // Written for #352, and the one row whose sentence existed nowhere beforehand --
    // which makes it the row where a plausible-sounding invention would go unnoticed.
    // It is pinned on the fact that makes it true rather than on its wording: a node
    // whose raft listener fails still serves compiles and still caches, so the reason
    // cannot be "this node stops working". It is that the CLUSTER stalls, because a
    // peer that cannot be dialled is still counted in the quorum it is absent from.
    auto const& reason = RowFor(NodeSurface::Raft).bindFailureReason;

    CHECK(reason.contains("quorum"));
    CHECK(reason.contains("serve compiles"));
}

TEST_CASE("No production row is tolerated, because no opener can carry one", "[node][surfaces][bind]")
{
    // The seam understands `Tolerate` -- the case above proves it warns and continues
    // -- but understanding the verdict and being able to ACT on it are two facts, and
    // this asserts the second is currently false everywhere.
    //
    // It is not a restatement of the "every row states a policy" case. That one
    // forbids the absence of a policy; this forbids the one policy whose caller-side
    // half is unwritten. Two openers would return a null surface inside a satisfied
    // `expected` and their callers dereference it with no check, so a flipped column
    // is a startup crash rather than a degraded node.
    //
    // The table `static_assert`s this, so the case cannot fail without the build
    // failing first. It is here to be READ: somebody adding a tolerant surface meets
    // the requirement in the test file as well as in the compiler output, and the
    // reason is written where they are already looking.
    for (auto const& row: NodeSurfaceTable())
    {
        INFO("surface " << row.name);
        CHECK(row.bindFailure == BindFailurePolicy::Refuse);
    }
}

TEST_CASE("An opener handed a tolerated verdict blames itself, not the port", "[node][surfaces][bind]")
{
    // The distinction the sentence has to carry: an operator reading it must not go
    // looking for whatever holds the port, because nothing does -- the bind failure
    // was survivable and this binary could not survive it. So the text names the
    // flag (which is where they would start), names the surface, and says plainly
    // that the defect is in the build rather than in what they typed.
    auto const message =
        BindToleranceUnsupported(RowFor(NodeSurface::Admin), "the caller dereferences the endpoint unconditionally");

    CHECK(message.contains("--admin-listen"));
    CHECK(message.contains("admin"));
    CHECK(message.contains("the caller dereferences the endpoint unconditionally"));

    // The load-bearing half. Without it this reads as a configuration error, and the
    // operator spends the night on a port that was never the problem.
    CHECK(message.contains("defect in this binary"));
    CHECK_FALSE(message.contains("refusing to start"));
}

// ---------------------------------------------------------------------------
// #1328: the consensus address peers DIAL, printed apart from the one the node BINDS.

namespace
{

/// A consensus node that binds the wildcard and names where peers dial it.
///
/// **The distinguishing fixture, and the one that does not suggest itself.** A bind
/// and a dial address that coincide -- `--listen-raft=10.0.0.4:6680 --raft-self=10.0.0.4`
/// -- print the same string under both readings, so a worksheet that printed the bound
/// address as the dial address would pass. A bare `--listen-raft` binds the wildcard,
/// which is the ordinary deployment and the one where the two differ.
/// @return The configuration.
[[nodiscard]] NodeConfig WildcardBoundWithRaftSelf()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.raftListen = "6680";
    cfg.raftSelf = "10.0.0.4";
    return cfg;
}

/// The dial address's label as the worksheet prints it: indented under `dialled at:`.
/// @return Two spaces and `CompileCacheWire::ConsensusEndpointLabel`.
[[nodiscard]] std::string DialLabel()
{
    return std::format("  {}", CompileCacheWire::ConsensusEndpointLabel);
}

/// The one line of @p sheet that starts with @p label.
/// @param sheet What `RenderSurfaces` printed.
/// @param label The line's leading words.
/// @return The line, or nothing when no line starts so.
[[nodiscard]] std::optional<std::string> LineStarting(std::string_view sheet, std::string_view label)
{
    for (auto const line: std::views::split(sheet, '\n'))
    {
        auto const text = std::string_view { line.begin(), line.end() };
        if (text.starts_with(label))
            return std::string { text };
    }
    return std::nullopt;
}

/// The address column of a worksheet line: the first word after its label.
/// @param line One line of the worksheet.
/// @param label The line's label, which may itself contain spaces.
/// @return The address column.
[[nodiscard]] std::string AddressColumnOf(std::string_view line, std::string_view label)
{
    auto rest = line.substr(label.size());
    rest.remove_prefix(std::min(rest.find_first_not_of(' '), rest.size()));
    return std::string { rest.substr(0, rest.find(' ')) };
}

} // namespace

TEST_CASE("The worksheet prints the consensus address peers DIAL apart from the one the node BINDS",
          "[node][surfaces][consensus]")
{
    auto const cfg = WildcardBoundWithRaftSelf();

    auto const dial = ConsensusDialAddressOf(cfg);
    REQUIRE(dial.has_value());
    CHECK(*dial == "10.0.0.4:6680");

    auto const sheet = RenderSurfaces(cfg);
    INFO(sheet);
    auto const raftRow = LineStarting(sheet, "raft ");
    auto const dialLine = LineStarting(sheet, DialLabel());
    REQUIRE(raftRow.has_value());
    REQUIRE(dialLine.has_value());

    auto const bound = AddressColumnOf(Unwrap(raftRow), "raft");
    auto const dialled = AddressColumnOf(Unwrap(dialLine), DialLabel());
    CHECK(bound == "0.0.0.0:6680");
    CHECK(dialled == "10.0.0.4:6680");
    // The acceptance, stated as what DISTINGUISHES: the two printed addresses differ. A
    // worksheet printing the bound address twice passes every other line of this case
    // that reads only one of them.
    CHECK(dialled != bound);
    // And the line says which is which, because the comparison is the whole value.
    CHECK(Unwrap(dialLine).contains("DIAL"));
    CHECK(Unwrap(dialLine).contains("BINDS"));

    // A block of its own, never a row of the bound-address table: the table's rows are
    // what a firewall worksheet is transcribed from, and a row is a COLUMN-ONE line to every
    // reader of a pasted transcript -- the docs check included. So the label is indented
    // under its heading, and nothing in column one names it.
    CHECK_FALSE(LineStarting(sheet, "consensus").has_value());
    // Each heading FOUND before it is compared: a missing one is `npos`, greater than every
    // position, so "`raft` comes before it" would hold on a worksheet that printed no heading.
    auto const heading = sheet.find("\ndialled at:\n");
    auto const notes = sheet.find("\nnotes:");
    REQUIRE(heading != std::string::npos);
    REQUIRE(notes != std::string::npos);
    CHECK(heading < sheet.find(DialLabel()));
    CHECK(sheet.find("raft ") < heading);
    CHECK(sheet.find(DialLabel()) < notes);
}

TEST_CASE("A node running no consensus prints its dial address as ABSENT, not as an empty one",
          "[node][surfaces][consensus]")
{
    // `RunsConsensus` is false iff `--listen-raft` does not resolve, and that is a real
    // deployment: a plain worker. Absent is not zero -- an empty address column would read
    // as "the node dials nowhere", which is a different fact from "there is no consensus".
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.raftSelf = "10.0.0.4"; // named, and still nothing to dial: no consensus port
    cfg.raftListen.clear();

    auto const dial = ConsensusDialAddressOf(cfg);
    REQUIRE_FALSE(dial.has_value());
    CHECK(dial.error() == ConsensusDialGap::NoConsensus);

    auto const line = LineStarting(RenderSurfaces(cfg), DialLabel());
    REQUIRE(line.has_value());
    CHECK(AddressColumnOf(Unwrap(line), DialLabel()) == "-");
    CHECK(Unwrap(line).contains("absent"));
    CHECK_FALSE(Unwrap(line).contains("10.0.0.4"));
}

TEST_CASE("A consensus node that names itself neither way prints NOT STATED and the flags that would state it",
          "[node][surfaces][consensus]")
{
    // `--print-surfaces` prints a configuration the node refuses, which is its point, so it
    // has to be able to say that nobody stated this address -- neither an address nor an
    // absence would be true. "Neither way" is a host name that RESOLVED to nothing; before
    // it resolves the address is awaited, which is its own line.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.raftListen = "6680";
    auto awaiting = cfg;
    ApplyHostNames(cfg, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = {} });

    auto const dial = ConsensusDialAddressOf(cfg);
    REQUIRE_FALSE(dial.has_value());
    CHECK(dial.error() == ConsensusDialGap::Unstated);

    auto const line = LineStarting(RenderSurfaces(cfg), DialLabel());
    REQUIRE(line.has_value());
    CHECK(Unwrap(line).contains("NOT STATED"));
    CHECK(Unwrap(line).contains("--raft-self"));

    auto const pending = LineStarting(RenderSurfaces(awaiting), DialLabel());
    REQUIRE(pending.has_value());
    CHECK(Unwrap(pending).contains("AT STARTUP"));
    CHECK_FALSE(Unwrap(pending).contains("NOT STATED"));
}

TEST_CASE("the metrics surfaces a node serves follow the components it was told to run", "[node][surfaces][metrics]")
{
    // #1501 attributed all 148 catalogue rows to a surface, which makes THIS the number that
    // decides whether a figure is reported. It used to be an empty `constexpr` array -- right
    // while `CacheAcceptPath` was the only surface and a node serves none of it, and the silent
    // failure once every row was attributed, because empty renders all 148 absent.
    auto const serves = [](NodeConfig const& cfg, MetricsSurface surface) {
        auto const served = NodeServedSurfacesFor(cfg);
        return std::ranges::find(served.Span(), surface) != served.Span().end();
    };

    SECTION("three are a property of the BINARY and no flag turns them on")
    {
        // A compile node constructs no `Server`, no `CacheEngine` and no daemon-side `0xFC`
        // executor, so these cannot move at any traffic level ever -- which is #1484's finding.
        // Asserted against a RICH configuration rather than a default one: a default `NodeConfig`
        // serves little, so a case built on one passes for the wrong reason.
        auto rich = Testing::FirstStart(NodeConfig {});
        rich.slots = 4;
        REQUIRE(ServesScheduler(rich));
        rich.cacheMemoryBytes = 8 * 1024 * 1024;
        rich.nodeListen = "127.0.0.1:1";

        CHECK_FALSE(serves(rich, MetricsSurface::CacheAcceptPath));
        CHECK_FALSE(serves(rich, MetricsSurface::CacheStorage));
        CHECK_FALSE(serves(rich, MetricsSurface::CacheCompileSurface));

        // And two it always serves, so the case above is not passing because the set is empty.
        CHECK(serves(rich, MetricsSurface::NodeFrameEndpoint));
        CHECK(serves(rich, MetricsSurface::LiveStats));
    }

    SECTION("the worker follows --slots, in both directions")
    {
        // BOTH directions, because a build that never served `CompileWorker` would pass the
        // `--slots=0` half on its own -- and that is the direction this feature could plausibly
        // break, since it renders a real figure as `-`.
        auto withWorker = Testing::FirstStart(NodeConfig {});
        withWorker.slots = 2;
        REQUIRE(RunsWorker(withWorker));
        CHECK(serves(withWorker, MetricsSurface::CompileWorker));

        auto withoutWorker = Testing::FirstStart(NodeConfig {});
        withoutWorker.slots = 0;
        REQUIRE_FALSE(RunsWorker(withoutWorker));
        CHECK_FALSE(serves(withoutWorker, MetricsSurface::CompileWorker));
    }

    SECTION("the scheduler follows the mode, and enrollment needs the scheduler BESIDE consensus")
    {
        // `ServesEnrollment` is `RunsConsensus(cfg) && a started scheduler tier`, so consensus
        // alone must NOT bring the enrollment surface with it. That conjunction is the one thing
        // here a single predicate would have got wrong -- and a LEARNER is the node it separates:
        // it runs consensus and its mode serves no scheduler.
        auto scheduling = Testing::FirstStart(NodeConfig {});
        REQUIRE(ServesScheduler(scheduling));
        CHECK(serves(scheduling, MetricsSurface::CompileScheduler));
        CHECK(serves(scheduling, MetricsSurface::NodeEnrollment));

        auto learner = Testing::FirstStart(NodeConfig {});
        REQUIRE(learner.formation.has_value());
        if (learner.formation.has_value())
            learner.formation->mode = Cluster::NodeMode::Learner;
        REQUIRE(RunsConsensus(learner));
        REQUIRE_FALSE(ServesScheduler(learner));
        CHECK_FALSE(serves(learner, MetricsSurface::CompileScheduler));
        CHECK_FALSE(serves(learner, MetricsSurface::NodeEnrollment));

        auto neither = Testing::FirstStart(NodeConfig {});
        neither.raftListen.clear();
        neither.raftListenExplicit = true;
        REQUIRE_FALSE(RunsConsensus(neither));
        CHECK_FALSE(serves(neither, MetricsSurface::CompileScheduler));
        CHECK_FALSE(serves(neither, MetricsSurface::NodeEnrollment));
        CHECK_FALSE(serves(neither, MetricsSurface::ConsensusPeerWire));
    }

    SECTION("a default-constructed ServedSurfaces means NOT NARROWED, so it holds every surface")
    {
        // The direction of a mistake is stated here rather than left to a reader: a set left too
        // WIDE renders a plausible zero, which is what this tree did before any of this existed;
        // one left too NARROW invents a `-`. So an unrevisited call site must keep the old
        // behaviour, and an empty default did the opposite -- it rendered all 148 rows absent and
        // one case out of ~4900 noticed.
        ServedSurfaces const notNarrowed {};
        CHECK(notNarrowed.count == EverySurface.size());
        for (auto const surface: EverySurface)
            CHECK(std::ranges::find(notNarrowed.Span(), surface) != notNarrowed.Span().end());
    }
}

TEST_CASE("Discovery on a node running no consensus is not served, and the worksheet says why",
          "[node][surfaces][formation][defaults]")
{
    // Discovery is on by default and runs beside consensus only, so a worker that turns
    // consensus off with an empty --listen-raft opens no discovery socket -- and a worksheet
    // or a --node-status reporting one would name a port nothing bound.
    auto worker = Testing::FirstStart(NodeConfig {});
    worker.raftListen.clear();
    REQUIRE_FALSE(worker.discoveryAddress.empty());
    CHECK(RowFor(NodeSurface::Discovery).Resolve(worker).empty());
    CHECK(RenderSurfaces(worker).contains("not served; see the discovery note below"));
    CHECK(RowFor(NodeSurface::Discovery).note.contains("beside consensus only"));

    // The control: the same address beside consensus is served.
    CHECK_FALSE(RowFor(NodeSurface::Discovery).Resolve(Testing::FirstStart(NodeConfig {})).empty());
}

TEST_CASE("Every node is attributed the shared-cache counters", "[node][surfaces][metrics][shared-cache]")
{
    // Every node builds the shared-cache component on its merged listener, dormant or serving, so
    // every node can move these -- a dormant one counts its not-serving refusals.
    CHECK(std::ranges::contains(NodeServedSurfacesFor(NodeConfig {}).Span(), MetricsSurface::NodeSharedCache));
}

TEST_CASE("The worksheet prints a kernel-chosen port as one nobody can name, never as port 0", "[node][surfaces]")
{
    // A `:0` on a worksheet is a port an operator would copy into a firewall rule, and a
    // rule on port 0 admits nothing -- so the reply socket's line says whose port it is.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.discoveryAddress = "255.255.255.255:6681";

    auto const unpinned = LineStarting(RenderSurfaces(cfg), "discovery reply ");
    REQUIRE(unpinned.has_value());
    CHECK(AddressColumnOf(Unwrap(unpinned), "discovery reply") == "0.0.0.0:*");
    CHECK(Unwrap(unpinned).ends_with("UDP, port chosen by the kernel at bind"));

    // Pinned, it is a port like any other, and the line says nothing more than the protocol.
    cfg.discoveryReplyPort = 6682;
    auto const pinned = LineStarting(RenderSurfaces(cfg), "discovery reply ");
    REQUIRE(pinned.has_value());
    CHECK(AddressColumnOf(Unwrap(pinned), "discovery reply") == "0.0.0.0:6682");
    CHECK(Unwrap(pinned).ends_with("  UDP"));
}

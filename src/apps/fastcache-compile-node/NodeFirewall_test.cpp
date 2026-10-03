// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeFirewall.hpp"
#include "NodeKey.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Platform/Firewall.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FirewallFakes.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// An absolute path on this host, since the cases that apply their rules need one: a rule whose
/// program is not absolute is refused before the firewall is touched.
#if defined(_WIN32)
inline std::filesystem::path const Program { "C:/Program Files/fastcached/bin/fastcache-compile-node.exe" };
#else
inline std::filesystem::path const Program { "/opt/fastcached/bin/fastcache-compile-node" };
#endif

/// A node as `--install-service` sees it. `main` applies the formation record before any early
/// verb runs, so the raft and discovery rows are judged by the mode a first start takes -- and
/// since the zero-config defaults that mode runs consensus on 6680 and beacons. A bare
/// `NodeConfig` has no mode yet and serves neither row, which no install ever derives rules from.
/// @return The configuration a first start runs with.
[[nodiscard]] NodeConfig Installed()
{
    return Testing::FirstStart(NodeConfig {});
}

/// The name of every rule, in order, so a case asserts WHICH surfaces were opened.
[[nodiscard]] std::vector<std::string> Openings(std::vector<FirewallRule> const& rules)
{
    std::vector<std::string> openings;
    openings.reserve(rules.size());
    for (auto const& rule: rules)
        openings.push_back(rule.name);
    return openings;
}

} // namespace

TEST_CASE("A node that binds only loopback opens nothing in the firewall", "[node][firewall]")
{
    // Consensus off, which takes discovery with it: the two rows that bind the wildcard whatever
    // the node listens on.
    auto worker = Installed();
    worker.nodeListen = "127.0.0.1:6674";
    worker.raftListen.clear();
    CHECK(NodeFirewallRules(worker, Program).empty());
}

TEST_CASE("A zero-config node's install opens its node and raft ports and both discovery sockets",
          "[node][firewall][defaults]")
{
    // What the MSI's registration opens with nothing configured. Discovery is on by default, so
    // the any-port reply rule is part of EVERY default install, not an opt-in: without it a node
    // hears every beacon and completes no handshake.
    auto const rules = NodeFirewallRules(Installed(), Program);
    CHECK(Openings(rules)
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674",
                                        "FastCacheCompileNode raft tcp/6680",
                                        "FastCacheCompileNode discovery-beacon udp/6681",
                                        "FastCacheCompileNode discovery-reply udp/any" });
}

TEST_CASE("A node opens every surface that faces the network, each on its own protocol", "[node][firewall]")
{
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.discoveryAddress = "255.255.255.255:6681";
    cfg.discoveryReplyPort = 6682;

    auto const rules = NodeFirewallRules(cfg, Program);
    CHECK(Openings(rules)
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674",
                                        "FastCacheCompileNode raft tcp/6680",
                                        "FastCacheCompileNode discovery-beacon udp/6681",
                                        "FastCacheCompileNode discovery-reply udp/6682" });
    for (auto const& rule: rules)
    {
        CHECK(rule.group == FirewallGroupFor("FastCacheCompileNode"));
        CHECK(rule.program == Program);
        CHECK(rule.serviceName == "FastCacheCompileNode");
        CHECK(rule.remoteAddresses.empty());
    }
}

TEST_CASE("A node's kernel-chosen discovery reply port is opened by program, on every local UDP port", "[node][firewall]")
{
    // The default: `--discovery-reply-port` unset, so the kernel chooses the port every
    // challenge and proof comes back to. No port-scoped rule can name it, and a node with only
    // the beacon rule hears every beacon and completes no handshake -- measured on Windows, where
    // the node never received a peer's challenge.
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.discoveryAddress = "255.255.255.255:6681";

    auto const rules = NodeFirewallRules(cfg, Program);
    CHECK(Openings(rules)
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674",
                                        "FastCacheCompileNode raft tcp/6680",
                                        "FastCacheCompileNode discovery-beacon udp/6681",
                                        "FastCacheCompileNode discovery-reply udp/any" });
    REQUIRE(rules.size() == 4);

    // The beacon keeps its port; only the reply socket is any-port, and still the program's
    // and the service's alone.
    CHECK(rules[2].localPort == FirewallLocalPort { .kind = FirewallPortKind::Fixed, .number = 6681 });
    auto const& reply = rules[3];
    CHECK(reply.protocol == FirewallProtocol::Udp);
    CHECK(reply.localPort == FirewallLocalPort { .kind = FirewallPortKind::Any, .number = 0 });
    CHECK(reply.program == Program);
    CHECK(reply.serviceName == "FastCacheCompileNode");
    CHECK(reply.group == FirewallGroupFor("FastCacheCompileNode"));

    // And the firewall takes it: an any-port rule passes the checks a port-0 rule fails.
    Testing::RecordingFirewall firewall;
    auto const applied = ApplyServiceFirewall(firewall, FirewallGroupFor("FastCacheCompileNode"), rules);
    REQUIRE(applied.has_value());
    CHECK(applied->added == 4);
}

TEST_CASE("A node's named discovery reply port gets that one port and no any-port rule", "[node][firewall]")
{
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.discoveryAddress = "255.255.255.255:6681";
    cfg.discoveryReplyPort = 6690;

    auto const rules = NodeFirewallRules(cfg, Program);
    CHECK(Openings(rules)
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674",
                                        "FastCacheCompileNode raft tcp/6680",
                                        "FastCacheCompileNode discovery-beacon udp/6681",
                                        "FastCacheCompileNode discovery-reply udp/6690" });
    CHECK(
        std::ranges::none_of(rules, [](FirewallRule const& rule) { return rule.localPort.kind == FirewallPortKind::Any; }));
}

TEST_CASE("A node without discovery opens neither discovery rule", "[node][firewall]")
{
    // `--discovery-reply-port` on its own is refused when the command line is parsed; here it is
    // set anyway, so the derivation is seen to follow `--discovery` rather than the reply port.
    // Discovery is on by default, so this node turned it off: an empty `--discovery=`.
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.discoveryAddress.clear();
    cfg.discoveryReplyPort = 6690;

    CHECK(Openings(NodeFirewallRules(cfg, Program))
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674", "FastCacheCompileNode raft tcp/6680" });
}

TEST_CASE("A node's uninstall removes the any-port discovery reply rule with the rest", "[node][firewall]")
{
    // Through the pair `main` calls. The install opens the derived rules; the uninstall removes
    // the service's GROUP rather than re-deriving anything, so the any-port rule goes whatever
    // the configuration at uninstall says.
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.discoveryAddress = "255.255.255.255:6681";

    Testing::RecordingFirewall firewall;
    auto const installed =
        WithRegistrationFirewall(ServiceControlResult { .outcome = ServiceControlOutcome::Created, .message = "installed" },
                                 &firewall,
                                 cfg.serviceName,
                                 NodeFirewallRules(cfg, Program));
    REQUIRE(installed.ExitCode() == 0);
    auto const opened = firewall.NamesInGroup(FirewallGroupFor(cfg.serviceName));
    REQUIRE(opened.has_value());
    CHECK(std::ranges::count(Testing::Unwrap(opened), std::string { "FastCacheCompileNode discovery-reply udp/any" }) == 1);

    auto const removed =
        WithRemovalFirewall(ServiceControlResult { .outcome = ServiceControlOutcome::Removed, .message = "removed" },
                            &firewall,
                            cfg.serviceName);
    CHECK(removed.ExitCode() == 0);
    CHECK(firewall.rules.empty());
}

TEST_CASE("A node's --firewall-allow scopes every rule, and --service-name names them", "[node][firewall]")
{
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.serviceName = "FastCacheNodeB";
    cfg.firewallAllow = { "10.0.0.0/8", "fd00::/8" };
    cfg.discoveryAddress.clear();

    auto const rules = NodeFirewallRules(cfg, Program);
    REQUIRE(rules.size() == 2);
    CHECK(rules.front().name == "FastCacheNodeB node tcp/6674");
    for (auto const& rule: rules)
    {
        CAPTURE(rule.name);
        CHECK(rule.group == "fastcached: FastCacheNodeB");
        CHECK(rule.remoteAddresses == cfg.firewallAllow);
    }

    // The any-port reply rule is scoped exactly as the others. It is the widest of them on the
    // local side, which makes it the last one to leave open to every remote address.
    cfg.discoveryAddress = "255.255.255.255:6681";
    auto const withDiscovery = NodeFirewallRules(cfg, Program);
    CHECK(Openings(withDiscovery)
          == std::vector<std::string> { "FastCacheNodeB node tcp/6674",
                                        "FastCacheNodeB raft tcp/6680",
                                        "FastCacheNodeB discovery-beacon udp/6681",
                                        "FastCacheNodeB discovery-reply udp/any" });
    for (auto const& rule: withDiscovery)
    {
        CAPTURE(rule.name);
        CHECK(rule.remoteAddresses == cfg.firewallAllow);
    }
}

TEST_CASE("A node opens its raft port and not an admin port left at its loopback default", "[node][firewall]")
{
    auto cfg = Installed();
    cfg.nodeListen = "[::1]:6674"; // IPv6 loopback: no rule
    cfg.adminListen = "9464";      // a bare port binds the admin surface's loopback default: no rule
    cfg.raftListen = "6690";       // a bare port binds raft's wildcard default: a rule
    cfg.discoveryAddress.clear();  // discovery off, so the raft row is the only one left

    CHECK(Openings(NodeFirewallRules(cfg, Program)) == std::vector<std::string> { "FastCacheCompileNode raft tcp/6690" });
}

TEST_CASE("A node bound to the IPv6 wildcard opens its port", "[node][firewall]")
{
    // Consensus off, which takes discovery with it, so the two rows under test are the only ones.
    auto cfg = Installed();
    cfg.nodeListen = "[::]:6674";
    cfg.adminListen = "[::]:9464";
    cfg.raftListen.clear();

    auto const rules = NodeFirewallRules(cfg, Program);
    CHECK(Openings(rules)
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674", "FastCacheCompileNode admin tcp/9464" });
    for (auto const& rule: rules)
        CHECK(rule.bindHost == "::");
}

TEST_CASE("Every surface's protocol has a firewall spelling", "[node][firewall]")
{
    // A row whose protocol the mapping lacks would be skipped in silence -- a surface facing the
    // network with no rule, which presents as a fleet that never forms.
    auto cfg = Installed();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.discoveryAddress = "255.255.255.255:6681";
    auto const rules = NodeFirewallRules(cfg, Program);
    CHECK(std::ranges::any_of(rules, [](FirewallRule const& rule) { return rule.protocol == FirewallProtocol::Udp; }));
    CHECK(std::ranges::any_of(rules, [](FirewallRule const& rule) { return rule.protocol == FirewallProtocol::Tcp; }));

    // And by the table, so a row added later is covered whether or not a configuration above
    // happens to turn it on.
    for (auto const& row: NodeSurfaceTable())
    {
        CAPTURE(row.name);
        CHECK(FirewallProtocolOf(row.protocol).has_value());
    }
}

TEST_CASE("Every surface port kind maps to the firewall ports that cover it", "[node][firewall]")
{
    // Pinned per kind, since the mapping is the whole decision: a kernel-chosen port mapped to
    // `Fixed` is a rule on port 0, which the firewall checks refuse and which would admit nothing.
    CHECK(FirewallPortKindOf(SurfacePortKind::Fixed) == FirewallPortKind::Fixed);
    CHECK(FirewallPortKindOf(SurfacePortKind::KernelChosen) == FirewallPortKind::Any);
}

// --- The install derives what the SERVICE will open, after it secured the state directory --------

namespace
{

/// A state directory as an install meets it: the record is HELD until the registration has
/// secured the directory (a 0.3.0-era directory other accounts could write in), or unreadable
/// whatever happens (a damaged record), or readable and absent from the start.
class ScriptedFormationReader final: public IKeptFormationReader
{
  public:
    /// What `Read` answers before the directory was secured.
    std::optional<std::string> heldUntilSecured;
    /// What `Read` answers whatever happens; wins over `heldUntilSecured`.
    std::optional<std::string> unreadable;
    /// Set by the registration, as `InstallService` secures the directory.
    bool secured { false };
    /// How many times the record was read.
    mutable int reads { 0 };

    /// @copydoc IKeptFormationReader::Read
    [[nodiscard]] std::expected<KeptFormation, std::string> Read() const override
    {
        ++reads;
        if (unreadable.has_value())
            return std::unexpected { *unreadable };
        if (heldUntilSecured.has_value() && !secured)
            return std::unexpected { *heldUntilSecured };
        // No record kept: the first start mints the solitary one.
        return KeptFormation {};
    }
};

/// The four rules a zero-config node's service opens.
[[nodiscard]] std::vector<std::string> ZeroConfigOpenings()
{
    return { "FastCacheCompileNode node tcp/6674",
             "FastCacheCompileNode raft tcp/6680",
             "FastCacheCompileNode discovery-beacon udp/6681",
             "FastCacheCompileNode discovery-reply udp/any" };
}

} // namespace

TEST_CASE("An install whose formation record was held opens what the service will, once it secured the directory",
          "[node][firewall][install]")
{
    // The 0.3.0 upgrade: the old MSI's %ProgramData%\fastcache-node inherits a grant that lets
    // other accounts write in it, so `main` HELD the record and left the configuration unshaped --
    // and an unshaped configuration runs no consensus, so rules derived from it open the node port
    // alone. The registration secures the directory; the rules are derived after it.
    // What the MSI registers: a worker with everything defaulted, which names no scheduler -- a
    // node that serves is refused one, and registers where its formation record says.
    NodeConfig unshaped;
    REQUIRE_FALSE(unshaped.formation.has_value());
    ScriptedFormationReader formation;
    formation.heldUntilSecured = "the state directory lets other accounts create entries in it";
    Testing::RecordingFirewall firewall;

    auto const result = InstallWithServiceFirewall(
        [&formation] {
            formation.secured = true;
            return ServiceControlResult { .outcome = ServiceControlOutcome::Created, .message = "installed" };
        },
        [] { return ServiceControlResult { .outcome = ServiceControlOutcome::Failed, .message = "not asked" }; },
        unshaped,
        Program,
        unshaped.serviceName,
        formation,
        &firewall);

    INFO(result.message);
    REQUIRE(result.ExitCode() == 0);
    auto const opened = firewall.NamesInGroup(FirewallGroupFor(unshaped.serviceName));
    REQUIRE(opened.has_value());
    auto names = Testing::Unwrap(opened);
    std::ranges::sort(names);
    auto expected = ZeroConfigOpenings();
    std::ranges::sort(expected);
    CHECK(names == expected);

    // The control: the same configuration's own rules, the derivation the install used to make.
    CHECK(Openings(NodeFirewallRules(unshaped, Program))
          == std::vector<std::string> { "FastCacheCompileNode node tcp/6674" });
}

TEST_CASE("An install whose formation record cannot be read after securing is refused, and opens nothing",
          "[node][firewall][install]")
{
    ScriptedFormationReader formation;
    formation.unreadable = "the formation record in this node's state directory cannot be read (truncated)";
    Testing::RecordingFirewall firewall;
    auto registrations = 0;
    auto removals = 0;

    auto const result = InstallWithServiceFirewall(
        [&registrations] {
            ++registrations;
            return ServiceControlResult { .outcome = ServiceControlOutcome::Created, .message = "installed" };
        },
        [&removals] {
            ++removals;
            return ServiceControlResult { .outcome = ServiceControlOutcome::Removed, .message = "removed" };
        },
        Installed(),
        Program,
        "FastCacheCompileNode",
        formation,
        &firewall);

    INFO(result.message);
    CHECK(registrations == 1);
    // A record that could not be read may be read at the next attempt: transient.
    CHECK(result.ExitCode() == 1);
    CHECK(result.message.contains("refusing the install"));
    CHECK(result.message.contains("cannot be read (truncated)"));
    // The registration this install created does not outlive the refusal.
    CHECK(removals == 1);
    CHECK(result.message.contains("was removed again"));
    CHECK(firewall.rules.empty());
}

TEST_CASE("An install refused after its registration removes the registration it created, and keeps one it re-applied",
          "[node][firewall][install]")
{
    // The MSI starts the node straight after the install, whatever the install answered, so a
    // registration THIS install created must not outlive the refusal. One it only re-applied is an
    // upgrade's: removing it would take away a service the operator had, so it stays, refusing to
    // start for the same reason, and the message says which happened.
    ScriptedFormationReader formation;
    formation.unreadable = "the formation record in this node's state directory cannot be read (truncated)";

    struct Removal
    {
        ServiceControlOutcome registeredAs;   ///< What the registration reported.
        ServiceControlOutcome removalAnswers; ///< What the removal reports, when it is asked.
        int removals;                         ///< How many times the removal must be asked.
        ServiceControlOutcome refusalLeaves;  ///< What the refused install reports.
        std::string_view says;                ///< What it tells the operator.
    };
    auto const cases = std::to_array<Removal>({
        { .registeredAs = ServiceControlOutcome::Created,
          .removalAnswers = ServiceControlOutcome::Removed,
          .removals = 1,
          .refusalLeaves = ServiceControlOutcome::Failed,
          .says = "this install had created was removed again" },
        { .registeredAs = ServiceControlOutcome::Created,
          .removalAnswers = ServiceControlOutcome::NotInstalled,
          .removals = 1,
          .refusalLeaves = ServiceControlOutcome::Failed,
          .says = "this install had created was removed again" },
        { .registeredAs = ServiceControlOutcome::Created,
          .removalAnswers = ServiceControlOutcome::Failed,
          .removals = 1,
          .refusalLeaves = ServiceControlOutcome::Failed,
          .says = "could NOT be removed" },
        { .registeredAs = ServiceControlOutcome::Reapplied,
          .removalAnswers = ServiceControlOutcome::Removed,
          .removals = 0,
          .refusalLeaves = ServiceControlOutcome::Failed,
          .says = "re-applied registration is left in place" },
    });
    for (auto const& removal: cases)
    {
        Testing::RecordingFirewall firewall;
        auto removals = 0;
        auto const result = InstallWithServiceFirewall(
            [&removal] { return ServiceControlResult { .outcome = removal.registeredAs, .message = "installed" }; },
            [&removal, &removals] {
                ++removals;
                return ServiceControlResult { .outcome = removal.removalAnswers, .message = "removal" };
            },
            Installed(),
            Program,
            "FastCacheCompileNode",
            formation,
            &firewall);
        INFO(result.message);
        CHECK(removals == removal.removals);
        CHECK(result.outcome == removal.refusalLeaves);
        CHECK(result.message.contains(removal.says));
        CHECK(firewall.rules.empty());
    }

    // WHAT DISTINGUISHES: above, the verdict is itself a failure, so a removal that failed cannot
    // be told apart from one that went through. A configuration the rules refuse is a DECISION, and
    // keeps that ending -- unless the removal failed, which leaves a registration nobody asked for.
    // A worker with its consensus port closed: an install refuses it (`WorkerConsensusClosed`), since
    // its worker would have nowhere to register at every boot.
    auto const refusedByRules = [] {
        NodeConfig cfg;
        cfg.raftListen.clear();
        return cfg;
    }();
    ScriptedFormationReader readable;
    auto const decided = std::to_array<Removal>({
        { .registeredAs = ServiceControlOutcome::Created,
          .removalAnswers = ServiceControlOutcome::Removed,
          .removals = 1,
          .refusalLeaves = ServiceControlOutcome::Declined,
          .says = "this install had created was removed again" },
        { .registeredAs = ServiceControlOutcome::Created,
          .removalAnswers = ServiceControlOutcome::Failed,
          .removals = 1,
          .refusalLeaves = ServiceControlOutcome::Failed,
          .says = "could NOT be removed" },
        { .registeredAs = ServiceControlOutcome::Reapplied,
          .removalAnswers = ServiceControlOutcome::Removed,
          .removals = 0,
          .refusalLeaves = ServiceControlOutcome::Declined,
          .says = "re-applied registration is left in place" },
    });
    for (auto const& removal: decided)
    {
        Testing::RecordingFirewall firewall;
        auto removals = 0;
        auto const result = InstallWithServiceFirewall(
            [&removal] { return ServiceControlResult { .outcome = removal.registeredAs, .message = "installed" }; },
            [&removal, &removals] {
                ++removals;
                return ServiceControlResult { .outcome = removal.removalAnswers, .message = "removal" };
            },
            refusedByRules,
            Program,
            "FastCacheCompileNode",
            readable,
            &firewall);
        INFO(result.message);
        CHECK(removals == removal.removals);
        CHECK(result.outcome == removal.refusalLeaves);
        CHECK(result.message.contains(removal.says));
        CHECK(result.message.contains(WorkerWithConsensusClosedRefusal));
        // One full stop between the rule's sentence and what the install adds, never two.
        CHECK_FALSE(result.message.contains(".."));
        CHECK(firewall.rules.empty());
    }
}

TEST_CASE("A failed registration reads no formation and opens nothing", "[node][firewall][install]")
{
    ScriptedFormationReader formation;
    Testing::RecordingFirewall firewall;

    auto const result = InstallWithServiceFirewall(
        [] { return ServiceControlResult { .outcome = ServiceControlOutcome::Failed, .message = "access denied" }; },
        [] { return ServiceControlResult { .outcome = ServiceControlOutcome::Failed, .message = "not asked" }; },
        Installed(),
        Program,
        "FastCacheCompileNode",
        formation,
        &firewall);

    CHECK(result.ExitCode() == 1);
    CHECK(result.message == "access denied");
    CHECK(formation.reads == 0);
    CHECK(firewall.rules.empty());
}

TEST_CASE("An install whose formation record was held is judged on the configuration the service starts with",
          "[node][firewall][install]")
{
    // A rule that fires only on the SHAPED configuration: a worker that binds the network by name
    // and advertises loopback tells every peer to dial itself -- refused on a node that admits
    // other machines, which every consensus node does. Unshaped (the record held at parse time),
    // the node runs no consensus, admits nobody, and the rule is silent: judged there, the install
    // would succeed and the start refuse.
    NodeConfig unshaped;
    unshaped.nodeListen = "0.0.0.0:6674";
    unshaped.nodeListenExplicit = true;
    unshaped.advertise = "127.0.0.1:6674";
    unshaped.advertiseExplicit = true;
    REQUIRE_FALSE(unshaped.formation.has_value());

    // The control, both ways round: the row fires on the shaped configuration and not on this one.
    constexpr std::string_view LoopbackAdvertise = "--advertise names loopback";
    CHECK_FALSE(NodeInstallRejection(unshaped).value_or("").contains(LoopbackAdvertise));
    CHECK(NodeInstallRejection(Testing::FirstStart(unshaped)).value_or("").contains(LoopbackAdvertise));

    ScriptedFormationReader formation;
    formation.heldUntilSecured = "the state directory lets other accounts create entries in it";
    Testing::RecordingFirewall firewall;
    auto const result = InstallWithServiceFirewall(
        [&formation] {
            formation.secured = true;
            return ServiceControlResult { .outcome = ServiceControlOutcome::Created, .message = "installed" };
        },
        [] { return ServiceControlResult { .outcome = ServiceControlOutcome::Removed, .message = "removed" }; },
        unshaped,
        Program,
        unshaped.serviceName,
        formation,
        &firewall);

    INFO(result.message);
    // A configuration the rules refuse is refused again: a decision.
    CHECK(result.ExitCode() == 2);
    CHECK(result.message.contains("refusing the install"));
    CHECK(result.message.contains(LoopbackAdvertise));
    CHECK(firewall.rules.empty());
}

TEST_CASE("An install reads the formation record in exactly the directory its registration secured",
          "[node][firewall][install]")
{
    // A `cluster_dir:` in the configuration FILE reaches the merged configuration and never the
    // registration, which carries only what was typed. The handover secures the registration's
    // directory (`RegisteredStateDirectory`), so that is the one directory the install may read:
    // a record in the file's directory sits where other accounts may still write, which is the
    // read the registration-first order exists to prevent. Here the file's directory holds a record
    // nobody can read, so an install that read it would refuse.
    Testing::ScratchDirectory const scratch { "install-state-dir" };
    // On Windows the machine-wide base is the probe's `ProgramData`, so the registration's
    // directory lands inside the scratch directory; on POSIX it is a fixed path.
    Testing::ScriptedConfigPathProbe const probe { { { "ProgramData", (scratch / "programdata").string() } },
                                                   Testing::ScriptedConfigPathProbe::Privilege::Privileged };
    NodeConfig registration;
    REQUIRE(registration.clusterDir.empty());
    auto const secured = RegisteredStateDirectory(registration, probe);
    REQUIRE(secured.has_value());
    auto const securedInScratch = Testing::Unwrap(secured).string().starts_with(scratch.Path().string());
    if (!securedInScratch && std::filesystem::exists(Testing::Unwrap(secured)))
        SKIP("this host keeps a machine-wide node state directory at " << Testing::Unwrap(secured).string()
                                                                       << ", which the case cannot stand in for");

    auto merged = registration;
    merged.clusterDir = scratch / "from-file";
    auto const recordIn = [](std::filesystem::path const& directory) {
        return (directory / std::string { Cluster::FormationRecordFileName }).string();
    };
    scratch.Write(std::format("from-file/{}", Cluster::FormationRecordFileName), "not a formation record");

    auto const install = [&](Testing::RecordingFirewall& firewall, std::vector<ServiceSpec>& registered) {
        return InstallNodeService(
            merged,
            registration,
            probe,
            Program,
            [&registered](ServiceSpec const& spec) {
                registered.push_back(spec);
                return ServiceControlResult { .outcome = ServiceControlOutcome::Created, .message = "installed" };
            },
            [](ServiceSpec const& /*spec*/) {
                return ServiceControlResult { .outcome = ServiceControlOutcome::Removed, .message = "removed" };
            },
            &firewall);
    };

    Testing::RecordingFirewall firewall;
    std::vector<ServiceSpec> registered;
    auto const result = install(firewall, registered);
    INFO(result.message);
    CHECK(result.ExitCode() == 0);
    CHECK_FALSE(result.message.contains(recordIn(merged.clusterDir)));
    REQUIRE(registered.size() == 1);
    // The directory the handover secures is the one the install read.
    CHECK(std::ranges::contains(registered.front().ownedPaths,
                                OwnedPath { .path = Testing::Unwrap(secured),
                                            .privacy = PathPrivacy::Private,
                                            .credentialFiles = { std::filesystem::path { NodeKeyFileName } } }));
    auto const opened = firewall.NamesInGroup(FirewallGroupFor(registration.serviceName));
    REQUIRE(opened.has_value());
    CHECK(std::ranges::contains(Testing::Unwrap(opened), std::string { "FastCacheCompileNode raft tcp/6680" }));

    // The other direction, where the case can write into the registration's directory: a record
    // there that cannot be read refuses the install, naming that record and not the file's.
    if (securedInScratch)
    {
        auto const relative =
            Testing::Unwrap(secured).lexically_relative(scratch.Path()) / std::string { Cluster::FormationRecordFileName };
        scratch.Write(relative.generic_string(), "not a formation record either");
        Testing::RecordingFirewall refusedFirewall;
        std::vector<ServiceSpec> refusedRegistered;
        auto const refused = install(refusedFirewall, refusedRegistered);
        INFO(refused.message);
        CHECK(refused.ExitCode() == 1);
        CHECK(refused.message.contains(recordIn(Testing::Unwrap(secured))));
        CHECK_FALSE(refused.message.contains(recordIn(merged.clusterDir)));
        CHECK(refusedFirewall.rules.empty());
    }
}

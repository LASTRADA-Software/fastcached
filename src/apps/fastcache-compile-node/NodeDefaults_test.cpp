// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/FileTrust.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;
using FastCache::Testing::ScriptedConfigPathProbe;
using FastCache::Testing::ScriptedHostNaming;
using FastCache::Testing::Unwrap;
using Privilege = ScriptedConfigPathProbe::Privilege;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Parse flags, without the program name, into a configuration.
/// @param args The flags.
/// @return The configuration, or the first parse error.
[[nodiscard]] std::expected<NodeConfig, ConfigError> Parse(std::vector<char const*> const& args)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg);
    if (!flow.has_value())
        return std::unexpected(flow.error());
    return cfg;
}

/// A host-naming lookup that answers what it was scripted to, at once, and records the bound it
/// was asked with -- so no case waits on the bound it is testing.
class ScriptedHostNamingLookup final: public IHostNamingLookup
{
  public:
    /// @param answer What `ReadWithin` returns; null stands for a lookup that ran out of time.
    explicit ScriptedHostNamingLookup(std::unique_ptr<IHostNaming> answer):
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::unique_ptr<IHostNaming> ReadWithin(std::chrono::milliseconds bound) override
    {
        _askedBound = bound;
        return std::move(_answer);
    }

    /// @return The bound the last `ReadWithin` was given, or nothing when it was never asked.
    [[nodiscard]] std::optional<std::chrono::milliseconds> AskedBound() const
    {
        return _askedBound;
    }

  private:
    std::unique_ptr<IHostNaming> _answer;
    std::optional<std::chrono::milliseconds> _askedBound;
};

/// The environment every platform's rows are asked of, with every base set -- so which row
/// answers is decided by the privilege and the service manager, never by a variable missing.
[[nodiscard]] std::map<std::string, std::string, std::less<>> EveryBase()
{
    return { { "ProgramData", "C:\\ProgramData" },
             { "LOCALAPPDATA", R"(C:\Users\dev\AppData\Local)" },
             { "XDG_STATE_HOME", "/home/dev/.state" },
             { "HOME", "/home/dev" } };
}

} // namespace

TEST_CASE("The default cluster dir is the platform's machine-wide state directory", "[node][formation][defaults]")
{
    auto const probe = ScriptedConfigPathProbe { EveryBase(), Privilege::Privileged };
    auto const directory = DefaultNodeClusterDirectory(probe);
    REQUIRE(directory.has_value());
    CHECK(Unwrap(directory).origin == StateDirectoryOrigin::MachineWide);
#if defined(_WIN32)
    CHECK(Unwrap(directory).path == std::filesystem::path { "C:\\ProgramData\\fastcache-node" });
#elif defined(__APPLE__)
    CHECK(Unwrap(directory).path == std::filesystem::path { "/Library/Application Support/fastcache-node" });
#else
    CHECK(Unwrap(directory).path == std::filesystem::path { "/var/lib/fastcache-node" });
#endif
}

TEST_CASE("An unprivileged node keeps its identity in the account's own state directory", "[node][formation][defaults]")
{
    // The machine-wide directory is the service's: a hand-started node cannot create it, and one
    // that could would share the service's identity -- two processes proving one key.
    auto const probe = ScriptedConfigPathProbe { EveryBase(), Privilege::Unprivileged };
    auto const directory = DefaultNodeClusterDirectory(probe);
    REQUIRE(directory.has_value());
    CHECK(Unwrap(directory).origin == StateDirectoryOrigin::PerUser);
#if defined(_WIN32)
    CHECK(Unwrap(directory).path == std::filesystem::path { "C:\\Users\\dev\\AppData\\Local\\fastcache-node" });
#else
    CHECK(Unwrap(directory).path == std::filesystem::path { "/home/dev/.state/fastcache-node" });

    // And without XDG_STATE_HOME, the basedir specification's own fallback.
    auto environment = EveryBase();
    environment.erase("XDG_STATE_HOME");
    auto const fallback = DefaultNodeClusterDirectory(ScriptedConfigPathProbe { environment, Privilege::Unprivileged });
    REQUIRE(fallback.has_value());
    CHECK(Unwrap(fallback).path == std::filesystem::path { "/home/dev/.local/state/fastcache-node" });
#endif
}

TEST_CASE("A service manager's state directory wins, and only one named for this node", "[node][formation][defaults]")
{
    // The shipped systemd unit runs as an UNPRIVILEGED account whose home is `/`, so without
    // this row the machine's service would take the per-user default. A child inherits the
    // variable, so an entry named for some other service is not this node's.
    auto environment = EveryBase();
    environment.emplace(ServiceStateDirectoryVariable, "/var/lib/other:/var/lib/fastcache-node");
    auto const handedOver = DefaultNodeClusterDirectory(ScriptedConfigPathProbe { environment, Privilege::Unprivileged });
    REQUIRE(handedOver.has_value());

    auto foreign = EveryBase();
    foreign.emplace(ServiceStateDirectoryVariable, "/var/lib/github-runner");
    auto const inherited = DefaultNodeClusterDirectory(ScriptedConfigPathProbe { foreign, Privilege::Unprivileged });
    REQUIRE(inherited.has_value());
    CHECK(Unwrap(inherited).origin == StateDirectoryOrigin::PerUser);
#if defined(_WIN32)
    // No such convention on Windows: a service's token is privileged by itself.
    CHECK(Unwrap(handedOver).origin == StateDirectoryOrigin::PerUser);
#else
    CHECK(Unwrap(handedOver).origin == StateDirectoryOrigin::ServiceManager);
    CHECK(Unwrap(handedOver).path == std::filesystem::path { "/var/lib/fastcache-node" });
#endif
}

TEST_CASE("A default cluster dir whose base the environment does not name is absent, never relative",
          "[node][formation][defaults]")
{
    // An unset base would otherwise make the suffix relative to the working directory, which
    // for a service is C:\Windows\System32 -- the node would mint its identity there.
    auto const unsetPrivileged = ScriptedConfigPathProbe { {}, Privilege::Privileged };
    auto const emptyPrivileged = ScriptedConfigPathProbe { { { "ProgramData", "" } }, Privilege::Privileged };
    auto const unsetUser = ScriptedConfigPathProbe { {}, Privilege::Unprivileged };
    auto const emptyUser = ScriptedConfigPathProbe { { { "HOME", "" }, { "LOCALAPPDATA", "" } }, Privilege::Unprivileged };
#if defined(_WIN32)
    CHECK_FALSE(DefaultNodeClusterDirectory(unsetPrivileged).has_value());
    CHECK_FALSE(DefaultNodeClusterDirectory(emptyPrivileged).has_value());
#else
    // The POSIX machine-wide directory needs no environment.
    CHECK(DefaultNodeClusterDirectory(unsetPrivileged).has_value());
    CHECK(DefaultNodeClusterDirectory(emptyPrivileged).has_value());
#endif
    CHECK_FALSE(DefaultNodeClusterDirectory(unsetUser).has_value());
    CHECK_FALSE(DefaultNodeClusterDirectory(emptyUser).has_value());

    // A RELATIVE base is skipped as an unset one is -- the XDG specification calls it invalid --
    // and the next row answers, never a directory under the working directory.
#if defined(_WIN32)
    auto const relativeUser =
        ScriptedConfigPathProbe { { { "LOCALAPPDATA", R"(AppData\Local)" } }, Privilege::Unprivileged };
    auto const relativeMachine = ScriptedConfigPathProbe { { { "ProgramData", "ProgramData" } }, Privilege::Privileged };
    CHECK_FALSE(DefaultNodeClusterDirectory(relativeUser).has_value());
    CHECK_FALSE(DefaultNodeClusterDirectory(relativeMachine).has_value());
    CHECK_FALSE(MachineWideNodeClusterDirectory(relativeMachine).has_value());
#else
    auto const relativeState =
        ScriptedConfigPathProbe { { { "XDG_STATE_HOME", ".state" }, { "HOME", "/home/dev" } }, Privilege::Unprivileged };
    auto const fallback = DefaultNodeClusterDirectory(relativeState);
    REQUIRE(fallback.has_value());
    CHECK(Unwrap(fallback).path == std::filesystem::path { "/home/dev/.local/state/fastcache-node" });
    auto const relativeHome =
        ScriptedConfigPathProbe { { { "XDG_STATE_HOME", ".state" }, { "HOME", "dev" } }, Privilege::Unprivileged };
    CHECK_FALSE(DefaultNodeClusterDirectory(relativeHome).has_value());
#endif
}

TEST_CASE("The state directory is named with why it is that one, and asking before it resolves is a precondition",
          "[node][formation][defaults]")
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    CHECK_FALSE(ChosenStateDirectory(cfg).has_value());
    CHECK(NodeKeyPath(cfg).empty());
    CHECK_THROWS_AS(NodeStateDirectory(cfg), std::logic_error);
    CHECK(DescribeNodeStateDirectory(cfg).starts_with("not resolved"));

    ApplyNodeStateDirectory(cfg, ScriptedConfigPathProbe { EveryBase(), Privilege::Unprivileged });
    REQUIRE(ChosenStateDirectory(cfg).has_value());
    CHECK(DescribeNodeStateDirectory(cfg).contains(Unwrap(ChosenStateDirectory(cfg)).path.string()));
    CHECK(DescribeNodeStateDirectory(cfg).contains(DescribeStateDirectoryOrigin(StateDirectoryOrigin::PerUser)));

    // --print-identity says it too, on a line of its own: unelevated, the identity it prints is
    // the account's, which the service never runs as.
    auto const identityLine = DescribeIdentityOrigin(cfg);
    CHECK(identityLine.starts_with("state-directory "));
    CHECK(identityLine.ends_with("\n"));
    CHECK(identityLine.contains(Unwrap(ChosenStateDirectory(cfg)).path.string()));
    CHECK(identityLine.contains("per-user"));

    cfg.clusterDir = "cluster";
    CHECK(Unwrap(ChosenStateDirectory(cfg)).origin == StateDirectoryOrigin::Named);
    CHECK(DescribeIdentityOrigin(cfg).contains("named by --cluster-dir"));
    CHECK(NodeStateDirectory(cfg) == std::filesystem::path { "cluster" });
    CHECK(DescribeNodeStateDirectory(cfg).contains(DescribeStateDirectoryOrigin(StateDirectoryOrigin::Named)));

    // Every origin has words of its own, so no two read alike on `--print-surfaces`.
    for (auto const origin: Enumerators<StateDirectoryOrigin>())
        for (auto const other: Enumerators<StateDirectoryOrigin>())
            if (origin != other)
                CHECK(DescribeStateDirectoryOrigin(origin) != DescribeStateDirectoryOrigin(other));

    // And the worksheet prints it.
    CHECK(RenderSurfaces(cfg).contains(DescribeNodeStateDirectory(cfg)));
}

TEST_CASE("A state directory the start resolved is a key route though nobody typed --cluster-dir",
          "[node][formation][defaults]")
{
    // The worker 3d040a99b was about: no consensus, no --voter-key, no --cluster-dir -- and an
    // enrollment that saved its roster into the DEFAULT state directory, which `NodeRoster::Build`
    // reads back through `ChosenStateDirectory`. Asked before that read, the fail-closed answer
    // has to count that directory as one that may hold a roster, exactly as a typed one does.
    auto cfg = Testing::FirstStart(Unwrap(Parse({ "--listen-raft=" })));
    REQUIRE_FALSE(RunsConsensus(cfg));
    REQUIRE(cfg.clusterDir.empty());

    // The control: nothing resolved, so nothing could hold a roster yet.
    CHECK_FALSE(AdmitsByKey(cfg));

    ApplyNodeStateDirectory(cfg, ScriptedConfigPathProbe { EveryBase(), Privilege::Unprivileged });
    REQUIRE(ChosenStateDirectory(cfg).has_value());
    REQUIRE(Unwrap(ChosenStateDirectory(cfg)).origin != StateDirectoryOrigin::Named);
    CHECK(AdmitsByKey(cfg));
}

TEST_CASE("Only an invocation that writes into the state directory is refused for having none",
          "[node][formation][defaults]")
{
    // No directory resolved -- `env -i`, a service with no ProgramData. A start writes its identity
    // there, and so do an install (the id) and --print-identity (the key); the verbs that answer
    // and exit write nothing, and an uninstall is the recovery reached for when things are wrong.
    auto const refusedFor = [](auto edit) {
        auto cfg = Testing::FirstStart(NodeConfig {});
        edit(cfg);
        return StateDirectoryRefusal(cfg);
    };
    auto const refused = std::optional { NoStateDirectoryRefusal };

    CHECK(refusedFor([](NodeConfig&) {}) == refused);
    CHECK(refusedFor([](NodeConfig& c) { c.installService = true; }) == refused);
    CHECK(refusedFor([](NodeConfig& c) { c.printIdentity = true; }) == refused);

    CHECK_FALSE(refusedFor([](NodeConfig& c) { c.uninstallService = true; }).has_value());
    CHECK_FALSE(refusedFor([](NodeConfig& c) { c.printSurfaces = true; }).has_value());
    CHECK_FALSE(refusedFor([](NodeConfig& c) { c.cluster.action = ClusterAction::Status; }).has_value());

    // And a named directory is a directory, whatever the platform could derive.
    CHECK_FALSE(refusedFor([](NodeConfig& c) { c.clusterDir = "state"; }).has_value());
}

TEST_CASE("Every node holds an identity key in its cluster dir even with no flags", "[node][formation][defaults]")
{
    // Lane 2b's admission needs a proven key on EVERY node, and a node with no --cluster-dir now
    // has the platform default rather than no directory. A path only: nothing here writes.
    auto cfg = Testing::FirstStart(NodeConfig {});
    ApplyNodeStateDirectory(cfg, ScriptedConfigPathProbe { EveryBase(), Privilege::Privileged });
    auto const path = NodeKeyPath(cfg);
    REQUIRE_FALSE(path.empty());
    CHECK(path.filename() == NodeKeyFileName);
    CHECK(path.parent_path() == NodeStateDirectory(cfg));
}

TEST_CASE("A node with no flags advertises its FQDN on the node port and names itself to peers by it",
          "[node][formation][defaults]")
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    ApplyHostNaming(cfg, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
    CHECK(AdvertisedEndpoint(cfg) == "laptop.corp.example:6674");
    CHECK(RaftSelfEndpoint(cfg) == "laptop.corp.example:6680");
    CHECK(Unwrap(ConsensusDialAddressOf(cfg)) == "laptop.corp.example:6680");
    REQUIRE(cfg.hostNames.has_value());
    CHECK(Unwrap(cfg.hostNames).dnsSuffix == "corp.example");
}

TEST_CASE("A typed --advertise and --raft-self win over the FQDN", "[node][formation][defaults]")
{
    auto cfg = Unwrap(Parse({ "--advertise=10.1.2.3:7000", "--raft-self=10.1.2.3" }));
    ApplyHostNaming(cfg, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
    CHECK(AdvertisedEndpoint(cfg) == "10.1.2.3:7000");
    CHECK(RaftSelfEndpoint(cfg) == "10.1.2.3:6680");
}

TEST_CASE("Only a wildcard bind is advertised under the machine's name", "[node][formation][defaults]")
{
    // A loopback bind is a single-machine fleet and an address is what the operator bound; both
    // are advertised as bound, and only the wildcard -- wrong on every other machine -- is not.
    auto loopback = Unwrap(Parse({ "--listen-node=127.0.0.1:6674" }));
    ApplyHostNaming(loopback, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
    CHECK(AdvertisedEndpoint(loopback) == "127.0.0.1:6674");

    auto bound = Unwrap(Parse({ "--listen-node=10.0.0.5:6674" }));
    ApplyHostNaming(bound, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
    CHECK(AdvertisedEndpoint(bound) == "10.0.0.5:6674");
}

TEST_CASE("Only a wildcard consensus bind names itself by the machine's name", "[node][formation][defaults]")
{
    // The consensus address follows the advertised one's rule. A loopback --listen-raft named by
    // the FQDN would send every peer to an interface the bind never answers.
    auto const named = [](std::vector<char const*> const& argv) {
        auto cfg = Unwrap(Parse(argv));
        cfg.slots = 0;
        ApplyHostNaming(cfg, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
        return cfg;
    };

    CHECK(RaftSelfEndpoint(named({})) == "laptop.corp.example:6680");
    CHECK(RaftSelfEndpoint(named({ "--listen-raft=10.0.0.5:6680" })) == "10.0.0.5:6680");

    // A loopback bind names itself as bound: a cluster that never leaves this machine, started.
    auto const loopback = named({ "--listen-raft=127.0.0.1:6680" });
    CHECK(RaftSelfEndpoint(loopback) == "127.0.0.1:6680");
    CHECK_FALSE(ConsensusPastALoopbackBind(loopback));
    CHECK_FALSE(StartupPolicyRejection(loopback).has_value());

    // And a stated address off this machine contradicts that bind, and is refused by name.
    auto const contradicted = named({ "--listen-raft=127.0.0.1:6680", "--raft-self=laptop.corp.example" });
    CHECK(ConsensusPastALoopbackBind(contradicted));
    CHECK(StartupPolicyRejection(contradicted) == std::optional { std::string { ConsensusPastALoopbackBindRefusal } });

    // The control: stating loopback agrees with the bind.
    CHECK_FALSE(StartupPolicyRejection(named({ "--listen-raft=127.0.0.1:6680", "--raft-self=127.0.0.1" })).has_value());
}

TEST_CASE("Before the names resolve the advertised and dialled addresses are awaited, never refused",
          "[node][formation][defaults]")
{
    // A parse or an install has not resolved the names, and a no-flag --install-service must be
    // accepted: the rules refuse only a RESOLVED empty name. `--slots=0` so the worker's own
    // rows -- `--scheduler` is still required of a worker -- do not answer first.
    auto const pending = Unwrap(Parse({ "--slots=0" }));
    CHECK(AdvertisedNameAwaited(pending));
    CHECK(RaftSelfEndpoint(pending).empty());
    auto const awaited = ConsensusDialAddressOf(pending);
    REQUIRE_FALSE(awaited.has_value());
    CHECK(awaited.error() == ConsensusDialGap::AwaitingHostName);
    CHECK_FALSE(StartupPolicyRejection(pending).has_value());

    // A name that resolved to nothing is a node naming itself neither way, and is refused.
    auto nameless = pending;
    ApplyHostNames(nameless, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = {} });
    CHECK_FALSE(AdvertisedNameAwaited(nameless));
    auto const unstated = ConsensusDialAddressOf(nameless);
    REQUIRE_FALSE(unstated.has_value());
    CHECK(unstated.error() == ConsensusDialGap::Unstated);
    CHECK(StartupPolicyRejection(nameless) == std::optional<std::string> { ConsensusNamesNoDialAddressRefusal });

    // And a resolved one passes.
    auto named = pending;
    ApplyHostNaming(named, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
    CHECK_FALSE(AdvertisedNameAwaited(named));
    CHECK_FALSE(StartupPolicyRejection(named).has_value());
}

TEST_CASE("The start takes a resolved name as given", "[node][formation][defaults][naming]")
{
    ScriptedHostNamingLookup lookup { std::make_unique<ScriptedHostNaming>("laptop.corp.example", "corp.example") };
    auto const resolved = ResolveHostNames(lookup, "laptop", HostNamingBound);
    CHECK(lookup.AskedBound() == std::optional { HostNamingBound });
    CHECK(resolved.outcome == HostNamingOutcome::Resolved);
    CHECK(resolved.names == NodeHostNames { .fqdn = "laptop.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
    CHECK_FALSE(HostNamingWarning(resolved).has_value());
}

TEST_CASE("A lookup that runs out of time falls back to the bare host name and says so",
          "[node][formation][defaults][naming]")
{
    ScriptedHostNamingLookup lookup { nullptr };
    auto const resolved = ResolveHostNames(lookup, "laptop", 250ms);
    CHECK(resolved.outcome == HostNamingOutcome::TimedOut);
    CHECK(resolved.names == NodeHostNames { .fqdn = "laptop", .dnsSuffix = {}, .withheld = {} });
    auto const warning = HostNamingWarning(resolved);
    REQUIRE(warning.has_value());
    CHECK(Unwrap(warning).contains("250 ms"));
    CHECK(Unwrap(warning).contains("'laptop'"));
    CHECK(Unwrap(warning).contains("--advertise"));
}

TEST_CASE("A placeholder domain falls back to the bare host name and names what was refused",
          "[node][formation][defaults][naming]")
{
    ScriptedHostNamingLookup lookup { std::make_unique<ScriptedHostNaming>("laptop", "", "laptop.localdomain") };
    auto const resolved = ResolveHostNames(lookup, "laptop", HostNamingBound);
    CHECK(resolved.outcome == HostNamingOutcome::Declined);
    CHECK(resolved.names.fqdn == "laptop");
    auto const warning = HostNamingWarning(resolved);
    REQUIRE(warning.has_value());
    CHECK(Unwrap(warning).contains("'laptop.localdomain'"));
    CHECK(Unwrap(warning).contains("'laptop'"));
}

TEST_CASE("A name that reaches only this machine is withheld, whatever produced it", "[node][formation][defaults][naming]")
{
    // "A confident wrong signal is worse than a vague right one": a peer told to dial `localhost`
    // reaches ITSELF, with no error at either end. So such a name is offered to nobody, and it is
    // caught after every way the start comes by a name -- the platform's answer, the placeholder
    // rule's fallback, and the bare host name a lookup that ran out of time stands in with.
    auto const expectWithheld = [](ResolvedHostNames const& resolved, std::string_view name, std::string_view said) {
        CHECK(resolved.outcome == HostNamingOutcome::ReachesOnlyThisMachine);
        CHECK(resolved.names == NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = std::string { name } });
        auto const warning = HostNamingWarning(resolved);
        REQUIRE(warning.has_value());
        CHECK(Unwrap(warning).contains(std::format("'{}'", said)));
        CHECK(Unwrap(warning).contains("reaches only this machine"));
        CHECK(Unwrap(warning).contains("--raft-self"));
        // Not the bare-name sentence, which would be false here: this name resolves EVERYWHERE.
        CHECK_FALSE(Unwrap(warning).contains("DNS search list"));
    };

    SECTION("RHEL's unset default, through the placeholder rule's own fallback")
    {
        auto const judged = JudgeHostNaming("localhost.localdomain", "localdomain");
        ScriptedHostNamingLookup lookup { std::make_unique<ScriptedHostNaming>(
            judged.fqdn, judged.suffix, judged.declined) };
        expectWithheld(ResolveHostNames(lookup, "localhost", HostNamingBound), "localhost", "localhost.localdomain");
    }
    SECTION("a container whose host name is localhost")
    {
        ScriptedHostNamingLookup lookup { std::make_unique<ScriptedHostNaming>("localhost", "") };
        expectWithheld(ResolveHostNames(lookup, "localhost", HostNamingBound), "localhost", "localhost");
    }
    SECTION("the bare host name a timed-out lookup falls back to")
    {
        ScriptedHostNamingLookup lookup { nullptr };
        expectWithheld(ResolveHostNames(lookup, "LOCALHOST", 250ms), "LOCALHOST", "LOCALHOST");
    }
    SECTION("a name under .localhost, and a loopback address")
    {
        ScriptedHostNamingLookup under { std::make_unique<ScriptedHostNaming>("build.localhost", "localhost") };
        expectWithheld(ResolveHostNames(under, "build", HostNamingBound), "build.localhost", "build.localhost");
        ScriptedHostNamingLookup literal { std::make_unique<ScriptedHostNaming>("127.0.1.1", "") };
        expectWithheld(ResolveHostNames(literal, "box", HostNamingBound), "127.0.1.1", "127.0.1.1");
    }
}

TEST_CASE("A withheld name is offered to no peer, and the node serves this machine alone", "[node][formation][defaults]")
{
    auto const withheld = NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" };

    // A node with its defaults and no worker: consensus and discovery are on by default. Consensus
    // is confined to loopback -- a fleet of its own, so nothing tells a peer to dial `localhost` --
    // and discovery stands down, while the node still starts. The one address it does offer is
    // loopback, to a scheduler on this machine, which reaches it there.
    auto local = Testing::FirstStart(NodeConfig {});
    local.slots = 0;
    ApplyHostNames(local, withheld);
    CHECK(AdvertisedNameWithheld(local));
    CHECK(AdvertisedEndpoint(local) == "127.0.0.1:6674");
    // The readiness line names where the node BINDS, and what it advertises.
    CHECK(DescribeListeningEndpoint(local, false) == "0.0.0.0:6674");
    CHECK(DescribeListeningEndpoint(local, true) == "the socket its supervisor handed it");
    CHECK(DescribeAdvertisedEndpoint(local) == "127.0.0.1:6674");
    CHECK(ConsensusNameWithheld(local));
    CHECK(ConsensusConfinedToThisMachine(local));
    CHECK(RunsConsensus(local));
    auto const raft = RowFor(NodeSurface::Raft).Resolve(local);
    REQUIRE(raft.size() == 1);
    CHECK(raft.front().host == ThisMachineLoopbackHost);
    CHECK(RaftSelfEndpoint(local) == FormatHostPort(ThisMachineLoopbackHost, raft.front().port));
    CHECK(ConsensusAddressReachesOnlyThisMachine(local));
    CHECK(RowFor(NodeSurface::Discovery).Resolve(local).empty());
    CHECK_FALSE(StartupPolicyRejection(local).has_value());
    auto const detail = NameReachesOnlyThisMachineInUse(local);
    REQUIRE(detail.has_value());
    CHECK(Unwrap(detail).contains("'localhost'"));
    CHECK(Unwrap(detail).contains("advertised at loopback"));
    CHECK(Unwrap(detail).contains("a fleet of its own on loopback and can neither form nor join a fleet"));
    CHECK(Unwrap(detail).contains("fix this machine's DNS, or give --raft-self and --advertise"));

    // The control: the same node with a name peers can dial runs consensus and advertises it.
    auto named = Testing::FirstStart(NodeConfig {});
    named.slots = 0;
    ApplyHostNames(named, NodeHostNames { .fqdn = "box.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
    CHECK(RunsConsensus(named));
    CHECK(AdvertisedEndpoint(named) == "box.corp.example:6674");
    CHECK_FALSE(NameReachesOnlyThisMachineInUse(named).has_value());

    // A feature the operator ASKED for that would have to offer the name is refused by NAME --
    // never the downstream symptom (`--discovery needs --listen-raft`) of standing down.
    SECTION("a worker")
    {
        // A learner, registering with its fleet's scheduler on another machine as its formation
        // record says.
        auto worker = Testing::FirstStart(NodeConfig {});
        REQUIRE(worker.formation.has_value());
        if (worker.formation.has_value())
        {
            worker.formation->mode = Cluster::NodeMode::Learner;
            worker.formation->foundedHere = false;
            worker.formation->fleetSchedulers = { "sched.corp.example:6675" };
        }
        worker.toolchains = { "/usr/bin/g++" };
        ApplyHostNames(worker, withheld);
        CHECK(StartupPolicyRejection(worker) == std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
        // Nothing is advertised to a scheduler elsewhere, and a line that names it says why.
        CHECK(AdvertisedEndpoint(worker).empty());
        CHECK(DescribeAdvertisedEndpoint(worker) == "withheld (localhost)");
        // Unless it names its address itself.
        worker.advertise = "worker-01.corp.example:6674";
        CHECK(AdvertisedEndpoint(worker) == "worker-01.corp.example:6674");
        CHECK(StartupPolicyRejection(worker) != std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
    }
    SECTION("a typed --listen-raft")
    {
        auto consensus = local;
        consensus.raftListenExplicit = true;
        CHECK(RunsConsensus(consensus));
        CHECK(StartupPolicyRejection(consensus)
              == std::optional { std::string { ConsensusNameReachesOnlyThisMachineRefusal } });
    }
    SECTION("a typed --raft-self is the operator's word, loopback included")
    {
        // 127.0.0.1 is how a one-machine cluster says no other machine will ever dial it.
        auto stated = local;
        stated.raftSelf = "127.0.0.1";
        CHECK_FALSE(ConsensusNameWithheld(stated));
        CHECK(RunsConsensus(stated));
        CHECK(RaftSelfEndpoint(stated) == "127.0.0.1:6680");
        CHECK_FALSE(StartupPolicyRejection(stated).has_value());
        // Only the advertise half is still withheld, and the condition says exactly that.
        auto const left = NameReachesOnlyThisMachineInUse(stated);
        REQUIRE(left.has_value());
        CHECK(Unwrap(left).contains("advertised at loopback"));
        CHECK_FALSE(Unwrap(left).contains("consensus"));
    }
    SECTION("a parse or an install, which has not resolved the name, is refused for nothing it has not seen")
    {
        auto installing = Testing::FirstStart(NodeConfig {});
        installing.slots = 0;
        installing.raftSelf.clear();
        CHECK_FALSE(ConsensusNameWithheld(installing));
        CHECK(StartupPolicyRejection(installing)
              != std::optional { std::string { ConsensusNameReachesOnlyThisMachineRefusal } });
    }
}

TEST_CASE("Every setting that needs peers to reach consensus is refused by name on a withheld name",
          "[node][formation][defaults]")
{
    // Each of these meets a DOWNSTREAM refusal once consensus stands down -- `--discovery` "needs
    // --listen-raft" -- which sends the operator to add a flag and only then names the cause. So the named refusal keys on
    // the whole table, and a row with no case here, or missing from the refusal's text, fails.
    struct Choice
    {
        std::string_view setting;
        void (*choose)(NodeConfig&);
    };
    constexpr auto choices = std::to_array<Choice>({
        { .setting = "--listen-raft", .choose = [](NodeConfig& c) { c.raftListenExplicit = true; } },
        { .setting = "--discovery", .choose = [](NodeConfig& c) { c.discoveryAddressExplicit = true; } },
        { .setting = "recorded mode",
          .choose =
              [](NodeConfig& c) {
                  if (c.formation.has_value())
                      c.formation->mode = Cluster::NodeMode::Voter;
              } },
    });

    auto local = Testing::FirstStart(NodeConfig {});
    local.slots = 0;
    ApplyHostNames(local, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" });
    REQUIRE_FALSE(StartupPolicyRejection(local).has_value());
    auto const named = std::optional { std::string { ConsensusNameReachesOnlyThisMachineRefusal } };

    for (auto const& row: ConsensusPeerAsks())
    {
        CAPTURE(row.setting);
        CHECK(ConsensusNameReachesOnlyThisMachineRefusal.contains(row.setting));
        CHECK_FALSE(row.asks(local));
        // Scanned for by VALUE and named by no iterator: `readability-qualified-auto`'s fix for
        // one does not compile on MSVC.
        Choice const* choice = nullptr;
        for (auto const& each: choices)
            if (each.setting == row.setting)
                choice = &each;
        REQUIRE(choice != nullptr);

        auto asked = local;
        choice->choose(asked);
        CHECK(row.asks(asked));
        CHECK(StartupPolicyRejection(asked) == named);

        // The control: a name peers can dial is never refused for this.
        ApplyHostNames(asked, NodeHostNames { .fqdn = "box.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
        CHECK(StartupPolicyRejection(asked) != named);
    }
    // And the other direction: a choice whose row went missing would not be walked above.
    for (auto const& choice: choices)
    {
        CAPTURE(choice.setting);
        CHECK(std::ranges::contains(ConsensusPeerAsks(), choice.setting, &ConsensusPeerAsk::setting));
    }
}

TEST_CASE("The production lookup gives up at its bound and survives the late answer", "[node][formation][defaults][naming]")
{
    // A reader that blocks until released stands for a resolver that has not answered. The bound
    // is short because it is what is under test; the reader is released after, and its late
    // write lands in state it co-owns.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable opened;
        bool open { false };
    };
    auto gate = std::make_shared<Gate>();

    // The late answer records its own end, so the case can SEE it land: written into the handover
    // the reader co-owns after the caller gave up, then freed by that handover's last owner --
    // not leaked, and not written anywhere the caller already let go of.
    struct LateNaming final: IHostNaming
    {
        explicit LateNaming(std::shared_ptr<std::atomic<bool>> freed):
            _freed { std::move(freed) }
        {
        }
        LateNaming(LateNaming const&) = delete;
        LateNaming(LateNaming&&) = delete;
        LateNaming& operator=(LateNaming const&) = delete;
        LateNaming& operator=(LateNaming&&) = delete;
        ~LateNaming() override
        {
            _freed->store(true);
        }
        [[nodiscard]] std::string FullyQualifiedName() const override
        {
            return "late.example";
        }
        [[nodiscard]] std::string PrimaryDnsSuffix() const override
        {
            return "example";
        }
        [[nodiscard]] std::string DeclinedName() const override
        {
            return {};
        }

      private:
        std::shared_ptr<std::atomic<bool>> _freed;
    };
    auto const freed = std::make_shared<std::atomic<bool>>(false);
    auto const returned = std::make_shared<std::atomic<bool>>(false);

    {
        // Destroyed before the reader is released: what the late read runs on must be the
        // thread's OWN, never the lookup object's -- nor anything else the caller lets go of.
        ThreadedHostNamingLookup slow { [gate, freed, returned] {
            auto lock = std::unique_lock { gate->mutex };
            gate->opened.wait(lock, [&gate] { return gate->open; });
            returned->store(true);
            return std::unique_ptr<IHostNaming> { std::make_unique<LateNaming>(freed) };
        } };
        CHECK(slow.ReadWithin(20ms) == nullptr);
    }
    CHECK_FALSE(returned->load());
    {
        auto const lock = std::scoped_lock { gate->mutex };
        gate->open = true;
    }
    gate->opened.notify_all();
    CHECK(Testing::WaitUntil(
        "the late answer to land in the handover and be freed by its last owner",
        [&freed] { return freed->load(); },
        [&returned] { return std::format("reader returned: {}", returned->load()); }));

    ThreadedHostNamingLookup fast { [] {
        return std::unique_ptr<IHostNaming> { std::make_unique<ScriptedHostNaming>("fast.example", "example") };
    } };
    auto const answer = fast.ReadWithin(HostNamingBound);
    REQUIRE(answer != nullptr);
    CHECK(answer->FullyQualifiedName() == "fast.example");
}

TEST_CASE("unqualified-host-name is raised while peers are told to dial a bare name, and clears when they are not",
          "[node][formation][defaults][conditions]")
{
    auto bare = Testing::FirstStart(NodeConfig {});
    ApplyHostNames(bare, NodeHostNames { .fqdn = "laptop", .dnsSuffix = {}, .withheld = {} });
    auto const detail = UnqualifiedHostNameInUse(bare);
    REQUIRE(detail.has_value());
    CHECK(Unwrap(detail).contains("'laptop'"));
    CHECK(Unwrap(detail).contains("--advertise and --raft-self"));

    NodeConditions conditions;
    EvaluateHostNameCondition(conditions, bare);
    CHECK(conditions.StateOf(NodeCondition::UnqualifiedHostName) == Wire::ConditionState::Raised);

    // A reload typing --advertise leaves only the consensus half, and naming both clears it: the
    // row is Live, and asked again of the configuration in force.
    auto advertised = bare;
    advertised.advertise = "10.0.0.5:6674";
    CHECK(Unwrap(UnqualifiedHostNameInUse(advertised)).contains("in place of --raft-self:"));
    auto named = advertised;
    named.raftSelf = "10.0.0.5";
    CHECK_FALSE(UnqualifiedHostNameInUse(named).has_value());
    EvaluateHostNameCondition(conditions, named);
    CHECK(conditions.StateOf(NodeCondition::UnqualifiedHostName) == Wire::ConditionState::Clear);

    // A qualified name, and names not resolved yet, are not a bare name in use.
    auto qualified = Testing::FirstStart(NodeConfig {});
    ApplyHostNaming(qualified, ScriptedHostNaming { "laptop.corp.example", "corp.example" });
    CHECK_FALSE(UnqualifiedHostNameInUse(qualified).has_value());
    CHECK_FALSE(UnqualifiedHostNameInUse(Testing::FirstStart(NodeConfig {})).has_value());

    // --advertise is reloadable, so the remedy does not send an operator to restart for it.
    auto const remedy = RowFor(NodeCondition::UnqualifiedHostName).remedy;
    CHECK(remedy.contains("--advertise (advertise in the configuration file; a reload applies it)"));
    CHECK(remedy.contains("--raft-self (raft_self; a restart applies it)"));
}

TEST_CASE("host-name-reaches-only-this-machine is raised while a withheld name stood something down",
          "[node][formation][defaults][conditions]")
{
    // Its own row rather than the bare-name one, named for what was observed: the remedy differs
    // (the name resolves EVERYWHERE, to the wrong machine), and so does what the node did about it.
    auto local = Testing::FirstStart(NodeConfig {});
    local.slots = 0;
    ApplyHostNames(local, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" });

    NodeConditions conditions;
    EvaluateHostNameCondition(conditions, local);
    CHECK(conditions.StateOf(NodeCondition::HostNameReachesOnlyThisMachine) == Wire::ConditionState::Raised);
    CHECK(conditions.StateOf(NodeCondition::UnqualifiedHostName) == Wire::ConditionState::Clear);
    auto const rows = conditions.Snapshot();
    auto const row =
        std::ranges::find(rows, RowFor(NodeCondition::HostNameReachesOnlyThisMachine).id, &Wire::NodeConditionFields::id);
    REQUIRE(row != rows.end());
    CHECK(row->detail.contains("'localhost'"));
    CHECK(row->persistence == "live");

    // Live: once the operator names both addresses nothing is withheld any more, and it clears.
    auto stated = local;
    stated.raftSelf = "10.0.0.5";
    stated.advertise = "10.0.0.5:6674";
    EvaluateHostNameCondition(conditions, stated);
    CHECK(conditions.StateOf(NodeCondition::HostNameReachesOnlyThisMachine) == Wire::ConditionState::Clear);

    // And the remedy says which half a reload reaches.
    auto const remedy = RowFor(NodeCondition::HostNameReachesOnlyThisMachine).remedy;
    CHECK(remedy.contains("--advertise (advertise; a reload applies it)"));
    CHECK(remedy.contains("--raft-self (raft_self; a restart applies it)"));
    CHECK(remedy.contains("Fix this machine's DNS"));
}

TEST_CASE("A service registration owns the machine-wide state directory and hands it to a POSIX daemon",
          "[node][formation][defaults][service]")
{
    // OWNED on every platform, so the install creates it for the service's account and secures
    // it: a service minting a voter's key into %ProgramData%'s inherited access list would leave
    // it readable by every local user. And HANDED OVER where the service runs unprivileged: the
    // launchd daemon would otherwise take the per-user default. As systemd's own variable, never
    // as --cluster-dir, which would outrank the configuration file's cluster_dir for the life of
    // the registration.
    auto const exe = std::filesystem::path { "fastcache-compile-node" };
    auto const spec = MakeNodeServiceSpec(exe, Testing::FirstStart(NodeConfig {}), Testing::InstallerPathProbe());
    CHECK(std::ranges::none_of(spec.arguments, [](std::string const& a) { return a.starts_with("--cluster-dir"); }));
    auto const machineWide = MachineWideNodeClusterDirectory(Testing::InstallerPathProbe());
    REQUIRE(machineWide.has_value());
    CHECK(std::ranges::contains(spec.ownedPaths,
                                OwnedPath { .path = Unwrap(machineWide),
                                            .privacy = PathPrivacy::Private,
                                            .credentialFiles = { std::filesystem::path { NodeKeyFileName } } }));
#if defined(_WIN32)
    CHECK(Unwrap(machineWide) == std::filesystem::path { R"(C:\ProgramData\fastcache-node)" });
    CHECK_FALSE(ServiceManagerHandsOverStateDirectory());
    CHECK(spec.serviceAccountEnvironment.empty());
#else
    CHECK(ServiceManagerHandsOverStateDirectory());
    CHECK(spec.serviceAccountEnvironment
          == std::vector<std::pair<std::string, std::string>> {
              { std::string { ServiceStateDirectoryVariable }, Unwrap(machineWide).string() } });

    // What that daemon then resolves is the machine-wide directory, from the handover.
    auto handedOver = std::map<std::string, std::string, std::less<>> {};
    handedOver.emplace(ServiceStateDirectoryVariable, Unwrap(machineWide).string());
    auto const resolved = DefaultNodeClusterDirectory(ScriptedConfigPathProbe { handedOver, Privilege::Unprivileged });
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).path == Unwrap(machineWide));
#endif

    // A named --cluster-dir is the directory: it is what is owned, and nothing is handed over or
    // owned beside it.
    auto named = Testing::FirstStart(NodeConfig {});
    named.clusterDir = "/srv/node";
    auto const namedSpec = MakeNodeServiceSpec(exe, named, Testing::InstallerPathProbe());
    CHECK(namedSpec.serviceAccountEnvironment.empty());
    CHECK(namedSpec.ownedPaths
          == std::vector<OwnedPath> { OwnedPath { .path = named.clusterDir,
                                                  .privacy = PathPrivacy::Private,
                                                  .credentialFiles = { std::filesystem::path { NodeKeyFileName } } } });

    // And the plist carries the pairs beside the account.
    auto withEnvironment = spec;
    withEnvironment.serviceAccount = "fastcache-node";
    withEnvironment.serviceAccountEnvironment = { { "STATE_DIRECTORY", "/Library/Application Support/fastcache-node" } };
    auto const plist = BuildLaunchdPlist(withEnvironment, ServiceScope::System, std::filesystem::path { "/tmp" });
    CHECK(plist.contains("<key>EnvironmentVariables</key>"));
    CHECK(
        plist.contains("<key>STATE_DIRECTORY</key>\n        <string>/Library/Application Support/fastcache-node</string>"));
}

#if !defined(_WIN32)

namespace
{

/// The POSIX handover's two calls with the account part left out: the scratch directory a case
/// stands in for the machine-wide one already belongs to the account running the suite, and the
/// subject is what `Seclude` does to the MODE.
class ModeOnlyHandover final: public IOwnedPathHandover
{
  public:
    /// @copydoc IOwnedPathHandover::Share
    [[nodiscard]] std::optional<std::string> Share(std::filesystem::path const& /*path*/) override
    {
        ++shared;
        return std::nullopt;
    }

    /// @copydoc IOwnedPathHandover::Seclude
    [[nodiscard]] std::optional<std::string> Seclude(std::filesystem::path const& path,
                                                     std::span<std::filesystem::path const> credentialLeaves) override
    {
        ++secluded;
        auto const secured = SecureDirectoryForService(path, "ignored-on-posix", credentialLeaves);
        return secured.has_value() ? std::nullopt : std::optional { secured.error() };
    }

    int shared { 0 };   ///< How many paths were handed over keeping what they inherit.
    int secluded { 0 }; ///< How many were given a mode of their own.
};

} // namespace

TEST_CASE("A service registration's defaulted state directory loses its group and other bits on POSIX",
          "[node][formation][defaults][service]")
{
    // The chain from the registration to the mode, on the platform the Windows run cannot see:
    // the spec marks the machine-wide directory Private, the handover routes a Private path to its
    // seclusion, and the POSIX seclusion is `chmod go-rwx`. The real directory is
    // /var/lib/fastcache-node, which a suite run unprivileged cannot touch, so the spec's own
    // entry is re-pointed at a scratch directory and keeps every other field.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" },
                                          Testing::FirstStart(NodeConfig {}),
                                          Testing::InstallerPathProbe());
    auto const machineWide = MachineWideNodeClusterDirectory(Testing::InstallerPathProbe());
    REQUIRE(machineWide.has_value());
    auto const* const owned = core::findOrNull(spec.ownedPaths, Testing::Unwrap(machineWide), &OwnedPath::path);
    REQUIRE(owned != nullptr);

    Testing::ScratchDirectory const scratch { "node-default-state-mode" };
    auto entry = *owned;
    entry.path = scratch.Path() / "fastcache-node";
    std::filesystem::create_directories(entry.path);
    std::filesystem::permissions(entry.path, std::filesystem::perms::all, std::filesystem::perm_options::replace);

    ModeOnlyHandover handover;
    auto const handedOver = HandOverOwnedPaths(std::span<OwnedPath const> { &entry, 1 }, handover);
    INFO(handedOver.refusal.value_or(std::string {}));
    REQUIRE_FALSE(handedOver.refusal.has_value());
    CHECK(handover.secluded == 1);
    CHECK(handover.shared == 0);

    auto const left = std::filesystem::status(entry.path).permissions();
    CHECK((left & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
    CHECK((left & std::filesystem::perms::owner_all) == std::filesystem::perms::owner_all);
}

#endif

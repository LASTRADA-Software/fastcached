// SPDX-License-Identifier: Apache-2.0
#include "LauncherCli.hpp"
#include "NodeConfig.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
/// A configuration a supervisor could actually be handed.
/// @return A worker that would start.
[[nodiscard]] NodeConfig Installable()
{
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6676";
    cfg.toolchains = { "/usr/bin/g++" };
    return cfg;
}

/// The member a `--raft-peer` token names, for a config built without a parser.
///
/// `raftPeers` holds members rather than tokens, so a fixture that assigns it has to
/// go through the same grammar the option table does -- which is the point: a test
/// spelling a member some other way would be testing a shape no command line can
/// produce.
/// @param spec The token as an operator would write it.
/// @return The member it names.
[[nodiscard]] Cluster::ClusterMember Peer(std::string_view spec)
{
    auto const member = Cluster::ParseMemberSpec(spec);
    // `Unwrap` hands back a default-constructed member for an empty optional, and
    // relies on a preceding `REQUIRE` to have failed the test first. Without one a
    // fixture with a typo'd spec would quietly become a member with no id and no
    // endpoint -- which is exactly the shape these cases exist to refuse.
    INFO("peer spec: " << spec);
    REQUIRE(member.has_value());
    return Unwrap(member);
}

/// Parse an argv fragment into a `NodeConfig`, the way `main` does.
///
/// A local helper rather than a shared one: everything outside this file builds a
/// config directly, since what it is testing is what comes back OUT of one.
/// @param args The flags, without the program name.
/// @return The configuration, or the first parse error.
[[nodiscard]] std::expected<NodeConfig, ConfigError> ParseNodeArgv(std::vector<char const*> const& args)
{
    NodeConfig cfg;
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg);
    if (!flow.has_value())
        return std::unexpected(flow.error());
    return cfg;
}

/// Read a registration back the way the service will at its next start.
///
/// The round trip every case about a registration asks for, in one place: whatever
/// reaches a supervisor is re-read by this binary at every boot, so a value that
/// cannot be spelled back is a service that registers cleanly and then fails
/// forever. Written out by hand three times before, which is three copies of the
/// step a case is *not* trying to test.
/// @param spec The registration, whose strings must outlive the call.
/// @return What this binary would come back up with, or the first parse error.
[[nodiscard]] std::expected<NodeConfig, ConfigError> ReparseSpec(ServiceSpec const& spec)
{
    std::vector<char const*> argv;
    argv.reserve(spec.arguments.size());
    for (auto const& argument: spec.arguments)
        argv.push_back(argument.c_str());
    return ParseNodeArgv(argv);
}

/// What `CacheCapacityOf` reports for a node that built no cache tier.
///
/// A named constant rather than a bare `{}` at the call sites, because it is the
/// argument that carries the meaning: this node runs no cache, so it holds nothing
/// back from a compile.
constexpr Distributed::NodeCacheCapacity NoCacheTier {};

/// What `CacheCapacityOf` reports for a node whose only tier is @p tier, holding
/// @p bytes.
///
/// One helper over the tier rather than one per tier: the cases differ by which
/// enumerator they name, and that is the fact each is asserting.
/// @param tier Which tier the node built.
/// @param bytes That tier's budget.
/// @return The cache record such a node announces.
[[nodiscard]] Distributed::NodeCacheCapacity CacheTierOf(StorageTier tier, std::uint64_t bytes)
{
    Distributed::NodeCacheCapacity cache;
    cache.tierBytesLimit[static_cast<std::size_t>(tier)] = bytes;
    return cache;
}

/// A machine a test can describe, standing in for the one it runs on.
///
/// The whole reason `IHostFactsSource` is a seam. Every rule `NodeCapacityOf` and
/// `OfferableSlots` apply is a function of cores and memory, and asserting them
/// against `OnlineCpuCount()` would mean asserting whatever the CI runner happens to
/// be — a test that passes for the wrong reason on one machine and fails on the next.
class FakeHost final: public IHostFactsSource
{
  public:
    /// @param cores Hardware threads to report.
    /// @param memoryBytes Physical memory to report.
    FakeHost(std::uint32_t cores, std::uint64_t memoryBytes) noexcept:
        _cores { cores },
        _memoryBytes { memoryBytes }
    {
    }

    [[nodiscard]] HostFacts const& Facts() const override
    {
        return _facts;
    }

    [[nodiscard]] std::uint32_t LogicalCores() const override
    {
        return _cores;
    }

    [[nodiscard]] std::uint64_t TotalMemoryBytes() const override
    {
        return _memoryBytes;
    }

    [[nodiscard]] DiskSpace SpaceOn(std::filesystem::path const& /*path*/) const override
    {
        return DiskSpace { .capacityBytes = 0, .freeBytes = 0 };
    }

  private:
    HostFacts _facts;
    std::uint32_t _cores;
    std::uint64_t _memoryBytes;
};

} // namespace

TEST_CASE("NodeConfig: --migrate-cache is a mode, and never reaches a service registration", "[node][config]")
{
    auto const parsed = ParseNodeArgv({ "--migrate-cache", "--cache-dir=/var/cache/fastcache-node" });
    REQUIRE(parsed.has_value());
    auto const& cfg = *parsed;
    REQUIRE(cfg.migrateCache);

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);
    CHECK(std::ranges::none_of(spec.arguments, [](std::string const& arg) { return FlagMatches(arg, "--migrate-cache"); }));
    // ...while the flag it acts ON is carried, since that one does describe the
    // running worker. Asserted alongside so this cannot pass by emitting nothing.
    CHECK(std::ranges::any_of(spec.arguments, [](std::string const& arg) { return FlagMatches(arg, "--cache-dir"); }));
}

TEST_CASE("NodeConfig: every flag that is worker state reaches the supervisor", "[node][service]")
{
    // The daemon's equivalent case exists because its table once stopped after
    // nine fields, so `--install-service --tls --metrics ...` reported success
    // and registered a plaintext, unmonitored daemon. The worker's table is new
    // and has the same shape, so it gets the same guard rather than waiting to
    // learn the lesson twice.
    //
    // Walked off NodeOptions() rather than a list written out here: a hand-written
    // list is updated by the same person who forgot the emitter.
    constexpr auto NotWorkerState = std::to_array<std::string_view>({
        "--install-service",   // a service must never re-install itself
        "--uninstall-service", //
        "--service-scope",     // install-time only; not a thing the worker runs with
        "--service-name",      // emitted unconditionally, above the table
        "--daemon",            // carried as ServiceSpec::daemonFlag, not an argument
        "--help",              //
        "--version",           //
        // The one field with no safe representation in launch arguments: a
        // supervisor records them where every local account can read them, so
        // emitting the secret would publish it to exactly the accounts it exists
        // to keep out. InlineCredentialRejection reports the omission instead.
        "--requirepass",
        // One-shot questions asked OF a running cluster, not state a worker runs
        // with. A registration carrying one would replay a single operator
        // decision at every boot, forever.
        "--cluster-status",
        "--cluster-set",
        "--cluster-forget",
        "--cluster-admit",
        // Same rule, applied to the store rather than to the cluster: a worker
        // that converted its store at every boot would replay one operator's
        // decision forever, on a store that after the first run has nothing
        // left to convert.
        "--migrate-cache",
        // Reports on a configuration and exits; a service that printed its ports at
        // every boot instead of serving them would be one that never starts.
        "--print-surfaces",
    });

    // A configuration in which no field holds its default, so every emitter fires.
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6676";
    cfg.bindAddress = "10.0.0.4";
    cfg.port = 7777;
    cfg.toolchains = { "/usr/bin/g++", "abc123=/usr/bin/clang++" };
    // Off, because this case gives every field a value differing from its default
    // so that every emitter fires -- and this one's default is on.
    cfg.toolchainDiscovery = false;
    cfg.slots = 12;
    cfg.nodeClass = Distributed::NodeClass::Dedicated;
    cfg.reservedCores = 3;
    cfg.adminListen = "0.0.0.0:6677";
    cfg.dashboard = true;
    cfg.dashboardTokenFile = "dashboard.token";
    cfg.tlsCertFile = "admin.crt";
    cfg.tlsKeyFile = "admin.key";
    // Not alongside the certificate in a real configuration -- they contradict
    // each other -- but this case exists to give every field a non-default value
    // so every emitter fires, and it asserts nothing about coherence.
    cfg.tlsSelfSigned = true;
    cfg.schedulerListen = "0.0.0.0:6678";
    cfg.fleetMembers = { "10.0.0.1:6676", "10.0.0.2:6676" };
    cfg.fleetOpen = true;
    // Derived from the default rather than written out, because the default is a
    // fraction of HOST RAM now: any literal here is one that silently equals the
    // default on some machine, and this case exists precisely to give every field
    // a value that differs from it.
    cfg.cacheMemoryBytes = NodeConfig {}.cacheMemoryBytes + 1;
    // Emitted on whether it was *typed*, not on whether it differs -- so a
    // fixture that assigns the field has to say so too.
    cfg.cacheMemoryExplicit = true;
    cfg.cacheDir = "cache";
    cfg.cacheDiskBytes = 40ULL * 1024 * 1024 * 1024;
    cfg.cacheListen = "127.0.0.1:6679";
    // Typed, not merely different -- the same rule `--cache-memory` above follows,
    // and for this flag the provenance is what decides whether a bind failure stops
    // the node, so a registration that lost it warns past a taken port forever.
    cfg.cacheListenExplicit = true;
    cfg.upstream = "cache.internal:6674";
    cfg.nodeId = "n1";
    cfg.raftListen = "0.0.0.0:6680";
    cfg.raftPeers = { Peer("n1=10.0.0.4:6680"), Peer("n2=10.0.0.5:6680") };
    // Every field differs from its default, so every emitter fires -- which is the
    // narrow question this case asks. Alongside two peers is a legitimate joiner:
    // under `--raft-join` that list is who this node can REACH rather than who it
    // is a cluster with.
    cfg.raftJoin = true;
    cfg.clusterDir = "cluster";
    cfg.clusterId = "fleet-a";
    cfg.discoveryAddress = "255.255.255.255:6681";
    cfg.discoveryReplyPort = 6682;
    cfg.clusterKeyFile = "cluster.key";
    cfg.logLevel = LogLevel::Debug;
    cfg.pidfile = "worker.pid";
    cfg.drainTimeoutSeconds = 90;
    // Worker state like any other, and the one that supplies most of the rest: a
    // registration that dropped it would come back at every boot knowing only what
    // was typed alongside --install-service, which for a package install is close
    // to nothing.
    cfg.configPath = "node.yaml";

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);

    for (auto const& option: NodeOptions())
    {
        if (std::ranges::contains(NotWorkerState, option.primary))
            continue;
        INFO("flag: " << option.primary);
        // FlagMatches is the parser's own rule for "this token names that flag",
        // so the guard cannot drift from what the worker will accept back.
        auto const emitted = std::ranges::any_of(
            spec.arguments, [&option](std::string const& arg) { return FlagMatches(arg, option.primary); });
        CHECK(emitted);
    }
}

TEST_CASE("NodeConfig: the credential is never written into a registration", "[node][service]")
{
    // Same rule as the daemon's, and it has to be the worker's too: the token is
    // what the scheduler authenticates this worker by, so publishing it to every
    // local account would let any of them register as this worker.
    auto cfg = Installable();
    cfg.token = "hunter2";

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);

    CHECK(std::ranges::none_of(spec.arguments, [](std::string const& arg) { return arg.contains("hunter2"); }));
    CHECK(spec.inlineCredential == InlineCredential::Present);

    // Present, so the shared gate refuses the install rather than dropping the
    // secret in silence.
    CHECK(ServiceRegistrationRejection(spec).has_value());
}

TEST_CASE("NodeConfig: every toolchain is re-emitted", "[node][service]")
{
    // One token per toolchain: a worker that came back serving fewer compilers
    // than it was installed with presents as a fleet that stopped matching, not
    // as a packaging bug -- and nothing at either end says why.
    auto cfg = Installable();
    cfg.toolchains = { "/usr/bin/g++", "/usr/bin/clang++", "deadbeef=/opt/gcc-13/bin/g++" };

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);

    for (auto const& toolchain: cfg.toolchains)
    {
        INFO("toolchain: " << toolchain);
        CHECK(std::ranges::contains(spec.arguments, std::format("--toolchain={}", toolchain)));
    }
}

TEST_CASE("NodeConfig: discovery is on unless the operator turns it off", "[node][config]")
{
    // On by default, because the whole point of #139 is that installing the package
    // is the setup. A worker that had to be told to look would be back to a
    // per-machine list somebody maintains.
    auto const byDefault = ParseNodeArgv({ "--scheduler=s:1" });
    REQUIRE(byDefault.has_value());
    CHECK(byDefault->toolchainDiscovery);
    CHECK(byDefault->toolchains.empty());

    auto const off = ParseNodeArgv({ "--scheduler=s:1", "--no-toolchain-discovery", "--toolchain=/usr/bin/cc" });
    REQUIRE(off.has_value());
    CHECK_FALSE(off->toolchainDiscovery);
}

TEST_CASE("NodeConfig: a discovery-off registration comes back with it still off", "[node][service]")
{
    // It changes what the service DOES at every boot, so losing it in the round
    // trip would have the worker quietly serving compilers the operator excluded --
    // and nothing anywhere would say the registration had changed meaning.
    auto pinned = Installable();
    pinned.toolchainDiscovery = false;
    auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", pinned);
    CHECK(std::ranges::contains(spec.arguments, "--no-toolchain-discovery"));

    // Round-tripped through this project's own parser, which is the rule every
    // registration follows: whatever reaches a supervisor has to survive it.
    auto const reparsed = ReparseSpec(spec);
    REQUIRE(reparsed.has_value());
    CHECK_FALSE(reparsed->toolchainDiscovery);

    // And a default registration does not carry it, so the flag means what it says
    // rather than appearing in every command line.
    auto const plain = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", Installable());
    CHECK_FALSE(std::ranges::contains(plain.arguments, "--no-toolchain-discovery"));
}

TEST_CASE("NodeConfig: an install is judged by the startup rules too", "[node][service][policy]")
{
    // The defect: `--install-service` consulted `NodeServiceRejection` and returned
    // before `StartupPolicyRejection` ever ran, while `MakeNodeServiceSpec` baked
    // the very flags those rules govern into the registration. A command line that
    // cannot start therefore installed cleanly and then failed at EVERY boot, into
    // a log nobody reads -- the exact inversion of the rule the install path states
    // for itself: refuse where an operator is watching, not where nobody is.
    auto certWithoutKey = Installable();
    certWithoutKey.adminListen = "0.0.0.0:6680";
    certWithoutKey.tlsCertFile = "/etc/fastcache/server.pem";

    // Passes the install table, which knows nothing about TLS.
    REQUIRE_FALSE(NodeServiceRejection(certWithoutKey).has_value());
    // And is refused at startup, every time, for a reason decided the moment it was
    // typed.
    REQUIRE(StartupPolicyRejection(certWithoutKey).has_value());

    auto const refusal = NodeInstallRejection(certWithoutKey);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).contains("--tls-key"));
}

TEST_CASE("NodeConfig: an install reports the rule that names the install", "[node][service][policy]")
{
    // Both tables object; the install-time one is reported, because its wording
    // names the action the operator is taking. Ordering is the only thing the
    // composition decides, so it is the only thing worth pinning.
    auto broken = Installable();
    broken.advertise.clear(); // an install rule
    broken.raftJoin = true;   // and a startup rule, with no --node-id
    broken.nodeId.clear();

    REQUIRE(NodeServiceRejection(broken).has_value());
    REQUIRE(StartupPolicyRejection(broken).has_value());

    auto const refusal = NodeInstallRejection(broken);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).contains("--advertise"));
}

TEST_CASE("NodeConfig: a second startup rule is refused at install too", "[node][service][policy]")
{
    // A structurally different rule from the TLS one, so this pins the composition
    // rather than one lucky row: nothing about `--dashboard` resembles a cert/key
    // pair, and both must reach the install path.
    auto dashboardNowhere = Installable();
    dashboardNowhere.dashboard = true; // with no --admin-listen to serve it on

    REQUIRE_FALSE(NodeServiceRejection(dashboardNowhere).has_value());

    auto const refusal = NodeInstallRejection(dashboardNowhere);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).contains("--admin-listen"));
}

TEST_CASE("NodeConfig: a registration that can work is still accepted", "[node][service][policy]")
{
    // The other direction, and the one a union gets wrong: a worker that would
    // start must still install. Over-refusing here would make the package
    // uninstallable on a perfectly ordinary machine.
    CHECK_FALSE(NodeInstallRejection(Installable()).has_value());

    // Including with the surfaces the startup table governs actually configured.
    auto full = Installable();
    full.adminListen = "0.0.0.0:6680";
    full.tlsCertFile = "/etc/fastcache/server.pem";
    full.tlsKeyFile = "/etc/fastcache/server.key";
    CHECK_FALSE(NodeInstallRejection(full).has_value());
}

TEST_CASE("NodeConfig: a registration that could not work is refused", "[node][service]")
{
    // Each of these produces a service that registers cleanly and then cannot do
    // its job, which is the worst shape this system has: the operator is told it
    // was installed, and nothing at any later point says otherwise.
    CHECK(!NodeServiceRejection(Installable()).has_value());

    auto noScheduler = Installable();
    noScheduler.scheduler.clear();
    CHECK(NodeServiceRejection(noScheduler).has_value());

    // No toolchain is NOT one of them any more, and that reversal is the point of
    // #139: registering a service before anybody knows what the machine holds is
    // exactly what makes installing the package the whole setup. The node answers
    // the question at boot.
    auto noToolchain = Installable();
    noToolchain.toolchains.clear();
    CHECK_FALSE(NodeServiceRejection(noToolchain).has_value());

    // What still cannot work is discovery turned OFF with nothing named -- refused
    // here, where an operator is watching, rather than at every boot where nobody
    // is. This is what keeps `--no-toolchain-discovery` from being a null flag.
    auto noToolchainNoDiscovery = noToolchain;
    noToolchainNoDiscovery.toolchainDiscovery = false;
    auto const bothOff = NodeServiceRejection(noToolchainNoDiscovery);
    REQUIRE(bothOff.has_value());
    CHECK(Unwrap(bothOff).contains("--no-toolchain-discovery"));

    // Discovery off WITH a toolchain named is a perfectly good registration: it is
    // how an operator pins a build farm to a curated set.
    auto pinned = Installable();
    pinned.toolchainDiscovery = false;
    CHECK_FALSE(NodeServiceRejection(pinned).has_value());

    // The one worth the most: left empty, --advertise defaults to
    // {--bind}:{--port} and --bind defaults to 0.0.0.0, which no client can dial.
    // Such a worker registers, heartbeats, is leased, and is never reached.
    auto noAdvertise = Installable();
    noAdvertise.advertise.clear();
    auto const rejection = NodeServiceRejection(noAdvertise);
    REQUIRE(rejection.has_value());
    CHECK(Unwrap(rejection).contains("--advertise"));
    CHECK(Unwrap(rejection).contains("0.0.0.0"));
}

TEST_CASE("NodeConfig: the daemon flag is carried apart from the arguments", "[node][service]")
{
    // launchd and systemd supervise the process they start, so a job that forks
    // is reaped instantly as "exited"; the Windows SCM needs the opposite. One
    // spec has to answer both, which is why the flag is a field rather than an
    // argument.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable());

    CHECK(spec.daemonFlag == "--daemon");
    CHECK(std::ranges::none_of(spec.arguments, [](std::string const& arg) { return arg == "--daemon"; }));

    // The SCM's command line is where it gets spent.
    CHECK(BuildServiceCommandLine(spec).contains("--daemon"));
}

TEST_CASE("NodeConfig: the worker's service name is not the daemon's", "[node][service]")
{
    // A machine may well run both. One shared default would make installing
    // either silently displace the other's registration.
    NodeConfig const cfg;
    Config const daemonCfg;
    CHECK(cfg.serviceName != daemonCfg.serviceName);
}

TEST_CASE("NodeConfig: a system-scope job names an unprivileged account", "[node][service]")
{
    // A system-scope launchd job with no UserName runs as ROOT, and this process
    // compiles input that arrived over the network. Naming the account the Linux
    // unit already uses puts the platform's existing "that account does not
    // exist" guard in the way, so a macOS system-scope install refuses until the
    // package creates it rather than silently succeeding as root.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable());

    CHECK(spec.serviceAccount == "fastcache-node");

    // And not the daemon's: a worker runs a compiler on input from the network
    // while fastcached owns the cache storage, so one account would let a
    // compromised compile rewrite every cached object.
    auto const daemonSpec = MakeDaemonServiceSpec(std::filesystem::path { "fastcached" }, Config {});
    CHECK(spec.serviceAccount != daemonSpec.serviceAccount);

    // The same decision for the other supervisor. Told nothing, the Windows SCM
    // logs a service on as LocalSystem, which is the machine -- so the worker names
    // a virtual account there for exactly the reason it names an account here.
    CHECK(spec.windowsLogon == WindowsLogonAccount::VirtualAccount);
    CHECK(Unwrap(WindowsLogonName(spec)) == "NT SERVICE\\FastCacheCompileNode");
}

TEST_CASE("NodeConfig: the worker's launchd label is what the packaging removes", "[node][service]")
{
    // FASTCACHED_MACOS_NODE_LABEL in the top-level CMakeLists.txt is substituted
    // into fastcached-uninstall.sh.in, which boots the job out and deletes its
    // plist. It has to be this string, because the uninstaller also removes the
    // binary that would otherwise answer for itself -- so a label that drifted
    // would leave a job bootstrapped against a deleted executable, with nothing
    // on disk left able to reach it.
    //
    // Compared against the PACKAGING's value, compiled in by this target's
    // CMakeLists, rather than a literal repeated here. A literal would be a third
    // copy that agrees with the derivation and drifts from the package in
    // silence, which is the failure this assertion exists to prevent -- the
    // derivation itself (LaunchdLabelPrefix + the lowercased service name) is
    // already covered by ServiceControl_test.cpp.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable());

    CHECK(LaunchdLabel(spec) == std::string_view { FASTCACHED_MACOS_NODE_LABEL });
}

TEST_CASE("NodeConfig: the worker's registration survives its own parser", "[node][service]")
{
    // Every flag baked into a registration is re-read by this binary at the next
    // start, so one it cannot parse is a service that registers cleanly and then
    // fails forever. The worker takes NO config file and NO storage directory --
    // NodeOptions() has neither flag -- and the installer used to fill both in
    // from the daemon's defaults, because the application name was hardcoded.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable());
    CHECK(spec.applicationName.empty());

    // The assertion that matters is not "the field is empty" but what the field
    // makes the installer do, so drive the installer's own function -- on every
    // platform, which is the half that was missing while it lived inside the
    // macOS block.
    for (auto const scope: { ServiceScope::User, ServiceScope::System })
    {
        auto const filled = WithScopeDefaults(spec,
                                              scope,
                                              std::filesystem::path { "/Users/jo" },
                                              std::filesystem::path { "/opt/fastcached/etc/fastcached.yaml" });

        // Every argument must be one this binary's own parser accepts.
        for (auto const& argument: filled.arguments)
        {
            auto const flag = argument.substr(0, argument.find('='));
            INFO("registered flag: " << argument);
            CHECK(std::ranges::any_of(
                NodeOptions(), [&flag](auto const& option) { return option.primary == flag || option.alias == flag; }));
        }
    }
}

TEST_CASE("NodeConfig: consensus state may not default to a relative path in a service", "[node][service]")
{
    // `fastcache-cluster/<node-id>` is a fine default for a worker an operator
    // started in a directory they chose. A service has no such directory: the SCM
    // starts it in C:\Windows\System32 and launchd in /, both writable only by the
    // privileges this worker is deliberately not given. So the job would register,
    // report success, and fail to open its own consensus state at every start --
    // which is the shape refused here rather than at the next boot.
    auto cfg = Installable();
    cfg.raftListen = "0.0.0.0:6680";
    cfg.nodeId = "n1";

    REQUIRE(NodeServiceRejection(cfg).has_value());
    CHECK(Unwrap(NodeServiceRejection(cfg)).contains("--cluster-dir"));

    // Named, and it is accepted.
    cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
    CHECK(!NodeServiceRejection(cfg).has_value());

    // A worker running no consensus is unaffected: the flag it would need is one
    // it has no use for.
    auto plain = Installable();
    plain.clusterDir.clear();
    CHECK(!NodeServiceRejection(plain).has_value());
}

TEST_CASE("NodeConfig: a --raft-peer that names no member is refused where it was typed", "[node][consensus][policy]")
{
    // The grammar is enforced by the parser, so a token that is not a member is
    // refused on EVERY path -- a hand start, `--install-service`, and the cluster
    // verbs alike. It used to be checked inside `ConsensusTier::Start`, which the
    // install path returns long before reaching: a registration carrying
    // `--raft-peer=garbage` was written happily and then died at every boot (#168).
    //
    // Every way a token can fail to name a member, through the one grammar.
    for (auto const* const token: {
             "garbage",          // neither half
             "n1=",              // an id and nothing to dial
             "=10.0.0.1:6680",   // an endpoint and nobody it belongs to
             "n1=10.0.0.1",      // a host with no port
             "n1=10.0.0.1:0",    // a port nobody can connect to
             "n1=10.0.0.1:http", // a port that is not a number
         })
    {
        INFO("token: " << token);
        auto const flag = std::format("--raft-peer={}", token);
        auto const parsed = ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/cc", flag.c_str() });
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().field == "raft-peer");
        // The offending token is named, which is the half a policy row could not do:
        // its messages are static prose and an operator may have typed five peers.
        CHECK(parsed.error().context.contains(token));
    }

    // And the shapes that ARE members, including the bracketed v6 one every
    // `SplitHostPort` caller has to keep intact.
    auto const parsed = ParseNodeArgv({ "--scheduler=s:1",
                                        "--toolchain=/usr/bin/cc",
                                        "--raft-peer=n1=10.0.0.1:6680",
                                        "--raft-peer=n2=[2001:db8::1]:6680" });
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->raftPeers.size() == 2);
    CHECK(parsed->raftPeers[0].id == "n1");
    CHECK(parsed->raftPeers[0].raftEndpoint == "10.0.0.1:6680");
    CHECK(parsed->raftPeers[1].id == "n2");
    CHECK(parsed->raftPeers[1].raftEndpoint == "[2001:db8::1]:6680");
}

TEST_CASE("NodeConfig: a registered peer list comes back the way it went in", "[node][consensus][service]")
{
    // A registration replays its command line forever, so the peers have to survive
    // this project's own parser -- and they are now stored parsed, which means the
    // installer RE-RENDERS each token rather than echoing it. A member that came
    // back spelled differently is a node the cluster counts and cannot reach.
    auto cfg = Installable();
    cfg.nodeId = "n1";
    cfg.raftListen = "0.0.0.0:6680";
    cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
    cfg.raftPeers = { Peer("n1=10.0.0.1:6680"), Peer("n2=[2001:db8::1]:6680") };

    auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", cfg);

    auto const reparsed = ReparseSpec(spec);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->raftPeers == cfg.raftPeers);
}

TEST_CASE("NodeConfig: a --node-id that names no --raft-peer is refused before it is installed", "[node][consensus][policy]")
{
    // #168. `ConsensusTier::Start` refuses this, and the install path returns long
    // before any tier is built -- so the registration was written, reported
    // installed, and then exited `ExitUsage` at every boot with nobody watching.
    // It is a pure function of the command line, like every startup rule, so it is
    // one now.
    auto const shapes = std::to_array<std::pair<char const*, NodeConfig>>({
        { "no --raft-peer at all",
          [] {
              auto cfg = Installable();
              cfg.nodeId = "n1";
              return cfg;
          }() },
        { "peers that name somebody else",
          [] {
              auto cfg = Installable();
              cfg.nodeId = "n1";
              cfg.raftPeers = { Peer("n2=10.0.0.2:6680"), Peer("n3=10.0.0.3:6680") };
              return cfg;
          }() },
        { "a joiner that named the cluster and not itself",
          [] {
              auto cfg = Installable();
              cfg.nodeId = "n4";
              cfg.raftJoin = true;
              cfg.raftPeers = { Peer("n1=10.0.0.1:6680") };
              return cfg;
          }() },
    });

    for (auto const& [what, cfg]: shapes)
    {
        INFO(what);
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--raft-peer"));

        // And through the install path, which is the half #166 closed everywhere
        // else and this one reached around.
        auto const install = NodeInstallRejection(cfg);
        REQUIRE(install.has_value());
        CHECK(Unwrap(install).contains("--raft-peer"));
    }

    // The tier says the same thing in the same words, because it asks the same
    // predicate. A `NodeConfig` built by hand reaches it without a parser or a
    // policy table in between, and must not hear a second opinion.
    CHECK(Unwrap(StartupPolicyRejection(shapes[0].second)) == NodeIdNamesNoPeerRefusal);

    // And it names its own flag, which is what lets `main.cpp` print a tier's
    // refusal without prefixing one -- it used to, and rendered this message as
    // "--node-id --node-id names no --raft-peer".
    CHECK(NodeIdNamesNoPeerRefusal.starts_with("--node-id"));

    // A node that names itself is accepted, bootstrapping or joining. The key is
    // part of that shape rather than decoration: consensus grows the set this node's
    // compile port admits, so a clustered node has to be able to check a grant
    // (#282, the lease rule).
    auto bootstrapping = Installable();
    bootstrapping.nodeId = "n1";
    bootstrapping.raftListen = "6680";
    bootstrapping.raftPeers = { Peer("n1=10.0.0.1:6680"), Peer("n2=10.0.0.2:6680") };
    bootstrapping.clusterKeyFile = "cluster.key";
    CHECK_FALSE(StartupPolicyRejection(bootstrapping).has_value());
}

TEST_CASE("NodeConfig: a --node-id with no port for its peers is refused before it is installed",
          "[node][consensus][policy]")
{
    // The third refusal `ConsensusTier::Start` made and neither table did:
    // `ParseEndpoint` answers nullopt for an empty `--listen-raft` exactly as it
    // does for a malformed one, so a node with an id, a peer list naming itself
    // and no port installed cleanly and then exited at every boot. The rule asks
    // the tier's own question, the way the `--dashboard` row asks
    // `AdminEndpoint`'s.
    // Every one of these is refused and names the flag -- but WHICH rule answers
    // changed with #288, and asserting only "refused, and names --listen-raft" would
    // pass under both behaviours and so pin neither.
    //
    // `--listen-raft` is now in the surface table's grammar loop, because that table
    // holds every surface and leaving one out would need a column meaning "somebody
    // else checks this one". So a MALFORMED address is answered there, echoing what
    // the operator typed; an ABSENT one still reaches the consensus rule below, which
    // is the input that rule was written about. Both still fire, for their own
    // inputs, which is what made the move safe.
    struct RaftCase
    {
        char const* listen;  ///< What the operator wrote.
        char const* expects; ///< Text only the answering rule produces.
    };

    for (auto const& [listen, expects]: std::to_array<RaftCase>({
             // Nothing to judge, so the grammar loop skips it and the cross-flag rule
             // answers: the surface is configured and cannot be served.
             { .listen = "", .expects = "needs a usable" },
             // Text that is not an address, answered where the value can be echoed.
             { .listen = "nope", .expects = "is not [<address>:]<port>" },
             // Zero is not a port anyone can dial, so it is a malformed address
             // rather than an absent one.
             { .listen = "0", .expects = "is not [<address>:]<port>" },
             // A host with no port. Same rule, and the one an operator most often
             // reaches by forgetting the colon.
             { .listen = "10.0.0.1", .expects = "is not [<address>:]<port>" },
         }))
    {
        INFO("--listen-raft=" << listen);
        auto cfg = Installable();
        cfg.nodeId = "n1";
        cfg.raftPeers = { Peer("n1=10.0.0.1:6680") };
        cfg.raftListen = listen;

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--listen-raft"));
        CHECK(Unwrap(refusal).contains(expects));
        CHECK(NodeInstallRejection(cfg).has_value());
    }

    // A bare port is enough, and binds the wildcard -- peers are on other machines
    // by definition.
    auto bare = Installable();
    bare.nodeId = "n1";
    bare.raftPeers = { Peer("n1=10.0.0.1:6680") };
    bare.raftListen = "6680";
    bare.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
    // Consensus admits machines to this node's compile port, so a clustered node
    // needs the key that checks their grants (#282).
    bare.clusterKeyFile = "cluster.key";
    CHECK_FALSE(StartupPolicyRejection(bare).has_value());
    CHECK_FALSE(NodeInstallRejection(bare).has_value());

    // And a worker with no consensus at all is not asked for a port it has no use
    // for, which is by far the common deployment.
    CHECK_FALSE(StartupPolicyRejection(Installable()).has_value());
}

TEST_CASE("NodeConfig: a listen flag whose value is not an endpoint is refused before it is installed", "[node][policy]")
{
    // #186. Each of these grammars was checked inside the tier that binds it, and
    // `--install-service` returns long before any tier is built -- so a typo in any
    // of them registered cleanly and then exited `ExitUsage` at every boot. They are
    // pure functions of the command line, so they are decided where it is typed.
    //
    // A configuration per flag rather than one carrying all four, because the point
    // is that EACH is refused on its own: one row covering for another is exactly
    // what a table of four near-identical rules would hide.
    auto const shapes = std::to_array<std::pair<char const*, NodeConfig>>({
        { "--listen-scheduler",
          [] {
              auto cfg = Installable();
              cfg.schedulerListen = "nope";
              cfg.fleetOpen = true; // else the fleet-membership rule answers first
              return cfg;
          }() },
        { "--admin-listen",
          [] {
              auto cfg = Installable();
              cfg.adminListen = "nope";
              return cfg;
          }() },
        { "--listen-cache",
          [] {
              auto cfg = Installable();
              cfg.cacheListen = "nope";
              return cfg;
          }() },
        { "--discovery",
          [] {
              auto cfg = Installable();
              cfg.discoveryAddress = "nope";
              cfg.nodeId = "n1";
              cfg.raftListen = "6680";
              cfg.raftPeers = { Peer("n1=10.0.0.1:6680") };
              cfg.clusterKeyFile = "cluster.key";
              cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
              return cfg;
          }() },
    });

    for (auto const& [flag, cfg]: shapes)
    {
        INFO(flag);
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains(flag));
        // The offending text is echoed, which is what a row of static prose could
        // not do: an operator who mistyped one of four ports has to be told which.
        CHECK(Unwrap(refusal).contains("nope"));

        // And through the install path, which is the whole point: #166 composed the
        // tables so a registration is judged by both, and this reached around them.
        auto const install = NodeInstallRejection(cfg);
        REQUIRE(install.has_value());
        CHECK(Unwrap(install).contains(flag));
    }
}

TEST_CASE("NodeConfig: a listen flag that is absent, defaulted or valid is accepted", "[node][policy]")
{
    // The direction a "must parse" rule gets wrong. Empty means the surface is off
    // for three of these, and `--listen-cache` carries a non-empty default that
    // every ordinary node runs with -- so a rule reading "must parse" rather than
    // "parses when given" would refuse the default deployment outright.
    CHECK_FALSE(StartupPolicyRejection(Installable()).has_value());
    CHECK_FALSE(NodeInstallRejection(Installable()).has_value());

    auto off = Installable();
    off.schedulerListen.clear();
    off.adminListen.clear();
    off.cacheListen.clear(); // documented as "no cache tier", not as a mistake
    off.discoveryAddress.clear();
    CHECK_FALSE(StartupPolicyRejection(off).has_value());

    // Every surface configured, in each of the spellings the tiers accept: a bare
    // port, an address and port, and a bracketed v6 literal.
    auto configured = Installable();
    configured.schedulerListen = "6678";
    configured.fleetOpen = true;
    // `--fleet-open` admits every machine there is, so this node has to be able to
    // check the grants they present (#282).
    configured.clusterKeyFile = "cluster.key";
    configured.adminListen = "127.0.0.1:6677";
    configured.cacheListen = "[::1]:6679";
    configured.dashboard = true;
    CHECK_FALSE(StartupPolicyRejection(configured).has_value());
}

TEST_CASE("NodeConfig: a listen flag with a port and no host is refused, not widened", "[node][policy]")
{
    // `:6674` is not "the default host, and this port". An empty bind host reaches
    // `getaddrinfo` as nullptr under AI_PASSIVE, so it is the WILDCARD -- which for
    // the cache surface is the whole machine's network rather than the loopback its
    // default promises, and nothing anywhere would say so.
    auto cache = Installable();
    cache.cacheListen = ":6674";
    auto const widened = StartupPolicyRejection(cache);
    REQUIRE(widened.has_value());
    CHECK(Unwrap(widened).contains("--listen-cache"));

    auto admin = Installable();
    admin.adminListen = ":6677";
    CHECK(StartupPolicyRejection(admin).has_value());

    // A BARE port is a different thing and stays legal: it names no host at all, so
    // it takes the surface's own default rather than silently replacing it.
    auto bare = Installable();
    bare.cacheListen = "6674";
    bare.adminListen = "6677";
    bare.schedulerListen = "6678";
    bare.fleetOpen = true;
    bare.clusterKeyFile = "cluster.key"; // --fleet-open admits other machines (#282)
    CHECK_FALSE(StartupPolicyRejection(bare).has_value());
}

TEST_CASE("NodeConfig: --discovery takes an address and a port, never a bare port", "[node][policy]")
{
    // Its grammar is NOT the one the three listen flags take, and that difference is
    // the tier's: a beacon is sent TO an address, so there is no host for a bare
    // port to default to. A shared "is this an endpoint" test would accept `6681`
    // here and the tier would then refuse it at every boot.
    auto bare = Installable();
    bare.nodeId = "n1";
    bare.raftListen = "6680";
    bare.raftPeers = { Peer("n1=10.0.0.1:6680") };
    bare.clusterKeyFile = "cluster.key";
    bare.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
    bare.discoveryAddress = "6681";

    auto const refusal = StartupPolicyRejection(bare);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).contains("--discovery"));

    // Nor a port with no address in front of it, which is the shape that splits and
    // still names nowhere: `UdpSocket::Send` refuses an empty destination outright,
    // so such a node starts, runs discovery, and announces to nobody. That is decided
    // by the text alone, which is what makes it this table's to refuse.
    bare.discoveryAddress = ":6681";
    auto const headless = StartupPolicyRejection(bare);
    REQUIRE(headless.has_value());
    CHECK(Unwrap(headless).contains("--discovery"));

    // And the spelling it does take.
    bare.discoveryAddress = "255.255.255.255:6681";
    CHECK_FALSE(StartupPolicyRejection(bare).has_value());
}

TEST_CASE("NodeConfig: the --raft-join rules keep the more specific answer", "[node][consensus][policy]")
{
    // The new row must not make either of them dead: both describe a joiner more
    // precisely than "names no --raft-peer" does, and both come first.
    NodeConfig noIdentity;
    noIdentity.raftJoin = true;
    noIdentity.raftPeers = { Peer("n1=10.0.0.1:6680") };
    CHECK(Unwrap(StartupPolicyRejection(noIdentity)).contains("--raft-join needs --node-id"));

    NodeConfig noPeers;
    noPeers.raftJoin = true;
    noPeers.nodeId = "n4";
    CHECK(Unwrap(StartupPolicyRejection(noPeers)).contains("--raft-join needs --raft-peer"));
}

TEST_CASE("NodeConfig: consensus flags with no --node-id are refused rather than ignored", "[node][consensus][policy]")
{
    // `StartConsensusOrExplain` returns a null tier when `--node-id` is empty, so
    // these flags are read by nobody: nothing binds, nothing dials, and nothing
    // anywhere says the operator's cluster was not configured. That is the silent
    // no-op this table already refuses for `--cluster-key-file` without
    // `--discovery` and `--dashboard-token-file` without `--dashboard`.
    auto listening = Installable();
    listening.raftListen = "6680";
    listening.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
    CHECK(Unwrap(StartupPolicyRejection(listening)).contains("--node-id"));
    CHECK(NodeInstallRejection(listening).has_value());

    auto peered = Installable();
    peered.raftPeers = { Peer("n1=10.0.0.1:6680") };
    CHECK(Unwrap(StartupPolicyRejection(peered)).contains("--node-id"));

    // `--cluster-dir` is deliberately NOT one of them: `FleetHistoryPath` reads it
    // for the dashboard's history file, so a node with no consensus at all still
    // has a use for it. Refusing it here would refuse a working configuration.
    auto history = Installable();
    history.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
    CHECK_FALSE(StartupPolicyRejection(history).has_value());

    // And the ordinary single-machine worker, which configures none of this.
    CHECK_FALSE(StartupPolicyRejection(Installable()).has_value());
}

TEST_CASE("NodeConfig: a system-scope job owns the directories it was given", "[node][service]")
{
    // A system-scope worker runs as an unprivileged account and root created these
    // directories, so without the handover its first write fails with EACCES --
    // which launchd surfaces only as a job that exits over and over. The daemon
    // has always handed over its --storage for exactly this reason.
    auto cfg = Installable();
    cfg.cacheDir = std::filesystem::path { "/var/cache/fastcache-node" };
    cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);

    CHECK(std::ranges::contains(spec.ownedPaths, cfg.cacheDir));
    CHECK(std::ranges::contains(spec.ownedPaths, cfg.clusterDir));

    // Only what the operator named, never a parent: handing over /var/cache would
    // reassign a directory shared with other services to an unprivileged compile
    // account, silently, under a message saying the service had been installed.
    CHECK(std::ranges::none_of(spec.ownedPaths, [](std::filesystem::path const& owned) {
        return owned == std::filesystem::path { "/var/cache" } || owned == std::filesystem::path { "/var/lib" };
    }));

    // A worker given neither hands over nothing, rather than a path nobody asked
    // for -- the mirror of the rule above.
    CHECK(MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable()).ownedPaths.empty());
}

TEST_CASE("A node says who it admits in the line an operator reads at startup", "[node][policy][membership]")
{
    // #235's second half. The first was that a worker could not be GIVEN a policy;
    // this is that a worker given none said nothing about it -- it started, bound
    // the wildcard, registered, was leased out and refused every dispatched compile,
    // while the one line an operator reads to confirm it came up reported nothing
    // but health. The scheduler tier had such a line; the worker, which is the
    // surface that actually refuses, did not.
    //
    // One function rather than a phrase per surface, because a node running both
    // must state one policy rather than two, and because `main.cpp` is in no test
    // target -- a phrase built there could not be asserted on at all.
    struct Row
    {
        char const* what;                 ///< The shape, for the failure message.
        std::vector<std::string> members; ///< `--fleet-member`, if any.
        bool open;                        ///< Whether `--fleet-open` was given.
        std::string nodeId;               ///< `--node-id`, i.e. whether consensus runs.
        char const* says;                 ///< What the line must contain.
        char const* saysNot { nullptr };  ///< What it must NOT contain, if anything.
    };

    auto const rows = std::to_array<Row>({
        { .what = "no policy at all", .members = {}, .open = false, .nodeId = {}, .says = "this machine only" },
        // Naming the remedy is the point of that row: an operator who reads "this
        // machine only" and is not told the two flags has been informed of a symptom.
        { .what = "no policy names the flags that give one",
          .members = {},
          .open = false,
          .nodeId = {},
          .says = "--fleet-member" },
        // A node running consensus is about to admit hosts nobody typed, so a line
        // reading as a final answer would mislead -- but both flags are still named,
        // because the agreed member set ADDS to the listed one rather than replacing
        // it (#251). `--fleet-member` therefore keeps working on such a node, and it
        // is the only route by which a client machine, which is no cluster peer, is
        // admitted at all.
        { .what = "no policy, with consensus running, says the cluster will supply members",
          .members = {},
          .open = false,
          .nodeId = "n1",
          .says = "the cluster's members",
          // And never the unclustered phrase: "this machine only" is a final answer,
          // and on a node whose cluster is about to agree a member set it is wrong.
          .saysNot = "this machine only" },
        { .what = "no policy, with consensus running, still names --fleet-member",
          .members = {},
          .open = false,
          .nodeId = "n1",
          .says = "--fleet-member" },
        { .what = "no policy, with consensus running, still names --fleet-open",
          .members = {},
          .open = false,
          .nodeId = "n1",
          .says = "--fleet-open" },
        // And a node that HAS a list says the cluster adds to it, so an operator
        // reading a bare count does not conclude that is the whole policy.
        { .what = "a member list, with consensus running",
          .members = { "10.0.0.1:6676" },
          .open = false,
          .nodeId = "n1",
          .says = "the cluster's members" },
        { .what = "a member list",
          .members = { "10.0.0.1:6676", "10.0.0.2:6676" },
          .open = false,
          .nodeId = {},
          .says = "2 member" },
        // "This machine" out loud even when a list exists: that admission is
        // unconditional, and an operator reading a bare count would not know their
        // own builds were covered.
        { .what = "a member list still says this machine",
          .members = { "10.0.0.1:6676" },
          .open = false,
          .nodeId = {},
          .says = "this machine" },
        { .what = "--fleet-open", .members = {}, .open = true, .nodeId = {}, .says = "every caller" },
    });

    for (auto const& row: rows)
    {
        INFO(row.what);
        NodeConfig cfg;
        cfg.fleetMembers = row.members;
        cfg.fleetOpen = row.open;
        cfg.nodeId = row.nodeId;

        auto const summary = AdmissionSummary(cfg);
        CHECK(summary.contains(row.says));
        if (row.saysNot != nullptr)
            CHECK_FALSE(summary.contains(row.saysNot));
    }
}

TEST_CASE("A scheduler that could not admit anybody is refused at startup", "[node][scheduler][policy]")
{
    // Every rule here describes a configuration that would START SUCCESSFULLY and
    // then not work. That is the shape this codebase refuses at the one moment an
    // operator is watching, rather than leaving to be discovered as a fleet that
    // mysteriously never distributes anything.

    SECTION("a listener with no policy at all")
    {
        // An empty member set refusing everybody is the right *default* -- it is what
        // stops a misconfigured node becoming an open scheduler -- but it is not a
        // working configuration, and the two are told apart here rather than at
        // runtime by an operator reading counters.
        NodeConfig cfg;
        cfg.schedulerListen = "0.0.0.0:6678";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--fleet-member"));
        CHECK(Unwrap(refusal).contains("--fleet-open"));
    }

    SECTION("the two policies contradicting each other")
    {
        // Silently preferring either would make the narrower one a no-op an operator
        // believes is in force -- which is worse than refusing, because it is the
        // permissive half that would win by accident.
        NodeConfig cfg;
        cfg.schedulerListen = "0.0.0.0:6678";
        cfg.fleetOpen = true;
        // Derived from the default rather than written out, because the default is a
        // fraction of HOST RAM now: any literal here is one that silently equals the
        // default on some machine, and this case exists precisely to give every field
        // a value that differs from it.
        cfg.cacheMemoryBytes = NodeConfig {}.cacheMemoryBytes + 1;
        // Emitted on whether it was *typed*, not on whether it differs -- so a
        // fixture that assigns the field has to say so too.
        cfg.cacheMemoryExplicit = true;
        cfg.cacheDir = "cache";
        cfg.cacheListen = "127.0.0.1:6679";
        cfg.upstream = "cache.internal:6674";
        cfg.fleetMembers = { "10.0.0.1:6676" };

        REQUIRE(StartupPolicyRejection(cfg).has_value());

        // And on a node running no scheduler at all, which is where the rest of
        // this group stopped applying (#235): this row never depended on one, and
        // the permissive half is the one that would have won by accident.
        NodeConfig worker;
        worker.fleetMembers = { "10.0.0.1:6676" };
        worker.fleetOpen = true;

        // Named rather than merely counted: with the mirror row gone this shape is
        // refused by exactly one rule, and a bare `has_value()` would stay green if
        // some unrelated row started answering in its place.
        auto const refusal = StartupPolicyRejection(worker);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("contradict"));
    }

    SECTION("peers admitted to a worker that advertises the wildcard")
    {
        // The shape #235's removal newly made reachable, and the reason it needs a
        // row rather than a doc line: admitting peers is only ever so they can dial
        // this worker, and `--advertise` defaults to `{--bind}:{--port}` whose bind
        // is `0.0.0.0`. The scheduler hands that string to clients verbatim, so a
        // client on another machine dials the wildcard and reaches ITSELF. The
        // worker registers, heartbeats, is leased out and is never reached.
        struct Row
        {
            char const* what;      ///< The shape, for the failure message.
            char const* advertise; ///< `--advertise`, or empty for the fallback.
            char const* bind;      ///< `--bind`.
        };

        auto const refused = std::to_array<Row>({
            { .what = "no --advertise at all", .advertise = "", .bind = "0.0.0.0" },
            // Spelled out rather than defaulted: the endpoint is judged, never the
            // question of which flag produced it.
            { .what = "the wildcard spelled out", .advertise = "0.0.0.0:6676", .bind = "0.0.0.0" },
            { .what = "the v6 wildcard", .advertise = "[::]:6676", .bind = "0.0.0.0" },
            // An empty host is the wildcard too -- it reaches `getaddrinfo` as
            // nullptr -- which is the third case `--listen-cache=:6674` is refused
            // for and the one that reads like an address.
            { .what = "a bare colon", .advertise = ":6676", .bind = "0.0.0.0" },
        });

        for (auto const& row: refused)
        {
            INFO(row.what);
            NodeConfig cfg;
            cfg.scheduler = "scheduler.internal:6675";
            cfg.advertise = row.advertise;
            cfg.bindAddress = row.bind;
            cfg.fleetOpen = true;

            auto const refusal = StartupPolicyRejection(cfg);
            REQUIRE(refusal.has_value());
            CHECK(Unwrap(refusal).contains("--advertise"));
        }

        // And the three shapes that must NOT be refused, each of which would be a
        // working deployment this rule had broken.

        // The one-machine deployment: no membership flags, so no peer is admitted,
        // so nothing is told to dial anything. This is what an operator gets by
        // installing the package and starting a node, and it is correct.
        NodeConfig alone;
        alone.scheduler = "127.0.0.1:6675";
        CHECK_FALSE(StartupPolicyRejection(alone).has_value());

        // A node sharing its CACHE tier with listed peers and registering nowhere.
        // That surface is reached at `--listen-cache`, so its advertise is never
        // sent to anybody and the wildcard costs it nothing.
        NodeConfig cacheOnly;
        cacheOnly.fleetMembers = { "10.0.0.1:6676" };
        cacheOnly.clusterKeyFile = "cluster.key"; // it admits another machine (#282)
        CHECK_FALSE(StartupPolicyRejection(cacheOnly).has_value());

        // And the worker the getting-started page documents, which names both.
        NodeConfig worker;
        worker.scheduler = "scheduler.internal:6675";
        worker.advertise = "worker-01.internal:6676";
        worker.fleetOpen = true;
        worker.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(worker).has_value());
    }

    SECTION("a joiner with no identity")
    {
        // The cluster admits an ID and counts every vote against one. A node waiting
        // to be admitted without one would listen forever and could never be named.
        NodeConfig cfg;
        cfg.raftJoin = true;
        cfg.raftPeers = { Peer("n1=10.0.0.4:6680") };

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--node-id"));
    }

    SECTION("a joiner naming nothing at all")
    {
        // Its own address is the half only it knows -- and without the cluster's it
        // cannot answer the leader that admits it, which is what makes the leader
        // walk its replication back to an empty log. Admitted, dialled, and
        // permanently silent.
        NodeConfig cfg;
        cfg.raftJoin = true;
        cfg.nodeId = "n4";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--raft-peer"));
    }

    SECTION("the working shapes are accepted")
    {
        // Each carries `--cluster-key-file`, which every shape admitting another
        // machine now needs: the scheduler signs a grant and the worker checks it,
        // and neither is possible without the key (#282).
        NodeConfig listed;
        listed.schedulerListen = "0.0.0.0:6678";
        listed.fleetMembers = { "10.0.0.1:6676" };
        listed.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(listed).has_value());

        NodeConfig open;
        open.schedulerListen = "0.0.0.0:6678";
        open.fleetOpen = true;
        open.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(open).has_value());

        // A joiner names itself and the cluster it is asking to join: under
        // `--raft-join` that list is who it can REACH rather than who it counts.
        NodeConfig joiner;
        joiner.raftJoin = true;
        joiner.nodeId = "n4";
        joiner.raftListen = "6680";
        joiner.raftPeers = { Peer("n4=10.0.0.4:6680"), Peer("n1=10.0.0.1:6680"), Peer("n2=10.0.0.2:6680") };
        joiner.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(joiner).has_value());

        // And a worker running no scheduler at all -- by far the common case -- is
        // untouched by any of this.
        CHECK_FALSE(StartupPolicyRejection(NodeConfig {}).has_value());

        // Including one that names a membership policy, which is the whole of #235.
        // A mirror row used to refuse exactly this, reasoning that "a policy nothing
        // consults is a policy an operator believes is in force" -- and the premise
        // was false: one `NodeMembership` serves the scheduler, the cache tier AND
        // the compile port, and the compile port is built unconditionally. What the
        // row achieved was pinning every worker to an empty member list, which
        // admits loopback alone, so the worker the getting-started page documents
        // refused every dispatched compile with `NotAMember`.
        NodeConfig listedWorker;
        listedWorker.scheduler = "scheduler.internal:6675";
        listedWorker.advertise = "worker-01.internal:6676";
        listedWorker.fleetMembers = { "10.0.0.1:6676" };
        listedWorker.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(listedWorker).has_value());

        NodeConfig openWorker;
        openWorker.scheduler = "scheduler.internal:6675";
        openWorker.advertise = "worker-01.internal:6676";
        openWorker.fleetOpen = true;
        openWorker.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(openWorker).has_value());
    }
}

TEST_CASE("NodeConfig: a reserve of zero is re-emitted, because zero is an answer", "[node][service]")
{
    // `emitIfSet` compares against the default and stays silent when they match,
    // which is right for every other flag and wrong for this one: the difference
    // `--reserve-cores=0` carries IS its presence. Registered without it, a service
    // that its operator told to hold nothing back would come up holding two cores
    // back on every start, with a command line that looks correct.
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6676";
    cfg.toolchains = { "/usr/bin/g++" };
    cfg.reservedCores = 0;

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);
    CHECK(std::ranges::contains(spec.arguments, std::string { "--reserve-cores=0" }));

    // And a configuration that never mentioned one emits nothing, so the class
    // default keeps applying rather than being frozen at install time.
    NodeConfig quiet = cfg;
    quiet.reservedCores.reset();
    auto const quietSpec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, quiet);
    CHECK_FALSE(std::ranges::any_of(quietSpec.arguments,
                                    [](std::string const& arg) { return FlagMatches(arg, "--reserve-cores"); }));
}

TEST_CASE("NodeConfig: a node class is parsed by name and refused by name", "[node][cli]")
{
    // Off `NodeClassTable`, so a class added there is accepted here without an edit.
    auto const dedicated = ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/cc", "--node-class=dedicated" });
    REQUIRE(dedicated.has_value());
    CHECK(dedicated->nodeClass == Distributed::NodeClass::Dedicated);

    // The default is the safe one rather than the common one.
    auto const unstated = ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/cc" });
    REQUIRE(unstated.has_value());
    CHECK(unstated->nodeClass == Distributed::NodeClass::Workstation);
    CHECK_FALSE(unstated->reservedCores.has_value());

    // Refused rather than defaulted, and the message names what would have worked:
    // a rejection that cannot say that cannot be acted on.
    auto const wrong = ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/cc", "--node-class=server" });
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().context.contains("workstation"));
    CHECK(wrong.error().context.contains("dedicated"));
}

TEST_CASE("NodeConfig: a reserve of zero parses, unlike a slot count of zero", "[node][cli]")
{
    // The two flags differ deliberately. Zero slots is a worker that can do nothing
    // and is refused; zero reserved cores is a real instruction -- drive this machine
    // to its last core -- and is distinct from not passing the flag at all.
    auto const none = ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/cc", "--reserve-cores=0" });
    REQUIRE(none.has_value());
    REQUIRE(none->reservedCores.has_value());
    CHECK(Unwrap(none->reservedCores) == 0U);

    CHECK_FALSE(ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/cc", "--slots=0" }).has_value());
}

TEST_CASE("NodeConfig: the discovery reply port is pinned by name and needs discovery", "[node][cli]")
{
    // Where a node is ANSWERED, which is never where it listens: every node on
    // the segment binds the beacon port, and only one socket sharing a port is
    // handed a unicast. Kernel-chosen by default -- the value nobody has to pick
    // -- and pinned only where a host firewall opens named ports and would
    // otherwise drop every challenge and proof.
    // It names itself in `--raft-peer`, which every node with a `--node-id` does --
    // discovery finds the OTHERS, and this node's own address is the half only it
    // knows. Without it the startup table refuses the whole command line before
    // reaching anything this case is about.
    auto const base = std::vector<char const*> { "--scheduler=s:1",
                                                 "--toolchain=/usr/bin/cc",
                                                 "--node-id=n1",
                                                 "--listen-raft=6680",
                                                 "--raft-peer=n1=10.0.0.1:6680",
                                                 "--cluster-key-file=cluster.key",
                                                 "--discovery=255.255.255.255:6681" };

    auto const unset = ParseNodeArgv(base);
    REQUIRE(unset.has_value());
    CHECK(unset->discoveryReplyPort == 0);

    auto const withPort = [&base](char const* flag) {
        auto args = base;
        args.push_back(flag);
        return ParseNodeArgv(args);
    };

    auto const parsed = withPort("--discovery-reply-port=6682");
    REQUIRE(parsed.has_value());
    CHECK(parsed->discoveryReplyPort == 6682);

    // Zero is not an answer here, unlike --reserve-cores: it is what the default
    // already is, so typing it means the operator got the flag wrong.
    CHECK_FALSE(withPort("--discovery-reply-port=0").has_value());

    // And pinning where nothing answers is a half-finished configuration rather
    // than an instruction, so it is refused at startup rather than ignored -- a
    // port reserved for a service that is off is a port nothing will ever bind.
    //
    // Dereferenced directly rather than through `Unwrap`: that helper is for
    // `std::optional`, which clang-tidy cannot see a REQUIRE guard through, and
    // `std::expected` does not go through it.
    CHECK_FALSE(StartupPolicyRejection(*parsed).has_value());

    auto orphaned = *parsed;
    orphaned.discoveryAddress.clear();
    CHECK(StartupPolicyRejection(orphaned).has_value());

    // Pointing both halves at one port is the configuration the whole fix exists
    // to prevent, so it is refused by name rather than left to fail at bind with
    // a message that reads as somebody else holding the port.
    auto collided = *parsed;
    collided.discoveryReplyPort = 6681;
    auto const refusal = StartupPolicyRejection(collided);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).contains("--discovery-reply-port"));
}

TEST_CASE("NodeConfig: a node is sized from its hardware and its class", "[node][capacity]")
{
    // The mapping `NodeCapacityOf` exists to make checkable: which facts come from
    // the operator and which from the machine. Getting it wrong is invisible — the
    // node registers, heartbeats and is simply sized wrong forever, with a command
    // line that reads correctly.
    NodeConfig cfg;

    SECTION("a developer's laptop keeps cores for its owner")
    {
        FakeHost const laptop { 8, 32ULL << 30 };
        auto const capacity = NodeCapacityOf(cfg, laptop, NoCacheTier);

        CHECK(capacity.logicalCores == 8);
        CHECK(capacity.totalMemoryBytes == (32ULL << 30));
        // Unstated, so the class decides — which is the distinction the optional
        // carries and the reason it is not a plain integer.
        CHECK(capacity.nodeClass == Distributed::NodeClass::Workstation);
        CHECK_FALSE(capacity.reserveIsExplicit);
        CHECK(Distributed::OfferableSlots(capacity, cfg.slots) == 6);
    }

    SECTION("a build server is driven to its limit")
    {
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        FakeHost const server { 128, 512ULL << 30 };

        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, server, NoCacheTier), cfg.slots) == 128);
    }

    SECTION("a machine whose cores outrun its memory is sized by the memory")
    {
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        FakeHost const cramped { 128, 32ULL << 30 };

        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, cramped, NoCacheTier), cfg.slots) == 32);
    }

    SECTION("the node's own cache is memory a compile cannot have")
    {
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        FakeHost const cramped { 128, 32ULL << 30 };
        auto const memoryTier = CacheTierOf(StorageTier::Memory, 8ULL << 30);

        auto const capacity = NodeCapacityOf(cfg, cramped, memoryTier);
        // Declared, so the scheduler at the other end reaches the same number.
        CHECK(capacity.reservedMemoryBytes == (8ULL << 30));
        // And it is the SAME record the leader renders the member's cache from, not
        // a second opinion about it: the reservation is that budget rather than a
        // number derived alongside it.
        CHECK(capacity.cache.tierBytesLimit == memoryTier.tierBytesLimit);
        // And the machine still reports the RAM it *has*: the dashboard prints this,
        // and a machine that appeared to shrink by the size of its own cache would be
        // a different lie.
        CHECK(capacity.totalMemoryBytes == (32ULL << 30));
        // 24 rather than 32. The old arithmetic offered a slot per gigabyte of total
        // RAM while the node held eight of those gigabytes itself -- forty gigabytes
        // of promises on a thirty-two gigabyte box.
        CHECK(Distributed::OfferableSlots(capacity, cfg.slots) == 24);
    }

    SECTION("a node that built no cache tier holds nothing back for one")
    {
        // The pairing no case used to make, and the whole of #167: a cache budget
        // that was asked for, and a tier that was never built. `--listen-cache=`
        // reaches it, and so does a DEFAULT cache port `fastcached` already holds on
        // the same machine -- neither of which touches `cacheMemoryBytes`, whose
        // default is a quarter of RAM. Sized from the flag, this node reserved eight
        // gigabytes for nothing and offered the fleet 24 slots instead of 32:
        // under-utilisation rather than breakage, and therefore silent forever.
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        // Set and then deliberately ignored: this line IS the regression guard, and
        // it is the only section here that touches the flag at all.
        cfg.cacheMemoryBytes = 8ULL << 30;
        FakeHost const cramped { 128, 32ULL << 30 };

        auto const capacity = NodeCapacityOf(cfg, cramped, NoCacheTier);
        CHECK(capacity.reservedMemoryBytes == 0);
        CHECK(Distributed::OfferableSlots(capacity, cfg.slots) == 32);
    }

    SECTION("a disk-only cache is disk, and takes no memory from a compile")
    {
        // `--cache-memory 0 --cache-dir <path>` is a real deployment, and the tier
        // it builds is resident nowhere -- which `StorageTierTable` now says rather
        // than each reader assuming. Summing every tier's budget into the memory
        // reservation would cost this node ten of its slots for a cache on disk.
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        FakeHost const cramped { 128, 32ULL << 30 };

        auto const capacity = NodeCapacityOf(cfg, cramped, CacheTierOf(StorageTier::Disk, 10ULL << 30));
        CHECK(capacity.reservedMemoryBytes == 0);
        CHECK(Distributed::OfferableSlots(capacity, cfg.slots) == 32);
    }

    SECTION("a resident tier with no ceiling is reserved as the whole machine")
    {
        // Zero is this vocabulary's UNBOUNDED, not its "nothing" -- the same two
        // meanings of that number `--cache-memory 0` already has a rule about. Read
        // as "reserve nothing", it would be the exact inverse of what the tier just
        // said: the node would offer every slot while its cache grew without a
        // ceiling. Reserved as the whole machine, `OfferableSlots`'s own floor
        // applies and the memory decides rather than the 128 cores.
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        FakeHost const cramped { 128, 32ULL << 30 };

        auto const capacity = NodeCapacityOf(cfg, cramped, CacheTierOf(StorageTier::Memory, 0));
        CHECK(capacity.reservedMemoryBytes == (32ULL << 30));
        CHECK(Distributed::OfferableSlots(capacity, cfg.slots) == 1);
    }

    SECTION("a typed reserve of zero is not the same as no reserve")
    {
        FakeHost const laptop { 8, 32ULL << 30 };

        cfg.reservedCores = 0;
        auto const explicitNone = NodeCapacityOf(cfg, laptop, NoCacheTier);
        CHECK(explicitNone.reserveIsExplicit);
        CHECK(Distributed::OfferableSlots(explicitNone, cfg.slots) == 8);

        cfg.reservedCores = 4;
        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, laptop, NoCacheTier), cfg.slots) == 4);
    }

    SECTION("an explicit slot count overrides every derivation")
    {
        cfg.slots = 20;
        FakeHost const laptop { 8, 32ULL << 30 };

        // With a cache tier big enough that every derivation would have clamped
        // below 20, so this passes only because the typed number wins outright.
        auto const capacity = NodeCapacityOf(cfg, laptop, CacheTierOf(StorageTier::Memory, 8ULL << 30));
        CHECK(Distributed::OfferableSlots(capacity, cfg.slots) == 20);
    }
}

TEST_CASE("NodeConfig: the node answers where the launcher looks", "[node][cli]")
{
    // Two programs agreeing on one constant, asserted here because that agreement is
    // the entire mechanism: it is what makes `fastcache-cc` find a local node with no
    // FASTCACHE_ADDR set, and nothing else would notice if one of them moved. The
    // symptom of a drift is not an error — the launcher connects to a closed port,
    // falls back, and every build is silently uncached.
    CHECK(NodeConfig {}.cacheListen == Cc::DefaultAddr);

    // Loopback, and that is the anti-leeching half rather than a preference: this
    // surface serves this machine's whole build output, so reaching it from the
    // network has to be something an operator typed.
    CHECK(NodeConfig {}.cacheListen.starts_with("127.0.0.1:"));

    // And it is on by default, because a local tier is what the program is for.
    CHECK(NodeConfig {}.cacheMemoryBytes > 0);
}

TEST_CASE("A dashboard that could never show a fleet is refused at startup", "[node][dashboard][policy]")
{
    // Every rule here describes a configuration that would start successfully and
    // then not work, refused at the one moment an operator is watching. Each
    // message names the flags, because "invalid" tells them nothing about what to
    // type instead.

    /// A node whose scheduler and admin surface are both configured, so only the
    /// dashboard rule under test can fire.
    auto const servingNode = [] {
        NodeConfig cfg;
        cfg.schedulerListen = "0.0.0.0:6678";
        cfg.fleetOpen = true;
        // `--fleet-open` admits other machines, and a node that admits them has to
        // be able to check the grants they present (#282). Present here so that only
        // the dashboard rule under test can fire.
        cfg.clusterKeyFile = "cluster.key";
        cfg.adminListen = "6677"; // a bare port, so loopback
        cfg.dashboard = true;
        return cfg;
    };

    SECTION("a dashboard with no admin surface to serve it on")
    {
        auto cfg = servingNode();
        cfg.adminListen.clear();

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--dashboard"));
        CHECK(Unwrap(refusal).contains("--admin-listen"));
    }

    SECTION("a dashboard on a node that never leads a fleet")
    {
        // A node running no scheduler has no registry to report, so the page could
        // only ever say it is not the leader -- which looks like a working
        // dashboard right up until somebody reads it.
        auto cfg = servingNode();
        cfg.schedulerListen.clear();

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--dashboard"));
        CHECK(Unwrap(refusal).contains("--listen-scheduler"));
    }

    SECTION("a credential nothing reads")
    {
        // A secret an operator went to the trouble of provisioning, read by
        // nobody. `--cluster-key-file` used to have a sibling rule and no longer
        // does -- its readers grew until the rule refused the configurations they
        // needed -- and this one survives because it does not have that problem:
        // the dashboard is either on or off, and that is a flag, not something a
        // tier resolves at startup.
        auto cfg = servingNode();
        cfg.dashboard = false;
        cfg.dashboardTokenFile = "dashboard.token";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--dashboard-token-file"));
    }

    SECTION("a certificate with no key, and a key with no certificate")
    {
        // Without this the node would start and serve the admin surface in the
        // clear while an operator believed it was encrypted.
        auto certOnly = servingNode();
        certOnly.tlsCertFile = "admin.crt";
        auto const certRefusal = StartupPolicyRejection(certOnly);
        REQUIRE(certRefusal.has_value());
        CHECK(Unwrap(certRefusal).contains("--tls-key"));

        auto keyOnly = servingNode();
        keyOnly.tlsKeyFile = "admin.key";
        auto const keyRefusal = StartupPolicyRejection(keyOnly);
        REQUIRE(keyRefusal.has_value());
        CHECK(Unwrap(keyRefusal).contains("--tls-cert"));

        // Both together is the configuration that works.
        auto both = servingNode();
        both.tlsCertFile = "admin.crt";
        both.tlsKeyFile = "admin.key";
        CHECK_FALSE(StartupPolicyRejection(both).has_value());
    }

    SECTION("a generated certificate and a named one contradict each other")
    {
        // Silently preferring either would serve an identity the operator did not
        // choose, which is the whole thing a certificate is for.
        auto cfg = servingNode();
        cfg.tlsSelfSigned = true;
        cfg.tlsCertFile = "admin.crt";
        cfg.tlsKeyFile = "admin.key";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--tls-self-signed"));
        CHECK(Unwrap(refusal).contains("--tls-cert"));
    }

    SECTION("TLS material with no admin surface to terminate it")
    {
        // The silent no-op shape again: a certificate nothing serves.
        auto selfSigned = servingNode();
        selfSigned.adminListen.clear();
        selfSigned.dashboard = false;
        selfSigned.tlsSelfSigned = true;
        auto const selfSignedRefusal = StartupPolicyRejection(selfSigned);
        REQUIRE(selfSignedRefusal.has_value());
        CHECK(Unwrap(selfSignedRefusal).contains("--admin-listen"));

        auto named = servingNode();
        named.adminListen.clear();
        named.dashboard = false;
        named.tlsCertFile = "admin.crt";
        named.tlsKeyFile = "admin.key";
        auto const namedRefusal = StartupPolicyRejection(named);
        REQUIRE(namedRefusal.has_value());
        CHECK(Unwrap(namedRefusal).contains("--admin-listen"));
    }

    SECTION("a generated certificate on its own is a working configuration")
    {
        // The whole point of the flag: an encrypted admin surface with nothing to
        // obtain first.
        auto cfg = servingNode();
        cfg.tlsSelfSigned = true;
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a fleet map on a public port with no credential")
    {
        // The page lists every member's hostname, endpoint and capacity. An
        // operator who bound it to the network is publishing that to whoever asks,
        // and HTTPS does not help: TLS authenticates the SERVER to the browser and
        // says nothing about who the browser is.
        auto cfg = servingNode();
        cfg.adminListen = "0.0.0.0:6677";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--dashboard-token-file"));

        // Naming a credential is what makes that configuration legal.
        cfg.dashboardTokenFile = "dashboard.token";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("loopback needs no credential, because reaching it means being on the machine")
    {
        // A bare port binds loopback, and the whole default rests on that.
        auto const cfg = servingNode();
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());

        // Spelled out, it is the same answer.
        auto explicitLoopback = servingNode();
        explicitLoopback.adminListen = "127.0.0.1:6677";
        CHECK_FALSE(StartupPolicyRejection(explicitLoopback).has_value());
    }
}

TEST_CASE("Naming the dashboard flags parses them", "[node][dashboard][cli]")
{
    auto const parsed = ParseNodeArgv({ "--scheduler=cache:6675",
                                        "--toolchain=/usr/bin/g++",
                                        "--admin-listen=6677",
                                        "--dashboard",
                                        "--dashboard-token-file=/etc/fastcached/dashboard.token",
                                        "--tls-cert=/etc/fastcached/admin.crt",
                                        "--tls-key=/etc/fastcached/admin.key" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->dashboard);
    CHECK(parsed->dashboardTokenFile == "/etc/fastcached/dashboard.token");
    CHECK(parsed->tlsCertFile == "/etc/fastcached/admin.crt");
    CHECK(parsed->tlsKeyFile == "/etc/fastcached/admin.key");
    CHECK_FALSE(parsed->tlsSelfSigned);

    // The other spelling of asking for TLS, which needs no material to name.
    auto const generated =
        ParseNodeArgv({ "--scheduler=cache:6675", "--toolchain=/usr/bin/g++", "--admin-listen=6677", "--tls-self-signed" });
    REQUIRE(generated.has_value());
    CHECK(generated->tlsSelfSigned);
    CHECK(generated->tlsCertFile.empty());

    // Off unless asked for, like every other surface this program serves.
    auto const bare = ParseNodeArgv({ "--scheduler=cache:6675", "--toolchain=/usr/bin/g++" });
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->dashboard);
    CHECK(bare->dashboardTokenFile.empty());
}

TEST_CASE("The in-memory tier defaults to a bounded share of the machine", "[node][config][cache]")
{
    auto const budget = NodeConfig {}.cacheMemoryBytes;

    // The same `DefaultMaxMemoryBytes()` the daemon uses, rather than a second
    // opinion about the same question. Asserted against the function rather than
    // against a literal, because a literal here is a test that passes for the wrong
    // reason on whichever machine happens to run it.
    CHECK(budget == DefaultMaxMemoryBytes());

    // The clamp is what makes a fraction safe in both directions: a small laptop
    // still gets a cache worth having, and a very large build server does not
    // quietly take a tenth of its RAM resident for a cache nobody asked for.
    constexpr std::uint64_t Floor = 512ULL * 1024 * 1024;
    constexpr std::uint64_t Ceiling = 8ULL * 1024 * 1024 * 1024;
    CHECK(budget >= Floor);
    CHECK(budget <= Ceiling);
}

TEST_CASE("The cache budget can be given as a share of RAM, and zero still turns it off", "[node][config][cache]")
{
    // The flag speaks its own default's vocabulary. Without this, "a quarter of RAM,
    // but half of that" means working the bytes out by hand for every machine.
    auto const half = ParseNodeArgv({ "--cache-memory=50%" });
    REQUIRE(half.has_value());
    CHECK(half->cacheMemoryBytes > 0);
    auto const quarter = ParseNodeArgv({ "--cache-memory=25%" });
    REQUIRE(quarter.has_value());
    // A half is twice a quarter whatever this machine has, give or take the integer
    // division -- which is the relationship worth asserting, since the absolute
    // numbers are the runner's.
    CHECK(half->cacheMemoryBytes >= (quarter->cacheMemoryBytes * 2) - 1);

    auto const bytes = ParseNodeArgv({ "--cache-memory=1g" });
    REQUIRE(bytes.has_value());
    CHECK(bytes->cacheMemoryBytes == 1024ULL * 1024 * 1024);

    // **Zero turns the tier off.** It is not "unbounded", which is what zero means
    // to `InMemoryLruStorage` -- the flag that turns a cache off once turned its
    // limit off instead, and a percentage default must not have quietly reopened
    // that: `25%` and `0` have to stay different things.
    auto const off = ParseNodeArgv({ "--cache-memory=0" });
    REQUIRE(off.has_value());
    CHECK(off->cacheMemoryBytes == 0);
}

TEST_CASE("A cache budget nobody set emits no flag, and a zero one does", "[node][config][cache]")
{
    // `--install-service` registers the COMMAND-LINE config, so a field left alone
    // must emit nothing and be re-derived on the machine the service runs on. That
    // matters more now the default follows host RAM: baking today's bytes into a
    // unit would freeze them across a memory upgrade or a VM resize.
    auto const untouched = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", Installable());
    CHECK(std::ranges::none_of(untouched.arguments, [](auto const& arg) { return arg.starts_with("--cache-memory"); }));

    auto off = Installable();
    off.cacheMemoryBytes = 0;
    off.cacheMemoryExplicit = true;
    auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", off);
    // Turning it off is a decision, so it survives into the unit rather than being
    // read as "unset".
    CHECK(std::ranges::any_of(spec.arguments, [](auto const& arg) { return arg == "--cache-memory=0"; }));

    // **The one this exists for.** An operator reads the startup line, types that
    // number back to pin it, and lands on a value equal to the default *on this
    // machine*. Emitting on "differs from the default" would drop it, and the
    // service would re-derive from RAM at every start -- so the budget moves under a
    // VM resize or a memory upgrade, for exactly the operator who pinned it.
    auto pinned = Installable();
    pinned.cacheMemoryBytes = NodeConfig {}.cacheMemoryBytes;
    pinned.cacheMemoryExplicit = true;
    auto const pinnedSpec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", pinned);
    CHECK(std::ranges::any_of(pinnedSpec.arguments, [&pinned](auto const& arg) {
        return arg == std::format("--cache-memory={}", pinned.cacheMemoryBytes);
    }));
}

TEST_CASE("Typing the cache budget is what marks it explicit", "[node][config][cache]")
{
    // The bit is set by the parse, not by the caller: every path that reaches a
    // service spec goes through `ParseOptionsInto`, so a flag an operator typed
    // carries its own provenance rather than depending on somebody remembering to
    // record it alongside.
    auto const typed = ParseNodeArgv({ "--cache-memory=1g" });
    REQUIRE(typed.has_value());
    CHECK(typed->cacheMemoryExplicit);

    auto const silent = ParseNodeArgv({ "--scheduler=s:1" });
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->cacheMemoryExplicit);
}

TEST_CASE("Typing the cache port is what marks it explicit, even at its default value", "[node][config][cache]")
{
    // The one spelling value equality cannot see: an operator reading the startup
    // line and typing the port back to pin it lands on the default exactly (#286).
    auto const pinned = ParseNodeArgv({ "--listen-cache=127.0.0.1:6674" });
    REQUIRE(pinned.has_value());
    CHECK(pinned->cacheListen == NodeConfig {}.cacheListen);
    CHECK(pinned->cacheListenExplicit);

    // Turning the tier off is a decision too, and an empty value is as far from the
    // default as a port is.
    auto const off = ParseNodeArgv({ "--listen-cache=" });
    REQUIRE(off.has_value());
    CHECK(off->cacheListenExplicit);

    auto const silent = ParseNodeArgv({ "--scheduler=s:1" });
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->cacheListenExplicit);
}

TEST_CASE("NodeConfig: a setting the operator pinned reaches the supervisor as pinned", "[node][service]")
{
    // Walked off the table rather than written out per flag, because the failure it
    // guards is an OMISSION: a row given an `explicitBit` and then emitted with
    // `emitIfSet` passes every other case in this file, since they all hand the
    // field a value that differs. That is #286 exactly -- the registration half of
    // it -- and a hand-written list is maintained by whoever forgot the emitter.
    //
    // The value is left at its DEFAULT while the bit is set, which is the shape no
    // value comparison can tell from silence, and the only shape that matters here.
    for (auto const& option: NodeOptions())
    {
        if (option.explicitBit == nullptr)
            continue;

        auto pinned = Installable();
        pinned.*option.explicitBit = true;
        INFO("flag: " << option.primary);

        auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", pinned);
        CHECK(std::ranges::any_of(spec.arguments,
                                  [&option](std::string const& arg) { return FlagMatches(arg, option.primary); }));
    }

    // Its converse, or "always emit it" would pass: a setting nobody named stays out
    // of the registration, so the machine's default is not frozen at install time.
    auto const silent = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", Installable());
    for (auto const& option: NodeOptions())
    {
        if (option.explicitBit == nullptr)
            continue;
        INFO("flag: " << option.primary);
        CHECK(std::ranges::none_of(silent.arguments,
                                   [&option](std::string const& arg) { return FlagMatches(arg, option.primary); }));
    }
}

TEST_CASE("NodeConfig: a cache port the operator pinned survives its own installer", "[node][service][cache]")
{
    // The round trip the platform rules require, asked of the PROVENANCE and not
    // only the value: the service is how this program actually runs, so a
    // registration that came back classified as defaulted would warn past a bind
    // failure at every boot forever (#286).
    auto pinned = Installable();
    pinned.cacheListen = NodeConfig {}.cacheListen;
    pinned.cacheListenExplicit = true;

    auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", pinned);
    auto const reparsed = ReparseSpec(spec);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->cacheListen == pinned.cacheListen);
    CHECK(reparsed->cacheListenExplicit);
}

TEST_CASE("A flag whose value other machines read refuses one that is not text", "[node][config][utf8]")
{
    // Which flags carry text the fleet reads is a COLUMN of the option table, and
    // this pins the column rather than the parser -- `Cli/Options_test.cpp` covers
    // what `ParseUtf8Text` does. Every one of these ends up in a peer's
    // `ClusterState` or in a registration: `--advertise` and `--bind` become the
    // endpoint clients dial, `--node-id` and `--raft-peer` a member's identity and
    // the address its peers dial it at.
    //
    // Accepted here, such a value is refused by `SchedulerService::Register` on
    // every heartbeat forever, and the operator's only recovery is to rename the
    // thing -- on Windows, for a byte that was never going to survive `argv` in the
    // first place (issue #155).
    // A row per flag rather than one value for all four, because `--raft-peer` also
    // has a GRAMMAR (`<id>=<host>:<port>`, #168) and a bare `grün` is refused by
    // that instead -- which would pass this case for a reason that has nothing to do
    // with encoding, and would keep passing the day the encoding check was removed.
    // Each row is therefore the same token twice, differing only in how the umlaut
    // is spelled.
    struct Row
    {
        char const* flag;   ///< The flag under test.
        char const* latin1; ///< Its value carrying a lone 0xFC -- not UTF-8.
        char const* utf8;   ///< The same value, spelled in UTF-8.
    };

    constexpr std::array<Row, 4> Rows { {
        { .flag = "--advertise",
          .latin1 = "gr\xFC"
                    "n",
          .utf8 = "gr\xC3\xBC"
                  "n" },
        { .flag = "--bind",
          .latin1 = "gr\xFC"
                    "n",
          .utf8 = "gr\xC3\xBC"
                  "n" },
        { .flag = "--node-id",
          .latin1 = "gr\xFC"
                    "n",
          .utf8 = "gr\xC3\xBC"
                  "n" },
        { .flag = "--raft-peer",
          .latin1 = "gr\xFC"
                    "n=h:1234",
          .utf8 = "gr\xC3\xBC"
                  "n=h:1234" },
    } };

    for (auto const& row: Rows)
    {
        INFO("flag: " << row.flag);
        auto const refused = ParseNodeArgv({ row.flag, row.latin1 });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().field == row.flag);

        // The same value in UTF-8 is accepted: the rule is about encoding, not about
        // ASCII, and a fleet whose members may only name themselves in ASCII would
        // be a second restriction nobody announced.
        CHECK(ParseNodeArgv({ row.flag, row.utf8 }).has_value());
    }
}

TEST_CASE("A pinned toolchain fingerprint is in the column; the compiler is not", "[node][config][utf8]")
{
    // A fingerprint is what a scheduler matches a client's request against byte for
    // byte, and since #141 a registration carrying one that is not valid UTF-8 is
    // refused -- on every heartbeat, forever. Decided by the PARSE rather than where
    // the two halves are used, so `--install-service` cannot bake such a value into
    // a registration that then fails at every boot with nobody watching: it returns
    // before a toolchain is ever resolved.
    auto const refused = ParseNodeArgv({ "--toolchain=gr\xFC"
                                         "n=/usr/bin/g++" });
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().field == "--toolchain");

    // The bare form pins nothing, so there is nothing to refuse: the node computes
    // the fingerprint itself, in hex, which cannot fail this.
    CHECK(ParseNodeArgv({ "--toolchain=/usr/bin/g++" }).has_value());

    // And a pinned fingerprint spelled in UTF-8 is accepted, umlaut and all.
    CHECK(ParseNodeArgv({ "--toolchain=gr\xC3\xBC"
                          "n=/usr/bin/g++" })
              .has_value());
}

TEST_CASE("A cluster change an operator commits is in the column; forgetting is not", "[node][config][utf8]")
{
    // `--cluster-admit` and `--cluster-set` COMMIT their operand through consensus:
    // it lands in every peer's ClusterState and is rendered into /fleet.json. A
    // consensus entry is applied after it is committed, with nobody left to refuse
    // it, so the CLI is the last place a person can be told.
    auto const admit = ParseNodeArgv({ "--cluster-admit=gr\xFC"
                                       "n=host:6677" });
    REQUIRE_FALSE(admit.has_value());
    CHECK(admit.error().field == "--cluster-admit");

    auto const set = ParseNodeArgv({ "--cluster-set=name=gr\xFC"
                                     "n" });
    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().field == "--cluster-set");

    // `--cluster-forget` is deliberately NOT in the column, and this pins the
    // omission rather than tolerating it. Its operand IS the offending id, so a
    // check covering it would make a member admitted by an older peer impossible to
    // remove -- and it would count towards quorum forever (issue #159).
    auto const forget = ParseNodeArgv({ "--cluster-forget=gr\xFC"
                                        "n" });
    REQUIRE(forget.has_value());
    CHECK(forget->cluster.key
          == "gr\xFC"
             "n");
}

TEST_CASE("A path-valued flag is deliberately not in that column", "[node][config][utf8]")
{
    // On a host that transcodes nothing, a legacy filename is a perfectly good
    // filename. Refusing `--cache-dir` for the reason `--advertise` is refused would
    // break a working node over a rule about a field it is not.
    //
    // POSIX only, and not because Windows behaves differently here -- because the
    // case cannot arise there. The OS holds a command line as UTF-16 and hands this
    // process the UTF-8 form of it (the declared code page), so `argv` on Windows is
    // always valid UTF-8 and there is no legacy spelling for this flag to receive.
#if !defined(_WIN32)
    auto const parsed = ParseNodeArgv({ "--cache-dir=/var/cache/gr\xFC"
                                        "n" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->cacheDir
          == "/var/cache/gr\xFC"
             "n");

    // The COMPILER half of a pinned `--toolchain` is a path too, and is asked
    // nothing for the same reason -- only the fingerprint before the first `=`
    // travels.
    CHECK(ParseNodeArgv({ "--toolchain=deadbeef=/opt/gr\xFC"
                          "n/bin/g++" })
              .has_value());
#endif

    // Everywhere: a non-ASCII path is a path, not a fleet-visible identity, and
    // nothing here narrows it.
    auto const utf8 = ParseNodeArgv({ "--cache-dir=/var/cache/gr\xC3\xBC"
                                      "n" });
    REQUIRE(utf8.has_value());
    CHECK_FALSE(utf8->cacheDir.empty());
}

TEST_CASE("The drain bound is an operator's to set, zero included", "[node][config]")
{
    // A flag rather than a constant, because the right value is how long THIS site's
    // compiles legitimately run -- and a compile-time answer on a binary whose
    // `--install-service` replays its command line forever is one nobody can move
    // afterwards (#239).
    CHECK(NodeConfig {}.drainTimeoutSeconds == 30);

    auto const set = ParseNodeArgv({ "--drain-timeout=90" });
    REQUIRE(set.has_value());
    CHECK(set->drainTimeoutSeconds == 90);

    // Zero is a real answer and a DIFFERENT one from omitting the flag: it is what
    // this did before the bound existed, kept sayable for an operator who would
    // rather have the supervisor decide.
    auto const forever = ParseNodeArgv({ "--drain-timeout=0" });
    REQUIRE(forever.has_value());
    CHECK(forever->drainTimeoutSeconds == 0);

    // Refused by the row's own grammar rather than clamped somewhere downstream.
    CHECK_FALSE(ParseNodeArgv({ "--drain-timeout=soon" }).has_value());
}

TEST_CASE("NodeConfig: a cluster key is never refused for having no reader", "[node][policy][lease]")
{
    // A rule here used to refuse `--cluster-key-file` unless something read it, and
    // it was wrong twice for the same reason: each time a new reader appeared, the
    // rule refused the configuration that reader needed.
    //
    // It began as "unless --discovery". Then the scheduler started SIGNING lease
    // grants with the key (#281), and the rule turned a correct scheduler into a node
    // that would not start; it was widened to name the scheduler too. Then the WORKER
    // became a reader (#282) -- and a worker is exactly the node that runs neither of
    // the other two surfaces, so the rule refused the configuration the rule below
    // requires.
    //
    // There is no third narrowing, which is why the rule is gone rather than widened
    // again: whether a worker tier exists depends on what `--toolchain` and discovery
    // resolve to on the machine, which is not a fact this table can see.
    SECTION("a scheduler alone")
    {
        auto cfg = Installable();
        cfg.schedulerListen = "0.0.0.0:6678";
        cfg.fleetOpen = true;
        cfg.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a plain worker, which is what the last narrowing refused")
    {
        // No scheduler, no discovery, no consensus. This is the shape the previous
        // rule rejected and the shape the lease rule below makes mandatory for any
        // worker admitting a peer on another machine.
        auto cfg = Installable();
        cfg.fleetMembers = { "10.0.0.1:6676" };
        cfg.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a node reaching nothing but itself")
    {
        // Provisioning a key here reads no worse than provisioning one for a machine
        // that is about to be given peers, and refusing it bought nothing.
        auto cfg = Installable();
        cfg.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }
}

TEST_CASE("NodeConfig: the lease rule permits every flag provenance now emits", "[node][policy][lease][service]")
{
    // The one genuinely new interaction between #282 and #286, and it is new in one
    // direction only: `emitIfExplicit` makes a TYPED DEFAULT reach the supervisor
    // where it used to be dropped, so a registration now carries flags a startup rule
    // never had to see before. A rule that refused one of them would turn a working
    // install into a service that registers cleanly and refuses at every boot.
    //
    // Asserted rather than reasoned about. The lease rule reads `clusterKeyFile`,
    // `bindAddress` and the membership fields and none of the cache ones, so by
    // inspection it cannot refuse these -- but "by inspection" is what this file
    // exists to replace, and the next rule added to either table gets this case for
    // free.
    auto cfg = Installable();
    cfg.fleetMembers = { "10.0.0.1:6676" };
    cfg.clusterKeyFile = "cluster.key"; // the lease rule's own requirement (#282)

    // Both provenance-bearing flags typed at exactly their defaults, which is the
    // whole point of `explicitBit`: the value says nothing, the fact that it was
    // typed says everything.
    NodeConfig const defaults;
    cfg.cacheListen = defaults.cacheListen;
    cfg.cacheListenExplicit = true;
    cfg.cacheMemoryBytes = defaults.cacheMemoryBytes;
    cfg.cacheMemoryExplicit = true;

    // It installs.
    CHECK_FALSE(NodeInstallRejection(cfg).has_value());

    // And the registration really does carry them -- otherwise this case would pass
    // for the wrong reason, having asserted a rule permits flags that were never
    // emitted.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);
    CHECK(std::ranges::any_of(spec.arguments, [](std::string const& a) { return a.starts_with("--listen-cache="); }));
    CHECK(std::ranges::any_of(spec.arguments, [](std::string const& a) { return a.starts_with("--cache-memory="); }));

    // The half that actually bites: what a supervisor replays has to pass the
    // startup rules at every boot, not merely at install time.
    auto const reparsed = ReparseSpec(spec);
    REQUIRE(reparsed.has_value());
    CHECK_FALSE(StartupPolicyRejection((*reparsed)).has_value());

    // And provenance survives the round trip, or the second boot silently drops what
    // the first one was told.
    CHECK((*reparsed).cacheListenExplicit);
    CHECK((*reparsed).cacheMemoryExplicit);
}

TEST_CASE("NodeConfig: a worker that admits other machines needs a key to check their grants", "[node][policy][lease]")
{
    // The scheduler signs a grant; a worker with no key cannot check the signature,
    // so its compile port serves whoever reaches it (#282). A STARTUP refusal rather
    // than a per-request fallback, because "no key, so no check" decided per request
    // leaves the port open with every refusal counter at zero -- a fleet that looks
    // healthy from both ends.
    SECTION("a listed peer on another machine")
    {
        auto cfg = Installable();
        cfg.fleetMembers = { "10.0.0.1:6676" };

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--cluster-key-file"));
    }

    SECTION("--fleet-open, which admits every machine there is")
    {
        auto cfg = Installable();
        cfg.fleetOpen = true;

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--cluster-key-file"));
    }

    SECTION("consensus, whose member set grows to machines nobody typed")
    {
        // The admitted set is published into the same oracle the compile port
        // consults, so a clustered node's peers reach it without appearing in
        // `--fleet-member` at all.
        auto bootstrapping = Installable();
        bootstrapping.nodeId = "n1";
        bootstrapping.raftListen = "6680";
        bootstrapping.raftPeers = { Peer("n1=10.0.0.1:6680"), Peer("n2=10.0.0.2:6680") };
        REQUIRE(StartupPolicyRejection(bootstrapping).has_value());
        CHECK(Unwrap(StartupPolicyRejection(bootstrapping)).contains("--cluster-key-file"));

        // And a node waiting to be admitted, which is the shape the peer list cannot
        // speak for: a joiner names only ITSELF, on loopback here, and every machine
        // it will admit arrives from the cluster that admits it.
        auto joining = Installable();
        joining.nodeId = "n1";
        joining.raftListen = "6680";
        joining.raftJoin = true;
        joining.raftPeers = { Peer("n1=127.0.0.1:6680") };
        REQUIRE(StartupPolicyRejection(joining).has_value());
        CHECK(Unwrap(StartupPolicyRejection(joining)).contains("--cluster-key-file"));
    }

    SECTION("a compile port bound to loopback keeps working, whatever its policy")
    {
        // The other half of the predicate, and the one this repository's own e2e
        // harnesses depend on: they run whole fleets of loopback nodes under
        // `--fleet-open`, where "admit everybody" reaches nobody because the socket
        // answers on 127.0.0.1 alone. Refusing that would refuse a configuration in
        // which no other machine can dial the compile port at all.
        auto loopbackBound = Installable();
        loopbackBound.bindAddress = "127.0.0.1";
        loopbackBound.fleetOpen = true;
        CHECK_FALSE(StartupPolicyRejection(loopbackBound).has_value());

        // And the default bind is the WILDCARD, so the same node without that flag
        // is refused -- which is what makes the check worth having rather than
        // something an ordinary deployment slips past.
        auto wildcardBound = Installable();
        wildcardBound.fleetOpen = true;
        CHECK(wildcardBound.bindAddress == "0.0.0.0");
        CHECK(StartupPolicyRejection(wildcardBound).has_value());
    }

    SECTION("a node that admits only its own machine keeps working")
    {
        // The single-machine install, and the reason this rule is scoped to remote
        // peers rather than to "is a key configured": a process on this host already
        // has this host's compiler, so a lease check there escalates nobody.
        // Refusing it would break every developer's laptop to prevent nothing.
        CHECK_FALSE(StartupPolicyRejection(Installable()).has_value());

        auto loopback = Installable();
        loopback.fleetMembers = { "127.0.0.1", "127.0.0.2:6676", "[::1]:6676" };
        CHECK_FALSE(StartupPolicyRejection(loopback).has_value());
    }

    SECTION("and a key is all it takes")
    {
        auto cfg = Installable();
        cfg.fleetMembers = { "10.0.0.1:6676" };
        cfg.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }
}

// ---------------------------------------------------------------------------
// The configuration FILE (#291). Before it, this worker was configured by a bag
// of command-line arguments in a shell fragment the systemd unit expanded, and
// nothing anywhere said which of the two mechanisms won.

namespace
{
/// Apply a file and then a command line, the way `main` does.
///
/// It calls the very function `main` calls -- a helper that re-implemented the
/// three steps would pass while the binary did something else, which is the
/// shape "a reclaimer nothing constructs" takes here.
/// @param settings What the file said.
/// @param args The command line, program name already removed.
/// @return The merged configuration, or the refusal.
[[nodiscard]] std::expected<NodeConfig, ConfigError> FromFileAndArgv(std::vector<YamlSetting> const& settings,
                                                                     std::vector<char const*> const& args)
{
    NodeConfig cfg;
    return ApplyNodeConfiguration(settings, std::filesystem::path { "/etc/n.yaml" }, args, cfg).transform([&cfg] {
        return cfg;
    });
}

[[nodiscard]] YamlSetting Setting(std::string key, std::vector<std::string> values)
{
    return YamlSetting { .key = std::move(key), .values = std::move(values), .line = 1 };
}
} // namespace

TEST_CASE("NodeConfig: every row is reachable from a file or named as one that is not", "[node][config]")
{
    // The compile-time guard beside the table proves this for the build; this is
    // the same claim stated where somebody reading the tests will find it, and it
    // also pins the SHAPE of the exclusions -- a row a file may not carry must be
    // one a file could not sensibly express, which for every one of them today
    // means it ends the process or describes how the process was started.
    auto const keyed = std::ranges::count_if(NodeOptions(), [](auto const& row) { return !row.yamlKey.empty(); });
    CHECK(keyed > 30);

    // No key is spelled like its flag: the mapping is snake_case of the flag, and
    // a leading `--` in a YAML key would be a setting nobody could type.
    for (auto const& row: NodeOptions())
    {
        INFO("row: " << row.primary);
        CHECK_FALSE(row.yamlKey.starts_with("-"));
        CHECK_FALSE(row.yamlKey.contains('-'));
    }
}

TEST_CASE("NodeConfig: a setting in the file takes effect", "[node][config]")
{
    auto const merged = FromFileAndArgv({ Setting("scheduler", { "cache.internal:6675" }),
                                          Setting("slots", { "9" }),
                                          Setting("toolchain", { "/usr/bin/g++", "/usr/bin/clang++" }),
                                          Setting("no_toolchain_discovery", { "true" }) },
                                        {});

    REQUIRE(merged.has_value());
    CHECK(merged->scheduler == "cache.internal:6675");
    CHECK(merged->slots == 9);
    CHECK(merged->toolchains == std::vector<std::string> { "/usr/bin/g++", "/usr/bin/clang++" });
    CHECK_FALSE(merged->toolchainDiscovery);
}

TEST_CASE("NodeConfig: the command line wins over the file", "[node][config]")
{
    auto const merged = FromFileAndArgv({ Setting("scheduler", { "from-file:6675" }), Setting("slots", { "9" }) },
                                        { "--scheduler=from-argv:6675" });

    REQUIRE(merged.has_value());
    CHECK(merged->scheduler == "from-argv:6675");
    // And a setting the command line did NOT name is still the file's: precedence
    // is per setting, not per source.
    CHECK(merged->slots == 9);
}

TEST_CASE("NodeConfig: a command line naming a toolchain replaces the file's list", "[node][config]")
{
    // The worker's toolchain set is an OVERRIDE, so extending it is the one
    // behaviour that must not happen: a `--toolchain` meant to pin this run to one
    // compiler would otherwise add it to whatever the file already served.
    auto const merged =
        FromFileAndArgv({ Setting("toolchain", { "/usr/bin/g++", "/usr/bin/clang++" }) }, { "--toolchain=/usr/bin/tcc" });

    REQUIRE(merged.has_value());
    CHECK(merged->toolchains == std::vector<std::string> { "/usr/bin/tcc" });
}

TEST_CASE("NodeConfig: a file naming an unknown setting refuses to start", "[node][config]")
{
    auto const merged = FromFileAndArgv({ Setting("schedular", { "typo:6675" }) }, {});

    REQUIRE_FALSE(merged.has_value());
    CHECK(merged.error().code == ConfigErrorCode::UnknownKey);
    CHECK(merged.error().field == "schedular");
    CHECK(merged.error().source == "/etc/n.yaml");
}

TEST_CASE("NodeConfig: a file may not name a one-shot verb or a startup fact", "[node][config]")
{
    // Each of these is a decision taken once. A file is read at EVERY start, so a
    // key for one would replay it forever -- a worker that re-registers itself, or
    // asks the cluster a question, instead of serving.
    for (auto const& key: { "install_service",
                            "uninstall_service",
                            "migrate_cache",
                            "cluster_forget",
                            "service_name",
                            "service_scope",
                            "daemon",
                            "config",
                            "help",
                            "version",
                            "print_surfaces" })
    {
        auto const merged = FromFileAndArgv({ Setting(key, { "x" }) }, {});
        INFO("key: " << key);
        REQUIRE_FALSE(merged.has_value());
        CHECK(merged.error().code == ConfigErrorCode::UnknownKey);
    }
}

TEST_CASE("NodeConfig: a registration carries the config path and not the file's settings", "[node][service]")
{
    // A registration replays its arguments at every start. Baking in what the FILE
    // said would freeze one reading of that file into launch arguments that then
    // outrank the file itself: the operator edits it, restarts the service, and
    // nothing changes, with no error anywhere. So the spec is built from the
    // command-line-only parse, and what it carries about the file is the PATH.
    auto cfg = Installable();
    // RELATIVE, which is the interesting input: a service does not inherit the
    // installing shell's working directory, so a relative path captured at install
    // time resolves somewhere else at every start. What is asserted is therefore the
    // RULE -- made absolute, and the flag and the field agreeing -- and not a
    // literal, which would be a POSIX string on a test that also runs on Windows.
    cfg.configPath = "node.yaml";

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);

    std::error_code ec;
    auto const expected = std::filesystem::absolute(cfg.configPath, ec).string();
    REQUIRE_FALSE(ec);
    REQUIRE(expected != cfg.configPath);

    CHECK(std::ranges::any_of(
        spec.arguments, [&expected](auto const& argument) { return argument == std::format("--config={}", expected); }));

    // The same value on the field, so `InlineCredentialRejection` names the file the
    // service was actually given rather than one nobody passed. Two spellings of one
    // path in one registration is a refusal pointing somewhere the service never
    // looks.
    CHECK(spec.configPath == expected);
}

TEST_CASE("NodeConfig: a registration for a worker given no file names none", "[node][service]")
{
    // Empty is a real answer: the worker repeats the machine-wide lookup at every
    // start, which is what lets a package replace that file without touching the
    // registration. Emitting a resolved path instead would pin the service to
    // whatever the lookup found on the day somebody ran the installer.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable());

    CHECK(std::ranges::none_of(spec.arguments, [](auto const& argument) { return argument.starts_with("--config="); }));
    CHECK(spec.configPath.empty());
}

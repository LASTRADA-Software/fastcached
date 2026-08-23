// SPDX-License-Identifier: Apache-2.0
#include "LauncherCli.hpp"
#include "NodeConfig.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
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

/// Parse an argv fragment into a `NodeConfig`, the way `main` does.
///
/// A local helper rather than a shared one because the two cases below are the only
/// callers: everything else here builds a config directly, since what it is testing
/// is what comes back OUT of one.
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
    });

    // A configuration in which no field holds its default, so every emitter fires.
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6676";
    cfg.bindAddress = "10.0.0.4";
    cfg.port = 7777;
    cfg.toolchains = { "/usr/bin/g++", "abc123=/usr/bin/clang++" };
    cfg.slots = 12;
    cfg.nodeClass = Distributed::NodeClass::Dedicated;
    cfg.reservedCores = 3;
    cfg.adminListen = "0.0.0.0:6677";
    cfg.schedulerListen = "0.0.0.0:6678";
    cfg.fleetMembers = { "10.0.0.1:6676", "10.0.0.2:6676" };
    cfg.fleetOpen = true;
    // Not 256 MiB: that is the default now, and this case exists to give every
    // field a value differing from its default so every emitter fires.
    cfg.cacheMemoryBytes = 512 * 1024 * 1024;
    cfg.cacheDir = "cache";
    cfg.cacheListen = "127.0.0.1:6679";
    cfg.upstream = "cache.internal:6674";
    cfg.nodeId = "n1";
    cfg.raftListen = "0.0.0.0:6680";
    cfg.raftPeers = { "n1=10.0.0.4:6680", "n2=10.0.0.5:6680" };
    cfg.clusterDir = "cluster";
    cfg.clusterId = "fleet-a";
    cfg.discoveryAddress = "255.255.255.255:6681";
    cfg.clusterKeyFile = "cluster.key";
    cfg.logLevel = LogLevel::Debug;
    cfg.pidfile = "worker.pid";

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

TEST_CASE("NodeConfig: a registration that could not work is refused", "[node][service]")
{
    // Each of these produces a service that registers cleanly and then cannot do
    // its job, which is the worst shape this system has: the operator is told it
    // was installed, and nothing at any later point says otherwise.
    CHECK(!NodeServiceRejection(Installable()).has_value());

    auto noScheduler = Installable();
    noScheduler.scheduler.clear();
    CHECK(NodeServiceRejection(noScheduler).has_value());

    auto noToolchain = Installable();
    noToolchain.toolchains.clear();
    CHECK(NodeServiceRejection(noToolchain).has_value());

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
        // Not 256 MiB: that is the default now, and this case exists to give every
        // field a value differing from its default so every emitter fires.
        cfg.cacheMemoryBytes = 512 * 1024 * 1024;
        cfg.cacheDir = "cache";
        cfg.cacheListen = "127.0.0.1:6679";
        cfg.upstream = "cache.internal:6674";
        cfg.fleetMembers = { "10.0.0.1:6676" };

        REQUIRE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a policy with nothing to consult it")
    {
        // The mirror image: membership named on a node running no scheduler. Nothing
        // reads it, so an operator who believes they have restricted the fleet has
        // not, and no counter anywhere would say so.
        NodeConfig cfg;
        cfg.fleetMembers = { "10.0.0.1:6676" };

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--listen-scheduler"));
    }

    SECTION("the working shapes are accepted")
    {
        NodeConfig listed;
        listed.schedulerListen = "0.0.0.0:6678";
        listed.fleetMembers = { "10.0.0.1:6676" };
        CHECK_FALSE(StartupPolicyRejection(listed).has_value());

        NodeConfig open;
        open.schedulerListen = "0.0.0.0:6678";
        open.fleetOpen = true;
        CHECK_FALSE(StartupPolicyRejection(open).has_value());

        // And a worker running no scheduler at all -- by far the common case -- is
        // untouched by any of this.
        CHECK_FALSE(StartupPolicyRejection(NodeConfig {}).has_value());
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
        auto const capacity = NodeCapacityOf(cfg, laptop);

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

        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, server), cfg.slots) == 128);
    }

    SECTION("a machine whose cores outrun its memory is sized by the memory")
    {
        cfg.nodeClass = Distributed::NodeClass::Dedicated;
        FakeHost const cramped { 128, 32ULL << 30 };

        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, cramped), cfg.slots) == 32);
    }

    SECTION("a typed reserve of zero is not the same as no reserve")
    {
        FakeHost const laptop { 8, 32ULL << 30 };

        cfg.reservedCores = 0;
        auto const explicitNone = NodeCapacityOf(cfg, laptop);
        CHECK(explicitNone.reserveIsExplicit);
        CHECK(Distributed::OfferableSlots(explicitNone, cfg.slots) == 8);

        cfg.reservedCores = 4;
        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, laptop), cfg.slots) == 4);
    }

    SECTION("an explicit slot count overrides every derivation")
    {
        cfg.slots = 20;
        FakeHost const laptop { 8, 32ULL << 30 };

        CHECK(Distributed::OfferableSlots(NodeCapacityOf(cfg, laptop), cfg.slots) == 20);
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

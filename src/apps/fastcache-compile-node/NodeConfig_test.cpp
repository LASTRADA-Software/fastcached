// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

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
    cfg.adminListen = "0.0.0.0:6677";
    cfg.schedulerListen = "0.0.0.0:6678";
    cfg.fleetMembers = { "10.0.0.1:6676", "10.0.0.2:6676" };
    cfg.fleetOpen = true;
    cfg.cacheMemoryBytes = 256 * 1024 * 1024;
    cfg.cacheDir = "cache";
    cfg.cacheListen = "127.0.0.1:6679";
    cfg.upstream = "cache.internal:6674";
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

        auto const refusal = SchedulerPolicyRejection(cfg);
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
        cfg.cacheMemoryBytes = 256 * 1024 * 1024;
        cfg.cacheDir = "cache";
        cfg.cacheListen = "127.0.0.1:6679";
        cfg.upstream = "cache.internal:6674";
        cfg.fleetMembers = { "10.0.0.1:6676" };

        REQUIRE(SchedulerPolicyRejection(cfg).has_value());
    }

    SECTION("a policy with nothing to consult it")
    {
        // The mirror image: membership named on a node running no scheduler. Nothing
        // reads it, so an operator who believes they have restricted the fleet has
        // not, and no counter anywhere would say so.
        NodeConfig cfg;
        cfg.fleetMembers = { "10.0.0.1:6676" };

        auto const refusal = SchedulerPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--listen-scheduler"));
    }

    SECTION("the working shapes are accepted")
    {
        NodeConfig listed;
        listed.schedulerListen = "0.0.0.0:6678";
        listed.fleetMembers = { "10.0.0.1:6676" };
        CHECK_FALSE(SchedulerPolicyRejection(listed).has_value());

        NodeConfig open;
        open.schedulerListen = "0.0.0.0:6678";
        open.fleetOpen = true;
        CHECK_FALSE(SchedulerPolicyRejection(open).has_value());

        // And a worker running no scheduler at all -- by far the common case -- is
        // untouched by any of this.
        CHECK_FALSE(SchedulerPolicyRejection(NodeConfig {}).has_value());
    }
}

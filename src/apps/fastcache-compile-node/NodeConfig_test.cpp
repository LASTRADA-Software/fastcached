// SPDX-License-Identifier: Apache-2.0
#include "LauncherCli.hpp"
#include "NodeConfig.hpp"
#include "NodeSurfaces.hpp"
#include "NodeToolchains.hpp"

#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Config/SecretExposureWatcher.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_all.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/PathFlagCoverage.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
/// A configuration a supervisor could actually be handed.
/// @return A worker that would start.
/// A scheduler endpoint for fixtures that need one to be startable at all.
///
/// **`--scheduler` is required of EVERY node**, a pure `--serve-scheduler` included --
/// it names itself. Until #386 that rule was an inline `if` in `main.cpp`, which is in
/// no test target, so a fixture could describe a configuration the binary refuses to
/// start and nothing here could tell. Several did.
///
/// Where a reload fixture uses it, it has to appear in BOTH the file and the live
/// configuration and with the SAME value: present in only one, it reads as a change to
/// an unreloadable field and is refused for that instead -- a green test for the wrong
/// reason.
constexpr std::string_view SchedulerEndpoint = "scheduler.internal:6675";

/// What a node running `--serve-scheduler` names as its own scheduler: itself.
///
/// **Not `SchedulerEndpoint`, and the difference decides which rows fire.** A REMOTE
/// scheduler plus a membership policy is what the three advertise rows are about -- such
/// a node has told peers to dial it, so its `--advertise` has to be an address they can
/// reach -- and handing a scheduler-node fixture a remote endpoint trips those rows
/// instead of whatever it was written to test.
///
/// A scheduler registering with itself over loopback is the documented shape
/// (`--serve-scheduler ... --scheduler=127.0.0.1:6675`), and it is what makes
/// `SchedulerIsRemote` false.
///
/// Found by giving the scheduler fixtures a REMOTE endpoint and watching the dashboard
/// and fleet-policy cases go red on the advertise rows -- so the two constants are not
/// a tidiness preference, and picking the wrong one does not fail where you are looking.
constexpr std::string_view SelfScheduler = "127.0.0.1:6675";

[[nodiscard]] NodeConfig Installable()
{
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    // BOTH halves, because since #290 stage 3 one without the other is not an
    // installable worker. `--advertise` alone leaves `--listen-node` on its loopback
    // default, so the node would tell peers to dial an address it never accepts on --
    // which `StartupPolicyRejection` now refuses by name. A fixture called
    // `Installable` that no longer installs would have every case built on it failing
    // for a reason none of them is about.
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.advertise = "worker-01.internal:6674";
    cfg.toolchains = { "/usr/bin/g++" };

    // The provenance bits for what this fixture TYPES (#713). Since emission follows
    // whether the operator said a thing rather than whether its value differs from a
    // default, a fixture that assigns values and no bits models an operator who
    // typed nothing -- and every registration built from it would come back almost
    // empty. This fixture is named `Installable`, so it has to model somebody who
    // actually installed.
    //
    // `nodeListenExplicit` is deliberately NOT set here even though `nodeListen` is.
    // That bit is not bookkeeping: it decides whether a bind failure is FATAL
    // (#286), so cases that care set it themselves and say why. A fixture turning it
    // on for everybody would change what those cases are testing without their
    // authors ever seeing it.
    cfg.schedulerExplicit = true;
    cfg.advertiseExplicit = true;
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

TEST_CASE("The node's credential is a secret and nothing else", "[node][config]")
{
    // `--requirepass` fills `token`, and there is no username beside it. One was
    // DECLARED beside it -- parsed by nothing, read by nothing, emitted by nothing --
    // and the reason that was worth removing rather than leaving is adjacency: a dead
    // field next to a live credential is what somebody later wires up on the
    // assumption it was always meant to work, and a half-wired username on an
    // authentication path is worse than no username at all (#385).
    //
    // What this case can assert is the shape that half-wiring would take. A member
    // nothing parses is not expressible as a test in this language -- the guard for
    // that is the table itself, since a setting only exists here by having a row --
    // so the assertion is that no such row has appeared, in either of the two
    // spellings somebody reaching for one would try.
    NodeConfig cfg;
    std::array const args { "--requirepass=hunter2" };
    REQUIRE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg).has_value());
    CHECK(cfg.token == "hunter2");

    for (auto const& spelling: { std::string_view { "--user" }, std::string_view { "--username" } })
    {
        INFO("spelling: " << spelling);
        CHECK(std::ranges::none_of(NodeOptions(), [spelling](auto const& option) {
            return option.primary == spelling || option.alias == spelling;
        }));
    }
}

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
        // Installs the file a registration would be READ from, then exits (#397). A
        // registration carrying it would re-seed at every start, which is the same
        // objection `--install-service` carries and the reason both are one-shot.
        "--seed-config",
        "--service-scope", // install-time only; not a thing the worker runs with
        "--service-name",  // emitted unconditionally, above the table
        "--daemon",        // carried as ServiceSpec::daemonFlag, not an argument
        "--help",          //
        "--version",       //
        // The one field with no safe representation in launch arguments: a
        // supervisor records them where every local account can read them, so
        // emitting the secret would publish it to exactly the accounts it exists
        // to keep out. InlineCredentialRejection reports the omission instead.
        "--requirepass",
        // BOTH spellings of the timestamp switch, and the pair is excluded rather
        // than one of it. A registration carries whichever spelling PRODUCES the
        // configured value and never both, so this sweep -- one configuration, every
        // flag must appear in it -- cannot express the pair at all: demanding both is
        // a contradiction, and demanding one is a guess about which platform the
        // sweep is running on. The default is platform-dependent since #496, so that
        // guess is wrong half the time and silently: excluding only the negative
        // spelling passed here and dropped `--log-timestamps` from the sweep on
        // macOS.
        //
        // The coverage moved rather than vanished. "the timestamp switch registers
        // whichever spelling produces the value" drives all four combinations --
        // both values against both platform defaults -- which is more than this
        // sweep could assert for it even on one platform.
        "--log-timestamps",
        "--no-log-timestamps",
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
        // The enrollment verbs, and the same rule the cluster ones above carry -- with
        // the sharpest consequence in the set. `--enroll-open` is the one flag here
        // whose replay is a SECURITY event rather than a wasted one: a registration
        // carrying it would re-open the window at every boot, forever, so a machine
        // rebooting would silently re-enter the one interval in which a stranger can
        // ask this cluster for its key, with the counter that reports it
        // (`fastcache_enrollment_windows_opened_total`) rising on a schedule nobody
        // reads as an event. The window is a MOMENT an operator chooses; a service
        // registration is a decision replayed forever, and those cannot be the same
        // flag.
        //
        // `--enroll-from` is excluded for the ordinary reason rather than that one: it
        // is a one-shot join that ends the process, so a service that carried it would
        // try to join a cluster it is already a member of instead of serving.
        "--enroll-from",
        "--enroll-open",
        "--enroll-close",
        "--enroll-list",
        "--enroll-approve",
        "--enroll-reject",
    });

    // A configuration in which no field holds its default, so every emitter fires.
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6674";
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.toolchains = { "/usr/bin/g++", "abc123=/usr/bin/clang++" };
    cfg.extraAllowedArgs = { "-fanalyzer", "/Qvec-report:2" };
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
    cfg.serveScheduler = true;
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
    cfg.nodeListen = "127.0.0.1:6679";
    // Typed, not merely different -- the same rule `--cache-memory` above follows,
    // and for this flag the provenance is what decides whether a bind failure stops
    // the node, so a registration that lost it warns past a taken port forever.
    cfg.nodeListenExplicit = true;
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
    // A PATH, exactly like `--cluster-key-file` above, which is why it belongs in a
    // registration at all: the case below asserts the SECRET is never written, and
    // the two rules only stay compatible because what travels is where to read the
    // credential rather than the credential.
    cfg.schedulerTokenFile = "scheduler.token";
    cfg.logLevel = LogLevel::Debug;
    // Worker state, so it has to survive a registration: a service installed with
    // timestamps on must come back with them on, or the operator who turned them on
    // to diagnose something finds them gone at the next boot -- which is exactly when
    // they are being relied upon. Away from the default WHATEVER the default is, so
    // the fixture's own rule -- no field holds its default -- keeps holding on a
    // platform where that default is true (#496).
    cfg.logTimestamps = !FastCache::DefaultLogTimestamps;
    cfg.pidfile = "worker.pid";
    cfg.drainTimeoutSeconds = 90;
    // Worker state like any other, and the one that supplies most of the rest: a
    // registration that dropped it would come back at every boot knowing only what
    // was typed alongside --install-service, which for a package install is close
    // to nothing.
    cfg.configPath = "node.yaml";

    // Every provenance bit, set by WALKING the table rather than by fourteen
    // assignments here (#713). Emission follows whether the operator TYPED a flag,
    // not whether its value differs from a default, so a fixture that only assigns
    // values models an operator who typed nothing -- and this sweep would then be
    // asserting that a registration carrying almost nothing is complete.
    //
    // Derived and not restated, for the same reason the excluded-flag list above is:
    // a hand-written set of bits is maintained by the same person who forgot to add
    // one, and it would go stale silently in the direction that PASSES.
    for (auto const& option: NodeOptions())
        if (option.explicitBit != nullptr)
            cfg.*option.explicitBit = true;

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
    CHECK(ServiceRegistrationRejection(spec, SupervisorKind::Launchd).has_value());
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

TEST_CASE("NodeConfig: a node with no cluster key file serves no enrollment window", "[node][config]")
{
    // **The keyless-node rule, and it is here because this is where it can be tested.**
    // It used to live only as a conjunction inside `WorkerBody`, in the one translation
    // unit no test links, so nothing could assert it at all.
    //
    // A keyless consensus node is LEGAL -- the startup table refuses one only where the
    // compile port faces the network and admits remote peers -- and on such a node the
    // enrollment window was openable, listable and APPROVABLE while it could never
    // admit anybody, because the hand-over reads a key file that is not configured.
    // Worse than a refusal: an approval commits `ClusterAdmit` before the key is
    // consulted, so it grew the replicated configuration, and therefore the QUORUM, by
    // a machine that then received `StorageWriteFailed` and never became anything.
    auto const configured = ParseNodeArgv({ "--scheduler=s:1", "--listen-raft=7100", "--cluster-key-file=/etc/fc/key" });
    REQUIRE(configured.has_value());
    CHECK(EnrollmentConfigured(*configured));

    // The clause with the history. Consensus is on, so `RunsConsensus` says yes and the
    // OLD predicate said yes with it — which is the defect, not a near miss.
    auto const keyless = ParseNodeArgv({ "--scheduler=s:1", "--listen-raft=7100" });
    REQUIRE(keyless.has_value());
    REQUIRE(RunsConsensus(*keyless));
    CHECK_FALSE(EnrollmentConfigured(*keyless));

    // And the other clause still bites, or "require a key file" would have been
    // implemented as "require only a key file" and every keyed worker in the fleet
    // would offer to admit machines to a cluster it does not belong to.
    auto const clusterless = ParseNodeArgv({ "--scheduler=s:1", "--cluster-key-file=/etc/fc/key" });
    REQUIRE(clusterless.has_value());
    REQUIRE_FALSE(RunsConsensus(*clusterless));
    CHECK_FALSE(EnrollmentConfigured(*clusterless));

    auto const neither = ParseNodeArgv({ "--scheduler=s:1" });
    REQUIRE(neither.has_value());
    CHECK_FALSE(EnrollmentConfigured(*neither));
}

TEST_CASE("NodeConfig: a later enrollment verb drops the earlier one's subject", "[node][config]")
{
    // **Last-flag-wins has to move BOTH halves.** These are one-shot verbs that
    // overwrite each other, so `--enroll-approve=n4 --enroll-open` means `Open`. The
    // applier set the action and left the subject behind, so the client sent
    // `EnrollControl(Open, "n4")` -- which the decoder refuses on its arity rule,
    // `EnrollControlNamesSubject(Open)` being false while the subject is not empty --
    // and the operator was answered *a control frame this build cannot read*: a
    // VERSION-MISMATCH sentence for a flag-combination mistake, which sends somebody
    // comparing builds across a fleet that is fine.
    auto const overridden = ParseNodeArgv({ "--scheduler=s:1", "--enroll-approve=n4", "--enroll-open" });
    REQUIRE(overridden.has_value());
    CHECK(overridden->enroll.action == EnrollAction::Open);

    // The assertion that DISTINGUISHES: the action alone was already correct under the
    // defect, because it was the half that got assigned. It is the residue that made
    // the frame unreadable.
    CHECK(overridden->enroll.subject.empty());

    // The other order still carries its operand, or "clear it" would have been
    // implemented as "never set it" and every approval would name nobody.
    auto const named = ParseNodeArgv({ "--scheduler=s:1", "--enroll-open", "--enroll-approve=n4" });
    REQUIRE(named.has_value());
    CHECK(named->enroll.action == EnrollAction::Approve);
    CHECK(named->enroll.subject == "n4");

    // And a subject-taking verb replacing another keeps its OWN subject rather than the
    // first one's, which is the case a naive clear-on-entry would also pass.
    auto const second = ParseNodeArgv({ "--scheduler=s:1", "--enroll-approve=n4", "--enroll-reject=n9" });
    REQUIRE(second.has_value());
    CHECK(second->enroll.action == EnrollAction::Reject);
    CHECK(second->enroll.subject == "n9");
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

    // The one worth the most: left empty, --advertise defaults to whatever
    // --listen-node resolves to, which is LOOPBACK on a node that does not schedule.
    // Such a worker registers, heartbeats, is leased, and is never reached.
    //
    // The REASON changed with #290 stage 3 and the expectation is updated to match
    // rather than loosened: the old default was `{--bind}:{--port}` and the string to
    // look for was `0.0.0.0`. A refusal that keeps its name while its cause moves is
    // how a rule stops meaning what it says, so this asserts the new cause by name.
    auto noAdvertise = Installable();
    noAdvertise.advertise.clear();
    auto const rejection = NodeServiceRejection(noAdvertise);
    REQUIRE(rejection.has_value());
    CHECK(Unwrap(rejection).contains("--advertise"));
    CHECK(Unwrap(rejection).contains("--listen-node"));
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
    // A parse rather than a Config since #349: the daemon registers what the operator
    // NAMED, so its spec is built from the command line, not from the merged
    // configuration. This case only reads the account, and a parse that named nothing
    // is what asks for the default one.
    auto const daemonSpec = MakeDaemonServiceSpec(std::filesystem::path { "fastcached" }, FastCache::CliResult {});
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
    // fails forever. The worker takes a config file and NO storage directory --
    // `NodeOptions()` has `--config` and no `--storage` at all -- and the
    // installer used to fill both in from the daemon's defaults, because the
    // application name was hardcoded.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, Installable());

    // Since [#396](https://github.com/LASTRADA-Software/fastcached/issues/396) the
    // worker NAMES an application and accepts one default rather than none. Those
    // used to be a single answer, so the only safe thing it could say was nothing
    // -- at the cost of the system-scope `--config=` default and the install-time
    // `ServiceAccountReadDenial` on it. Both halves are asserted, because naming
    // the application again while accepting everything is precisely the
    // registration that dies at every boot.
    CHECK(spec.applicationName == NodeApplicationName);
    CHECK(AcceptsScopeDefault(spec.acceptedScopeDefaults, ScopeDefault::ConfigPath));
    CHECK_FALSE(AcceptsScopeDefault(spec.acceptedScopeDefaults, ScopeDefault::StoragePath));

    // The assertion that matters is not what the fields say but what they make the
    // installer do, so drive the installer's own function -- on every platform,
    // which is the half that was missing while it lived inside the macOS block.
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

TEST_CASE("NodeConfig: a consensus node that names no --raft-peer of its own is refused before it is installed",
          "[node][consensus][policy]")
{
    // #168. `ConsensusTier::Start` refuses this, and the install path returns long
    // before any tier is built -- so the registration was written, reported
    // installed, and then exited `ExitUsage` at every boot with nobody watching.
    // It is a pure function of the command line, like every startup rule, so it is
    // one now.
    //
    // Every shape carries `--listen-raft`, because that is what turns consensus on
    // since #1022. Without it the node runs no consensus at all and is answered by a
    // different rule -- which is the case below, asserted by the text only THAT rule
    // produces, since "refused, and names --raft-peer" would pass under both.
    auto const shapes = std::to_array<std::pair<char const*, NodeConfig>>({
        { "no --raft-peer at all",
          [] {
              auto cfg = Installable();
              cfg.nodeId = "n1";
              cfg.raftListen = "6680";
              cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
              return cfg;
          }() },
        { "peers that name somebody else",
          [] {
              auto cfg = Installable();
              cfg.nodeId = "n1";
              cfg.raftListen = "6680";
              cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
              cfg.raftPeers = { Peer("n2=10.0.0.2:6680"), Peer("n3=10.0.0.3:6680") };
              return cfg;
          }() },
        { "a joiner that named the cluster and not itself",
          [] {
              auto cfg = Installable();
              cfg.nodeId = "n4";
              cfg.raftListen = "6680";
              cfg.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };
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
    CHECK(Unwrap(StartupPolicyRejection(shapes[0].second)) == ConsensusNamesNoSelfPeerRefusal);

    // And it names its own flag, which is what lets `main.cpp` print a tier's
    // refusal without prefixing one -- it used to, and rendered this message as
    // "--node-id --node-id names no --raft-peer".
    //
    // The flag it names moved with the switch at #1022. Pinned as the leading text
    // rather than as "mentions --listen-raft somewhere", because a message naming the
    // wrong flag first is what sends an operator to add a flag they already have.
    CHECK(ConsensusNamesNoSelfPeerRefusal.starts_with("--listen-raft"));

    // The same three shapes with the switch OFF are refused by a DIFFERENT rule, and
    // that is the half a "refused, and names --raft-peer" assertion cannot see. Under
    // the old switch these were this rule's own inputs, so a test that only counted
    // refusals would pass whichever flag the predicate reads.
    for (auto const& [what, clustered]: shapes)
    {
        INFO(what << ", without --listen-raft");
        auto cfg = clustered;
        cfg.raftListen.clear();
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--listen-raft is what turns consensus ON"));
        CHECK_FALSE(Unwrap(refusal) == ConsensusNamesNoSelfPeerRefusal);
    }

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

TEST_CASE("NodeConfig: a cluster configured without --listen-raft is refused before it is installed",
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
    // the operator typed; an ABSENT one still reaches a cross-flag rule below. Both
    // still fire, for their own inputs, which is what made the move safe.
    //
    // What #1022 changed is which cross-flag rule the ABSENT case reaches. The flag
    // used to be required BY `--node-id`; now it is the switch, so its absence means
    // the id and the peer list configure nothing -- the same silent no-op read from
    // the other side, and a different sentence.
    struct RaftCase
    {
        char const* listen;  ///< What the operator wrote.
        char const* expects; ///< Text only the answering rule produces.
    };

    for (auto const& [listen, expects]: std::to_array<RaftCase>({
             // Nothing to judge, so the grammar loop skips it and the cross-flag rule
             // answers: consensus is off, so everything else here is inert.
             { .listen = "", .expects = "is what turns consensus ON" },
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

    // `--raft-peer` alone reaches its own row rather than `--node-id`'s, and the two
    // are separate rows because they are separate mistakes: one operator has told
    // this node who it is, the other who else there is.
    {
        auto cfg = Installable();
        cfg.raftPeers = { Peer("n1=10.0.0.1:6680") };
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).starts_with("--raft-peer"));
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

TEST_CASE("NodeConfig: the consensus switch is --listen-raft, not --node-id", "[node][consensus][policy]")
{
    // #1022. `RunsConsensus` read `--node-id`, so the node identity could never be
    // given a default: any default makes `nodeId.empty()` false forever, and
    // `ClusterSelfMember` then names no member on the one-machine deployment -- so
    // that node would be refused at every boot, and at `--install-service`.
    //
    // BOTH DIRECTIONS, because this is one expression and a test that only drives
    // the happy path passes whichever flag it reads. Each case below is one the
    // OTHER reading answers oppositely; a suite asserting only "consensus runs when
    // the cluster is fully configured" would agree with the reverted predicate on
    // every case it contains.
    //
    // The two TIERS that have to agree about this are asserted where each is tested
    // -- `ConsensusTier_test`'s "a tier is built exactly when RunsConsensus says so"
    // and `SchedulerTier_test`'s pair of cases -- because #613 was those two tiers
    // authoring one rule, and a moved rule is when that recurs.

    SECTION("--node-id with no --listen-raft runs NO consensus")
    {
        // The case that fails if the predicate is left reading the id.
        auto cfg = Installable();
        cfg.nodeId = "n1";
        cfg.raftPeers = { Peer("n1=10.0.0.1:6680") };
        CHECK_FALSE(RunsConsensus(cfg));
    }

    SECTION("--listen-raft with no --node-id RUNS consensus")
    {
        // The mirror, and the one the identity work needs: an id that is derived
        // rather than typed cannot be what turns the mode on.
        auto cfg = Installable();
        cfg.raftListen = "6680";
        CHECK(RunsConsensus(cfg));
    }

    SECTION("neither flag: the single-machine install still starts")
    {
        // The regression this ticket exists to make impossible. Asserted as a STARTUP
        // verdict rather than only as the predicate, because the predicate being false
        // is not the same fact as the node being allowed to run.
        auto const cfg = Installable();
        CHECK_FALSE(RunsConsensus(cfg));
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
        CHECK_FALSE(NodeInstallRejection(cfg).has_value());
    }

    SECTION("the surface row and the predicate are one answer")
    {
        // `--print-surfaces` resolves the raft row and `RunsConsensus` asks that same
        // row, so a port that is served and a mode that is on cannot disagree. Read
        // the other way round, this is what stops a second author of "is the raft
        // surface served" appearing in `NodeConfig.cpp`.
        //
        // Both values, because an identity between two expressions that are both
        // false is satisfied by a predicate that is always false.
        auto cfg = Installable();
        cfg.raftListen = "6680";
        CHECK(RunsConsensus(cfg));
        CHECK(RunsConsensus(cfg) == !RowFor(NodeSurface::Raft).Resolve(cfg).empty());
        cfg.raftListen.clear();
        CHECK_FALSE(RunsConsensus(cfg));
        CHECK(RunsConsensus(cfg) == !RowFor(NodeSurface::Raft).Resolve(cfg).empty());
    }

    SECTION("the ready line follows the switch too")
    {
        // `AdmissionSummary` spelled `nodeId.empty()` for itself, which is a third
        // author of the same rule: after the switch moved it would have told an
        // operator running a clustered node that it admits this machine only, while
        // consensus was about to admit hosts nobody typed.
        auto clustered = Installable();
        clustered.raftListen = "6680";
        CHECK(AdmissionSummary(clustered).contains("the cluster's members"));

        auto lone = Installable();
        lone.nodeId = "n1";
        CHECK_FALSE(AdmissionSummary(lone).contains("the cluster's members"));
    }
}

TEST_CASE("NodeConfig: the packaged socket-activated worker starts", "[node][policy]")
{
    // **The configuration that took the packaging jobs red on master**
    // ([#770](https://github.com/LASTRADA-Software/fastcached/issues/770)), written out
    // exactly as `.github/workflows/build.yml` writes it into the file the unit's
    // ExecStart names:
    //
    //     scheduler: 127.0.0.1:6675
    //     advertise: 127.0.0.1:6676
    //     toolchain:
    //       - /usr/bin/g++
    //
    // No `listen_node:` -- under socket activation the UNIT owns the address, so the
    // flag "still holds a value that describes nothing" and its default (6674) is not
    // the port this worker serves. #594's rule compared the advertised port against
    // that default, refused, and the service exited 2: the socket accepted the
    // activating connection, nothing came up, and the job failed on "a connection did
    // not bring the worker up".
    //
    // Invisible to every other suite because none of them is socket-activated, which
    // is why this case is written from the workflow's own bytes rather than from a
    // shape that seemed representative.
    NodeConfig packaged;
    packaged.scheduler = "127.0.0.1:6675";
    packaged.advertise = "127.0.0.1:6676";
    packaged.toolchains = { "/usr/bin/g++" };
    // `nodeListen` keeps its default AND `nodeListenExplicit` stays false -- which is
    // the whole point. Asserted rather than assumed, so a later change to the default
    // cannot quietly turn this into a case about some other configuration.
    REQUIRE_FALSE(packaged.nodeListenExplicit);
    REQUIRE(packaged.nodeListen == "127.0.0.1:6674");
    REQUIRE(packaged.advertise != packaged.nodeListen);

    CHECK_FALSE(StartupPolicyRejection(packaged).has_value());

    // And an install of the same shape is accepted too: `NodeInstallRejection`
    // composes this table, and a socket-activated worker is installed exactly like
    // this. Without it the rule would refuse at registration instead of at start --
    // the same fault moved one step earlier, where an operator is watching.
    packaged.installService = true;
    CHECK_FALSE(NodeInstallRejection(packaged).has_value());
}

TEST_CASE("NodeConfig: --advertise naming this machine at a port it does not serve is refused", "[node][policy]")
{
    // **The startup reachability rules judged the advertised HOST and never its port**,
    // so a node could tell every client to dial a port nothing on it listens on and
    // start cleanly ([#594](https://github.com/LASTRADA-Software/fastcached/issues/594)).
    // In the documented layout that sends every dispatched compile to 6674 -- the shared
    // `fastcached` cache daemon -- while the registration succeeds and every counter
    // reads normally.
    //
    // Reachable on a node with NO membership flags, which is the single-machine
    // deployment: the three advertise rows are scoped to `NamesAMembershipPolicy`, so
    // they answer a loopback advertise before this can. Measured with the binary, and
    // the fixtures below are the same shapes.
    auto const worker = [] {
        NodeConfig cfg;
        cfg.scheduler = std::string { SchedulerEndpoint };
        cfg.toolchains = { "/usr/bin/g++" };
        cfg.nodeListen = "6675";
        // **Set together, because a parse cannot produce one without the other.** The
        // rule only judges a port the operator NAMED (#770), and a fixture assigning
        // the value while leaving the provenance bit false models a configuration no
        // command line can produce -- the same shape as the nine fixtures #386
        // exposed, one flag along.
        cfg.nodeListenExplicit = true;
        return cfg;
    };

    SECTION("the port belongs to no surface: named, with the port this node does serve")
    {
        auto cfg = worker();
        cfg.advertise = "127.0.0.1:6674";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--advertise"));

        // **The port this node ACTUALLY serves, not a restatement of the mistake.** An
        // operator who typed the wrong one of their own ports needs the right one; the
        // ticket asks for this by name.
        CHECK(Unwrap(refusal).contains("6675"));
        CHECK(Unwrap(refusal).contains("6674"));
    }

    SECTION("the port is one of this node's OWN other surfaces, and is named as such")
    {
        // Sharper and unambiguous: the operator has named the wrong one of their own
        // ports, and the message says which. Walked off `NodeSurfaceTable()`, so a
        // surface added later is recognised without a second author.
        auto cfg = worker();
        cfg.adminListen = "6677";
        cfg.advertise = "127.0.0.1:6677";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--admin-listen"));
        CHECK(Unwrap(refusal).contains("6675"));
    }

    SECTION("a bare IPv6 loopback literal names no port, and is not refused for one")
    {
        // **Two parsers disagreed about `::1` and the refusal named a port nobody
        // typed** (#822). The gate asks `HostOfEndpoint`, which declines to split an
        // unbracketed IPv6 literal; `SplitHostPort` splits on the last colon anyway
        // and yielded host `:` with port `1`. So this configuration -- a node
        // advertising itself at a bare loopback literal, naming no port at all -- was
        // refused for advertising port 1.
        auto cfg = worker();
        cfg.advertise = "::1";

        auto const refusal = StartupPolicyRejection(cfg);
        CHECK_FALSE(refusal.has_value());
    }

    SECTION("a bracketed IPv6 loopback at the wrong port is still refused, and names it")
    {
        // **The control that stops the fix above being a rule against judging IPv6 at
        // all.** Bracketed, so there IS a port and both parsers agree on it -- and the
        // refusal must still fire and must name 6675, the port this node serves.
        // Without this, "declines to split a bare literal" and "never judges an IPv6
        // advertise" are one passing test.
        auto cfg = worker();
        cfg.advertise = "[::1]:6674";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--advertise"));
        CHECK(Unwrap(refusal).contains("6675"));
        // And it must not have invented the port-1 reading from the old split.
        CHECK_FALSE(Unwrap(refusal).contains("port 1,"));
    }

    SECTION("a remote host with any port is accepted, because NAT is legitimate")
    {
        // **The control that stops this being a rule against port forwarding.**
        // `--advertise` exists precisely so a node can name an address only clients can
        // reach -- NAT, a proxy, a container publishing a different external port -- and
        // `AdvertisesPastALoopbackBind`'s earlier draft was wrong for exactly this
        // reason. Without this section, "the advertised port must equal --listen-node"
        // would pass the section above.
        auto cfg = worker();
        cfg.advertise = "nat.example.com:9999";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a loopback advertise that AGREES with the bind is accepted")
    {
        // The single-machine fleet, and it is correct: the two halves agree.
        auto cfg = worker();
        cfg.advertise = "127.0.0.1:6675";
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("no --advertise at all is accepted")
    {
        // The default case, and the one a fix that judged `cfg.advertise` directly would
        // break: `AdvertisedEndpoint` falls back to the `Node` surface, so the endpoint
        // this node would advertise agrees with its bind by construction. Judged through
        // that function rather than the raw field, or this rule would refuse every node
        // that names nothing.
        CHECK_FALSE(StartupPolicyRejection(worker()).has_value());
    }

    SECTION("the host rows still answer first when both apply")
    {
        // Ordering, asserted rather than left to the reader. A membership node that
        // advertises loopback is unreachable by any peer, which is a bigger problem than
        // reaching it at the wrong port -- so that sentence is the one an operator gets.
        auto cfg = worker();
        cfg.nodeListen = "0.0.0.0:6675";
        cfg.advertise = "127.0.0.1:6674";
        cfg.fleetMembers = { "10.0.0.2" };
        cfg.clusterKeyFile = "cluster.key";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        // The host row's words, not this rule's: it is the one that says every peer
        // would be told to dial ITSELF. Matched on that phrase rather than on the
        // absence of this rule's, because "the other rule answered" is the claim.
        CHECK(Unwrap(refusal).contains("dial ITSELF"));
    }
}

TEST_CASE("NodeConfig: an empty --scheduler is refused at STARTUP and not only at install", "[node][policy]")
{
    // **The rule this ticket is about, asserted where it has to hold.** An empty
    // `--scheduler` was refused by an inline `if` in `main()` and by row 1 of
    // `NodeServiceRejection`. `main.cpp` is in no test target, so the copy that ran at
    // startup was the one nothing could assert, and the two had already drifted on the
    // sentence they say -- which is the observable half of a duplicate
    // ([#386](https://github.com/LASTRADA-Software/fastcached/issues/386)).
    auto headless = Installable();
    headless.scheduler.clear();

    // **The control FIRST, so one run shows the discrimination.** `NodeInstallRejection`
    // composes `NodeServiceRejection`, which has always had this row -- so it refuses an
    // empty `--scheduler` whether or not the startup table does. Asserting through it
    // would pass under the exact bug this case exists to catch, which is why the ticket
    // names it as the wrong target. Ordered ahead of the subject because a `REQUIRE`
    // below aborts the case: read after the subject, this control would simply not run
    // on the failing build and could not show what it is for.
    CHECK(NodeInstallRejection(headless).has_value());

    auto const refusal = StartupPolicyRejection(headless);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).contains("--scheduler"));

    // **The message must be the STARTUP sentence, not the install-time one.** Both
    // rules are about the same empty field and the install row says "to install a
    // service" -- true when a registration is being written and false at every start,
    // where the process is simply refusing to run. An operator told the wrong one goes
    // looking for a service they never asked to install.
    CHECK_FALSE(Unwrap(refusal).contains("to install a service"));

    // And the ordinary configuration is untouched: a node that names its scheduler
    // starts, so this is a refusal of the empty value and not of the flag.
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
        { "--listen-node",
          [] {
              auto cfg = Installable();
              cfg.nodeListen = "nope";
              cfg.fleetOpen = true; // else the fleet-membership rule answers first
              return cfg;
          }() },
        { "--admin-listen",
          [] {
              auto cfg = Installable();
              cfg.adminListen = "nope";
              return cfg;
          }() },
        { "--listen-node",
          [] {
              auto cfg = Installable();
              cfg.nodeListen = "nope";
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
    // for three of these, and `--listen-node` carries a non-empty default that
    // every ordinary node runs with -- so a rule reading "must parse" rather than
    // "parses when given" would refuse the default deployment outright.
    CHECK_FALSE(StartupPolicyRejection(Installable()).has_value());
    CHECK_FALSE(NodeInstallRejection(Installable()).has_value());

    auto off = Installable();
    off.serveScheduler = false;
    off.adminListen.clear();
    off.nodeListen.clear(); // documented as "no cache tier", not as a mistake
    off.discoveryAddress.clear();
    CHECK_FALSE(StartupPolicyRejection(off).has_value());

    // Every surface configured, in each of the spellings the tiers accept: a bare
    // port, an address and port, and a bracketed v6 literal.
    auto configured = Installable();
    configured.serveScheduler = true;
    configured.fleetOpen = true;
    // `--fleet-open` admits every machine there is, so this node has to be able to
    // check the grants they present (#282).
    configured.clusterKeyFile = "cluster.key";
    configured.adminListen = "127.0.0.1:6677";
    // The v6 WILDCARD, not `[::1]`. This node admits every machine there is, so a
    // loopback bind behind its routable --advertise is refused since #290 stage 3 --
    // it would tell peers to dial an address it never accepts on.
    configured.nodeListen = "[::]:6679";
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
    cache.nodeListen = ":6674";
    auto const widened = StartupPolicyRejection(cache);
    REQUIRE(widened.has_value());
    CHECK(Unwrap(widened).contains("--listen-node"));

    auto admin = Installable();
    admin.adminListen = ":6677";
    CHECK(StartupPolicyRejection(admin).has_value());

    // A BARE port is a different thing and stays legal: it names no host at all, so
    // it takes the surface's own default rather than silently replacing it.
    auto bare = Installable();
    bare.nodeListen = "6674";
    bare.adminListen = "6677";
    bare.serveScheduler = true;
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
    //
    // The first re-pointed at #1022's switch and the second did not, so this is also
    // where their ORDER is pinned: a joiner missing both would otherwise be told about
    // whichever row happens to be first, and the switch is the one to fix first
    // because the peer list configures nothing without it.
    NodeConfig noPort;
    noPort.raftJoin = true;
    noPort.raftPeers = { Peer("n1=10.0.0.1:6680") };
    CHECK(Unwrap(StartupPolicyRejection(noPort)).contains("--raft-join waits to be admitted"));

    NodeConfig noPeers;
    noPeers.raftJoin = true;
    noPeers.raftListen = "6680";
    CHECK(Unwrap(StartupPolicyRejection(noPeers)).contains("--raft-join needs --raft-peer"));

    // Missing both: the switch is answered, not the peer list.
    NodeConfig neither;
    neither.raftJoin = true;
    CHECK(Unwrap(StartupPolicyRejection(neither)).contains("--raft-join waits to be admitted"));
}

TEST_CASE("NodeConfig: consensus flags with no --listen-raft are refused rather than ignored", "[node][consensus][policy]")
{
    // `StartConsensusOrExplain` returns a null tier when consensus is off, so these
    // flags are read by nobody: nothing binds, nothing dials, and nothing anywhere
    // says the operator's cluster was not configured. That is the silent no-op this
    // table already refuses for `--cluster-key-file` without `--discovery` and
    // `--dashboard-token-file` without `--dashboard`.
    //
    // The flag whose ABSENCE puts a node in that state moved at #1022. `--listen-raft`
    // used to be one of the inert flags and is now the switch; `--node-id` used to be
    // the switch and is now inert without it. So the two rows swapped sides, and each
    // is asserted by the text only its own row produces.
    auto named = Installable();
    named.nodeId = "n1";
    CHECK(Unwrap(StartupPolicyRejection(named)).starts_with("--node-id names this node inside a cluster"));
    CHECK(NodeInstallRejection(named).has_value());

    auto peered = Installable();
    peered.raftPeers = { Peer("n1=10.0.0.1:6680") };
    CHECK(Unwrap(StartupPolicyRejection(peered)).starts_with("--raft-peer names the cluster"));

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
        std::string raftListen;           ///< `--listen-raft`, i.e. whether consensus runs (#1022).
        char const* says;                 ///< What the line must contain.
        char const* saysNot { nullptr };  ///< What it must NOT contain, if anything.
    };

    auto const rows = std::to_array<Row>({
        { .what = "no policy at all", .members = {}, .open = false, .raftListen = {}, .says = "this machine only" },
        // Naming the remedy is the point of that row: an operator who reads "this
        // machine only" and is not told the two flags has been informed of a symptom.
        { .what = "no policy names the flags that give one",
          .members = {},
          .open = false,
          .raftListen = {},
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
          .raftListen = "6680",
          .says = "the cluster's members",
          // And never the unclustered phrase: "this machine only" is a final answer,
          // and on a node whose cluster is about to agree a member set it is wrong.
          .saysNot = "this machine only" },
        { .what = "no policy, with consensus running, still names --fleet-member",
          .members = {},
          .open = false,
          .raftListen = "6680",
          .says = "--fleet-member" },
        { .what = "no policy, with consensus running, still names --fleet-open",
          .members = {},
          .open = false,
          .raftListen = "6680",
          .says = "--fleet-open" },
        // And a node that HAS a list says the cluster adds to it, so an operator
        // reading a bare count does not conclude that is the whole policy.
        { .what = "a member list, with consensus running",
          .members = { "10.0.0.1:6676" },
          .open = false,
          .raftListen = "6680",
          .says = "the cluster's members" },
        { .what = "a member list",
          .members = { "10.0.0.1:6676", "10.0.0.2:6676" },
          .open = false,
          .raftListen = {},
          .says = "2 member" },
        // "This machine" out loud even when a list exists: that admission is
        // unconditional, and an operator reading a bare count would not know their
        // own builds were covered.
        { .what = "a member list still says this machine",
          .members = { "10.0.0.1:6676" },
          .open = false,
          .raftListen = {},
          .says = "this machine" },
        { .what = "--fleet-open", .members = {}, .open = true, .raftListen = {}, .says = "every caller" },
    });

    for (auto const& row: rows)
    {
        INFO(row.what);
        NodeConfig cfg;
        cfg.fleetMembers = row.members;
        cfg.fleetOpen = row.open;
        cfg.raftListen = row.raftListen;

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
        cfg.serveScheduler = true;

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
        cfg.serveScheduler = true;
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
        cfg.nodeListen = "127.0.0.1:6679";
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
        // Each row names the REASON it expects, not merely that it was refused. Two
        // rows answer here now -- the wildcard and, since #290 stage 3, loopback -- and
        // both messages contain "--advertise", so asserting only that would let either
        // stand in for the other.
        struct Row
        {
            char const* what;      ///< The shape, for the failure message.
            char const* advertise; ///< `--advertise`, or empty for the fallback.
            char const* reason;    ///< A phrase only this refusal's message carries.
        };

        auto const refused = std::to_array<Row>({
            // "no --advertise at all" is deliberately NOT here. It used to be, because
            // the fallback was `{--bind}:{--port}` and `--bind` defaulted to the
            // wildcard. The fallback is now the `Node` surface, which defaults to
            // loopback -- and a node that BINDS loopback and ADVERTISES loopback is a
            // coherent single-machine fleet, not a mistake. It is asserted below.
            // Spelled out rather than defaulted: the endpoint is judged, never the
            // question of which flag produced it.
            { .what = "the wildcard spelled out", .advertise = "0.0.0.0:6676", .reason = "the wildcard resolves to" },
            { .what = "the v6 wildcard", .advertise = "[::]:6676", .reason = "the wildcard resolves to" },
            // An empty host is the wildcard too -- it reaches `getaddrinfo` as
            // nullptr -- which is the third case `--listen-node=:6674` is refused
            // for and the one that reads like an address.
            { .what = "a bare colon", .advertise = ":6676", .reason = "the wildcard resolves to" },
        });

        for (auto const& row: refused)
        {
            INFO(row.what);
            NodeConfig cfg;
            cfg.scheduler = "scheduler.internal:6675";
            cfg.advertise = row.advertise;
            cfg.fleetOpen = true;
            cfg.clusterKeyFile = "cluster.key";

            auto const refusal = StartupPolicyRejection(cfg);
            REQUIRE(refusal.has_value());
            CHECK(Unwrap(refusal).contains("--advertise"));
            CHECK(Unwrap(refusal).contains(row.reason));
        }

        // The three refusals stay TOLD APART, asserted together rather than inferred
        // from the rows above: the way this regresses is one predicate widening to
        // cover several, which passes every row individually and leaves the operator
        // who typed nothing reading about a wildcard they never wrote. Three
        // conditions sharing one phrase is the same defect the `reason` column above
        // catches, one row further along.
        NodeConfig wildcard;
        wildcard.scheduler = "scheduler.internal:6675";
        wildcard.fleetOpen = true;
        wildcard.advertise = "0.0.0.0:6676";

        // Bind and advertise DISAGREE in each direction. That disagreement is the
        // rule; neither address is wrong on its own.
        NodeConfig loopback = wildcard;
        loopback.nodeListen = "0.0.0.0:6674";  // accepts from the network...
        loopback.advertise = "127.0.0.1:6674"; // ...and tells peers to dial themselves

        NodeConfig pastTheBind = wildcard;
        pastTheBind.nodeListen = "127.0.0.1:6674";         // accepts only locally...
        pastTheBind.advertise = "worker-01.internal:6674"; // ...and says otherwise

        auto const wildcardRefusal = StartupPolicyRejection(wildcard);
        auto const loopbackRefusal = StartupPolicyRejection(loopback);
        auto const pastRefusal = StartupPolicyRejection(pastTheBind);
        REQUIRE(wildcardRefusal.has_value());
        REQUIRE(loopbackRefusal.has_value());
        REQUIRE(pastRefusal.has_value());
        CHECK(Unwrap(wildcardRefusal) != Unwrap(loopbackRefusal));
        CHECK(Unwrap(loopbackRefusal) != Unwrap(pastRefusal));
        CHECK(Unwrap(wildcardRefusal) != Unwrap(pastRefusal));

        // And a worker that named BOTH is not refused, which is what makes the three
        // rows a rule an operator can satisfy rather than a wall. This is the exact
        // configuration the loopback row's message now tells them to write.
        NodeConfig reachable = wildcard;
        reachable.advertise = "worker-01.internal:6674";
        reachable.nodeListen = "0.0.0.0:6674";
        // And a key, because widening the bind is what makes the compile verbs face
        // the network -- so the unrelated row that wants one for remote peers fires
        // too. Naming it here keeps this assertion about the three reachability rows
        // rather than about whichever rule happens to answer first.
        reachable.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(reachable).has_value());

        // **The single-machine fleet: loopback bind, loopback advertise, peers
        // admitted.** Nobody else is meant to reach this node, so "no remote client can
        // reach the advertised endpoint" is the configuration working rather than
        // failing. An earlier draft of the loopback row refused exactly this and would
        // have broken `dist-compile-e2e.sh`, which starts a `--fleet-open` scheduler on
        // `127.0.0.1` advertising `127.0.0.1`. The fixture comment that caught it was
        // right and is why the rule is about DISAGREEMENT rather than reachability.
        NodeConfig singleMachine;
        singleMachine.scheduler = "127.0.0.1:6675";
        singleMachine.nodeListen = "127.0.0.1:6674";
        singleMachine.advertise = "127.0.0.1:6674";
        singleMachine.fleetOpen = true;
        CHECK_FALSE(StartupPolicyRejection(singleMachine).has_value());

        // The same node with no --advertise at all, which is how an operator actually
        // reaches that shape: the fallback is the Node surface, so both halves are
        // loopback and they agree.
        NodeConfig defaulted = singleMachine;
        defaulted.advertise.clear();
        CHECK_FALSE(StartupPolicyRejection(defaulted).has_value());

        // And the three shapes that must NOT be refused, each of which would be a
        // working deployment this rule had broken.

        // The one-machine deployment: no membership flags, so no peer is admitted,
        // so nothing is told to dial anything. This is what an operator gets by
        // installing the package and starting a node, and it is correct.
        NodeConfig alone;
        alone.scheduler = "127.0.0.1:6675";
        CHECK_FALSE(StartupPolicyRejection(alone).has_value());

        // A node sharing its CACHE tier with listed peers and REGISTERING NOWHERE.
        // That surface is reached at `--listen-node`, so its advertise is never
        // sent to anybody and the wildcard costs it nothing -- which is why the three
        // advertise rows are scoped to `!c.scheduler.empty()`.
        //
        // **But "registering nowhere" is not a node that starts**, and that is worth
        // knowing rather than asserting past: `--scheduler` is required of every node,
        // so the exemption those rows carry describes a configuration the binary
        // refuses. This case could assert otherwise only because the scheduler rule
        // lived in `main.cpp`, which no test target compiles
        // ([#386](https://github.com/LASTRADA-Software/fastcached/issues/386)).
        //
        // So the property is asserted the way it is actually observable: such a config
        // IS refused, and refused for the scheduler rather than for the advertise. The
        // scope clause therefore still does what it says -- it just cannot decide
        // anything at startup, which is recorded here rather than changed, being
        // nobody's ticket yet.
        NodeConfig cacheOnly;
        cacheOnly.fleetMembers = { "10.0.0.1:6676" };
        cacheOnly.clusterKeyFile = "cluster.key"; // it admits another machine (#282)
        auto const cacheOnlyRefusal = StartupPolicyRejection(cacheOnly);
        REQUIRE(cacheOnlyRefusal.has_value());
        CHECK(Unwrap(cacheOnlyRefusal).contains("--scheduler"));
        CHECK_FALSE(Unwrap(cacheOnlyRefusal).contains("--advertise"));

        // And the worker the getting-started page documents, which names both.
        NodeConfig worker;
        worker.scheduler = "scheduler.internal:6675";
        // BOTH, as the getting-started page now says: --advertise alone leaves the
        // surface on loopback and the worker unreachable.
        worker.nodeListen = "0.0.0.0:6674";
        worker.advertise = "worker-01.internal:6674";
        worker.fleetOpen = true;
        worker.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(worker).has_value());
    }

    SECTION("a worker that names neither flag and registers with a remote scheduler")
    {
        // #463. The three rows above all judge a flag somebody TYPED, so the worker
        // that typed neither reached none of them: `--advertise` falls back to the
        // `Node` surface and `--listen-node` defaults to loopback, so the advertise
        // and the bind agree and the rule about their disagreement is silent. That
        // node registers `127.0.0.1:6674` with a scheduler on another machine and
        // every client it is leased to dials its own box.
        //
        // Refused at install time since #290 stage 3 and by nothing at a hand start,
        // which is the asymmetry the tables were composed to delete -- so both paths
        // are asserted here.
        NodeConfig bare;
        bare.scheduler = "scheduler.internal:6675";
        bare.fleetOpen = true;

        auto const refusal = StartupPolicyRejection(bare);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--advertise"));
        CHECK(Unwrap(refusal).contains("--listen-node"));
        CHECK(Unwrap(refusal).contains("dial ITSELF"));
        CHECK(NodeInstallRejection(bare).has_value());

        // A listed member set rather than `--fleet-open` reaches the same row: the
        // admission gate is the siblings' and answers either spelling.
        NodeConfig listed;
        listed.scheduler = "scheduler.internal:6675";
        listed.fleetMembers = { "10.0.0.1:6674" };
        CHECK(StartupPolicyRejection(listed).has_value());

        // And an explicit loopback `--advertise` is the same configuration spelled
        // out, judged on the endpoint rather than on which flag produced it.
        NodeConfig spelled = bare;
        spelled.advertise = "127.0.0.1:6674";
        CHECK(StartupPolicyRejection(spelled).has_value());

        // **Told apart from its three siblings**, for the reason they are told apart
        // from each other: the way this regresses is one predicate widening to cover
        // several, which leaves an operator reading about a flag they never wrote.
        NodeConfig wide = bare;
        wide.nodeListen = "0.0.0.0:6674";
        wide.advertise = "127.0.0.1:6674"; // a loopback advertise over a NETWORK bind
        wide.clusterKeyFile = "cluster.key";
        auto const wideRefusal = StartupPolicyRejection(wide);
        REQUIRE(wideRefusal.has_value());
        CHECK(Unwrap(wideRefusal) != Unwrap(refusal));

        // The two configurations this row must NOT refuse, each of which is a
        // working deployment.

        // The single-machine fleet, which is the whole reason the scheduler's host
        // decides this and not the bind: every address here is loopback and that is
        // correct. `dist-compile-e2e.sh` runs exactly this.
        NodeConfig singleMachine = bare;
        singleMachine.scheduler = "127.0.0.1:6675";
        CHECK_FALSE(StartupPolicyRejection(singleMachine).has_value());

        // **And the same fleet written the way people actually write it.**
        // `IsLoopbackHost` deliberately refuses to call `localhost` loopback, because
        // it is asked who may read this node's cache tier and a name a resolver
        // decides cannot answer that. Reused here unqualified, it would refuse a
        // working one-machine install and tell the operator to bind the wildcard --
        // the rule steering somebody into a wider surface than they had.
        for (auto const* spelling: { "localhost:6675", "[::1]:6675", "127.0.0.5:6675" })
        {
            INFO("--scheduler=" << spelling);
            NodeConfig sameBox = bare;
            sameBox.scheduler = spelling;
            CHECK_FALSE(StartupPolicyRejection(sameBox).has_value());
        }

        // A `--scheduler` that is not `host:port` is still not THIS row's business, and
        // since [#968](https://github.com/LASTRADA-Software/fastcached/issues/968) the
        // shape row answers it first. The expectation moves from "not refused" to
        // "refused by the OTHER rule", which is what this block's own comment argued
        // for before such a rule existed: a bare port reaches `HostOfEndpoint` as a
        // bare HOST, so it reads as "not loopback" and this row would have said where
        // the scheduler is when the fault is the value's shape.
        //
        // Asserted on WHICH refusal, never on the mere fact of one -- a case checking
        // only `has_value()` would pass whichever rule fired, which is the same green
        // it gave before the row existed.
        NodeConfig malformed = bare;
        malformed.scheduler = "6675";
        auto const shapeRefusal = StartupPolicyRejection(malformed);
        REQUIRE(shapeRefusal.has_value());
        CHECK(Unwrap(shapeRefusal).contains("--scheduler=6675"));
        CHECK(Unwrap(shapeRefusal).contains("an address to dial"));

        // And the worker that answered the refusal: both flags named, so nothing
        // about it is loopback any more.
        NodeConfig fixed = bare;
        fixed.nodeListen = "0.0.0.0:6674";
        fixed.advertise = "worker-01.internal:6674";
        fixed.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(fixed).has_value());
    }

    SECTION("a joiner with no consensus port")
    {
        // A node waiting to be admitted is still running consensus, and the port is
        // where the leader that admits it dials it. Without one nothing binds and no
        // admission could ever arrive, so it would wait forever.
        //
        // It used to be `--node-id` this row asked for, which is #1022's move: the id
        // stopped being the switch, so a joiner missing it is no longer a joiner that
        // cannot work -- a joiner missing the PORT is.
        NodeConfig cfg;
        cfg.raftJoin = true;
        cfg.raftPeers = { Peer("n1=10.0.0.4:6680") };

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--listen-raft"));
    }

    SECTION("a joiner naming nothing at all")
    {
        // Its own address is the half only it knows -- and without the cluster's it
        // cannot answer the leader that admits it, which is what makes the leader
        // walk its replication back to an empty log. Admitted, dialled, and
        // permanently silent.
        NodeConfig cfg;
        cfg.raftJoin = true;
        cfg.raftListen = "6680";

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--raft-peer"));
    }

    SECTION("the working shapes are accepted")
    {
        // Each carries `--cluster-key-file`, which every shape admitting another
        // machine now needs: the scheduler signs a grant and the worker checks it,
        // and neither is possible without the key (#282).
        // Each also names `--scheduler`, which every node needs to start at all -- a
        // scheduler included, and it names itself. These fixtures did not, so the
        // shapes they called "accepted" were shapes the binary refuses; the rule was an
        // inline `if` in `main.cpp` and no test could see it (#386).
        NodeConfig listed;
        listed.serveScheduler = true;
        listed.scheduler = std::string { SelfScheduler };
        listed.fleetMembers = { "10.0.0.1:6676" };
        listed.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(listed).has_value());

        NodeConfig open;
        open.serveScheduler = true;
        open.scheduler = std::string { SelfScheduler };
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
        joiner.scheduler = std::string { SchedulerEndpoint };
        CHECK_FALSE(StartupPolicyRejection(joiner).has_value());

        // And the minimum startable worker -- a scheduler and nothing else -- is
        // untouched by any of this. That is the claim this line always meant to make.
        //
        // **It used to be `StartupPolicyRejection(NodeConfig {})`, asserted to be
        // ACCEPTED, under a comment calling a worker with no scheduler "by far the
        // common case".** The binary has never agreed: `main()` refused an empty
        // `--scheduler` outright, and every documented command line names one,
        // including the scheduler's own and the cache-tier node's. A default
        // `NodeConfig` is not a deployment -- it is a struct -- and while the rule
        // lived in a file no test target compiles, this suite could hold a belief
        // about which nodes exist that production had been contradicting all along
        // ([#386](https://github.com/LASTRADA-Software/fastcached/issues/386)).
        NodeConfig minimal;
        minimal.scheduler = std::string { SchedulerEndpoint };
        CHECK_FALSE(StartupPolicyRejection(minimal).has_value());

        // And the section's real claim, now stated rather than implied: none of the
        // rows above depends on `--scheduler`. A bare config is refused, and refused
        // for the scheduler ALONE -- so a row here that started answering in its place
        // would fail this rather than hide behind it.
        auto const bare = StartupPolicyRejection(NodeConfig {});
        REQUIRE(bare.has_value());
        CHECK(Unwrap(bare).contains("--scheduler"));
        CHECK_FALSE(Unwrap(bare).contains("--fleet-member"));
        CHECK_FALSE(Unwrap(bare).contains("--raft-peer"));

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
        listedWorker.nodeListen = "0.0.0.0:6674";
        listedWorker.advertise = "worker-01.internal:6674";
        listedWorker.fleetMembers = { "10.0.0.1:6674" };
        listedWorker.clusterKeyFile = "cluster.key";
        CHECK_FALSE(StartupPolicyRejection(listedWorker).has_value());

        NodeConfig openWorker;
        openWorker.scheduler = "scheduler.internal:6675";
        openWorker.nodeListen = "0.0.0.0:6674";
        openWorker.advertise = "worker-01.internal:6674";
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
        // that was asked for, and a tier that was never built. `--listen-node=`
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
    CHECK(NodeConfig {}.nodeListen == Cc::DefaultAddr);

    // Loopback, and that is the anti-leeching half rather than a preference: this
    // surface serves this machine's whole build output, so reaching it from the
    // network has to be something an operator typed.
    CHECK(NodeConfig {}.nodeListen.starts_with("127.0.0.1:"));

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
    ///
    /// `--serve-scheduler` says this node RUNS a scheduler; `--scheduler` says where it
    /// registers, and is required of every node including one that schedules -- it names
    /// itself. This lambda set only the first, so the configurations it built could not
    /// have started, and nothing here could tell while that rule lived as an inline `if`
    /// in `main.cpp` ([#386](https://github.com/LASTRADA-Software/fastcached/issues/386)).
    /// The comment above already claimed both; now it is true.
    auto const servingNode = [] {
        NodeConfig cfg;
        cfg.serveScheduler = true;
        cfg.scheduler = std::string { SelfScheduler };
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
        cfg.serveScheduler = false;

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--dashboard"));
        CHECK(Unwrap(refusal).contains("--serve-scheduler"));
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
    auto const pinned = ParseNodeArgv({ "--listen-node=127.0.0.1:6674" });
    REQUIRE(pinned.has_value());
    CHECK(pinned->nodeListen == NodeConfig {}.nodeListen);
    CHECK(pinned->nodeListenExplicit);

    // Turning the tier off is a decision too, and an empty value is as far from the
    // default as a port is.
    auto const off = ParseNodeArgv({ "--listen-node=" });
    REQUIRE(off.has_value());
    CHECK(off->nodeListenExplicit);

    auto const silent = ParseNodeArgv({ "--scheduler=s:1" });
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->nodeListenExplicit);
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
    //
    // Both halves start from a config with EVERY bit cleared rather than from
    // `Installable()` as it comes. That fixture names two flags of its own since
    // #713, so building on it directly would make the first half pass for
    // `--scheduler` and `--advertise` whether or not their bit was the one set, and
    // would make the second half assert silence about a config that is not silent.
    // Cleared by walking the table, so a row added later arrives here already
    // handled.
    auto const named = [](NodeConfig cfg) {
        for (auto const& option: NodeOptions())
            if (option.explicitBit != nullptr)
                cfg.*option.explicitBit = false;
        return cfg;
    };

    for (auto const& option: NodeOptions())
    {
        if (option.explicitBit == nullptr)
            continue;

        auto pinned = named(Installable());
        pinned.*option.explicitBit = true;
        INFO("flag: " << option.primary);

        auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", pinned);
        CHECK(std::ranges::any_of(spec.arguments,
                                  [&option](std::string const& arg) { return FlagMatches(arg, option.primary); }));
    }

    // Its converse, or "always emit it" would pass: a setting nobody named stays out
    // of the registration, so the machine's default is not frozen at install time.
    auto const silent = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", named(Installable()));
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
    pinned.nodeListen = NodeConfig {}.nodeListen;
    pinned.nodeListenExplicit = true;

    auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", pinned);
    auto const reparsed = ReparseSpec(spec);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->nodeListen == pinned.nodeListen);
    CHECK(reparsed->nodeListenExplicit);
}

TEST_CASE("A flag whose value other machines read refuses one that is not text", "[node][config][utf8]")
{
    // Which flags carry text the fleet reads is a COLUMN of the option table, and
    // this pins the column rather than the parser -- `Cli/Options_test.cpp` covers
    // what `ParseUtf8Text` does. Every one of these ends up in a peer's
    // `ClusterState` or in a registration: `--advertise` becomes the endpoint clients
    // dial, `--node-id` and `--raft-peer` a member's identity and the address its
    // peers dial it at.
    //
    // The extent is DERIVED with `to_array` rather than written down. It was
    // `std::array<Row, 4>`, and removing the `--bind` row when #290 stage 3 deleted
    // the flag left a fourth row value-initialized to null pointers -- which
    // segfaulted the whole binary mid-run, taking a hundred later cases with it and
    // reporting as a shrunken test count rather than as a crash.
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

    constexpr auto Rows = std::to_array<Row>({
        { .flag = "--advertise",
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
    });

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
        cfg.serveScheduler = true;
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
    // `nodeListen` and the membership fields and none of the cache ones, so by
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
    cfg.nodeListen = defaults.nodeListen;
    cfg.nodeListenExplicit = true;

    // The default `--listen-node` is LOOPBACK, so the advertise has to agree with it
    // or the reachability rows refuse the pair -- which would fail this case for a
    // reason it is not about. `Installable()` names a routable pair because that is
    // the ordinary fleet worker; pinning the bind to its default makes this the
    // single-machine one.
    //
    // The SCHEDULER is the third half and leaving it remote was wrong before this
    // line ever ran (#463): a node advertising `127.0.0.1` to `cache.internal`
    // registers an address that scheduler's clients cannot dial. It read as a
    // single-machine fleet and was one only in the two fields this comment named.
    //
    // The MEMBER LIST deliberately stays remote, which is what keeps this case about
    // its own subject: `AdmitsRemotePeers` is what makes #282's lease rule apply at
    // all, so an all-loopback list would leave the rule unable to fire and every
    // assertion below true for a reason that is not the one claimed. It costs nothing
    // here -- the bind is loopback, so `CompilePortFacesTheNetwork` is false and the
    // rule permits it.
    cfg.advertise = "127.0.0.1:6674";
    cfg.scheduler = "127.0.0.1:6675";
    cfg.cacheMemoryBytes = defaults.cacheMemoryBytes;
    cfg.cacheMemoryExplicit = true;

    // It installs.
    CHECK_FALSE(NodeInstallRejection(cfg).has_value());

    // And the registration really does carry them -- otherwise this case would pass
    // for the wrong reason, having asserted a rule permits flags that were never
    // emitted.
    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);
    CHECK(std::ranges::any_of(spec.arguments, [](std::string const& a) { return a.starts_with("--listen-node="); }));
    CHECK(std::ranges::any_of(spec.arguments, [](std::string const& a) { return a.starts_with("--cache-memory="); }));

    // The half that actually bites: what a supervisor replays has to pass the
    // startup rules at every boot, not merely at install time.
    auto const reparsed = ReparseSpec(spec);
    REQUIRE(reparsed.has_value());
    CHECK_FALSE(StartupPolicyRejection((*reparsed)).has_value());

    // And provenance survives the round trip, or the second boot silently drops what
    // the first one was told.
    CHECK((*reparsed).nodeListenExplicit);
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
        // Expressed on `--listen-node`, which since #290 stage 3 is the bind. The
        // advertise has to agree with it, or the reachability rows refuse the
        // disagreement rather than the loopback.
        loopbackBound.nodeListen = "127.0.0.1:6674";
        loopbackBound.advertise = "127.0.0.1:6674";
        // And so does the SCHEDULER, which is the half this case claimed and did not
        // have (#463): `Installable()` registers with `cache.internal`, so as written
        // this was a loopback endpoint handed to a machine that could not dial it --
        // the fourth reachability row's exact subject, not the loopback fleet the
        // harnesses run. `dist-compile-e2e.sh` names 127.0.0.1 here too.
        loopbackBound.scheduler = "127.0.0.1:6675";
        loopbackBound.fleetOpen = true;
        CHECK_FALSE(StartupPolicyRejection(loopbackBound).has_value());

        // And a node that ACCEPTS from the network while advertising loopback is
        // refused, which is what makes the check worth having rather than something an
        // ordinary deployment slips past. Asserted on `--listen-node` now: `--bind` is
        // gone, and its wildcard default was what used to express this.
        auto wildcardBound = Installable();
        wildcardBound.fleetOpen = true;
        wildcardBound.nodeListen = "0.0.0.0:6674";
        wildcardBound.advertise = "127.0.0.1:6674";
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

namespace
{

/// Write @p body to a configuration file this case owns.
/// @param dir Scratch directory.
/// @param body YAML text.
/// @return The path written.
[[nodiscard]] std::filesystem::path WriteNodeConfigFile(std::filesystem::path const& dir, std::string_view body)
{
    std::filesystem::create_directories(dir);
    auto const path = dir / "node.yaml";
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out << body;
    return path;
}

/// Read @p path into a fresh configuration, exactly as a reload does.
///
/// A FRESH configuration, never the live one, and an empty argv -- the "no command
/// line to outrank the file" case. Deliberately the same recipe `main` wires into its
/// reloader: writing a different one here would let these cases pass while production
/// reloaded some other way.
/// @param path The configuration file.
/// @return The candidate, or why it could not be read.
[[nodiscard]] std::expected<NodeConfig, ConfigError> ReparseNodeConfig(std::filesystem::path const& path)
{
    NodeConfig candidate;
    auto const loaded = ReadYamlSettings(path).and_then([&candidate, &path](std::vector<YamlSetting> const& settings) {
        return ApplyNodeConfiguration(settings, path, {}, candidate);
    });
    if (!loaded.has_value())
        return std::unexpected(loaded.error());
    return candidate;
}

/// Write a configuration file for a node that could actually be running.
///
/// `WriteNodeConfigFile` plus the `scheduler:` line, so a fixture about reload MECHANICS
/// does not have to restate a startup precondition it is not testing. Named rather than
/// folded into `WriteNodeConfigFile`, because several cases assert on a file's exact
/// contents and a writer that silently added a key would break them for a reason none of
/// them is about.
/// @param dir Where to write it.
/// @param body The keys this case is about.
/// @return The path.
[[nodiscard]] std::filesystem::path WriteRunnableNodeConfigFile(std::filesystem::path const& dir, std::string_view body)
{
    return WriteNodeConfigFile(dir, std::format("scheduler: {}\n{}", SchedulerEndpoint, body));
}

/// The live configuration of a node that could actually be running.
/// @return A config naming a scheduler and nothing else.
[[nodiscard]] NodeConfig RunningNode()
{
    NodeConfig cfg;
    cfg.scheduler = std::string { SchedulerEndpoint };
    return cfg;
}

/// A reloader over @p initial reading @p path, wired as `main` wires one.
/// @param initial What the worker is running with.
/// @param path The configuration file.
/// @return The reloader.
[[nodiscard]] ConfigReloaderOf<NodeConfig> MakeNodeReloader(NodeConfig initial, std::filesystem::path const& path)
{
    return ConfigReloaderOf<NodeConfig> { std::move(initial), path, &ReparseNodeConfig, &ValidateNodeReloadable };
}

} // namespace

TEST_CASE("A reloadable setting reaches the SUBSYSTEM, not just the snapshot", "[node][config][reload]")
{
    // **The case that decides whether the column means anything** (#292).
    //
    // `--log-level` is marked `Reloadable::Yes` because `ILogger::SetMinLevel` exists.
    // That is a fact about the CODE, verified by hand, and the column does not create
    // it -- it records it. A row marked `Yes` whose subsystem does not honour the field
    // is not a policy mistake, it is a LIE about the code, and it produces exactly the
    // failure `Reloadable::No` exists to prevent: a published snapshot the running
    // process disagrees with.
    //
    // So this asserts on what the LOGGER emits, never on `reloader.Current()`. A case
    // that read the value back out of the snapshot would pass for a row that changes
    // nothing at all, which is the whole defect.
    Testing::ScratchDirectory const scratch { "node-reload-live" };
    auto const& dir = scratch.Path();
    auto const path = WriteRunnableNodeConfigFile(dir, "log_level: error\n");

    auto initial = RunningNode();
    initial.logLevel = LogLevel::Info;

    std::ostringstream sink;
    ConsoleLogger logger { sink, initial.logLevel, LogTimestamps::No };

    logger.Logf(LogLevel::Info, "before the reload");
    REQUIRE(sink.str().contains("before the reload"));

    auto reloader = MakeNodeReloader(initial, path);
    REQUIRE(reloader.Reload().has_value());

    // The one line `main` performs on a successful reload.
    logger.SetMinLevel(reloader.Current()->logLevel);

    // The same call is now BELOW the threshold and must produce nothing. This is the
    // subsystem's observable behaviour, and it is the only thing that can tell a
    // reloadable row from one that merely claims to be.
    sink.str({});
    logger.Logf(LogLevel::Info, "after the reload");
    CHECK(sink.str().empty());

    // And the level it was raised TO is in force, not merely "not Info".
    logger.Logf(LogLevel::Error, "an error survives");
    CHECK(sink.str().contains("an error survives"));
}

TEST_CASE("An unreloadable setting that changed is reported, and nothing is applied", "[node][config][reload]")
{
    // The other arm. `--slots` is live-wired: the worker advertised a slot count to the
    // scheduler and sized a pool from it, so a reload changing it would leave the
    // scheduler dispatching against a number this worker no longer serves -- which is
    // the argument `main.cpp`'s SIGHUP comment used to give for handling no reload at
    // all, restated as the reason this row is not marked.
    Testing::ScratchDirectory const scratch { "node-reload-refused" };
    auto const& dir = scratch.Path();
    auto const path = WriteRunnableNodeConfigFile(dir, "slots: 9\n");

    auto initial = RunningNode();
    initial.slots = 4;

    auto reloader = MakeNodeReloader(initial, path);
    auto const outcome = reloader.Reload();

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == ConfigErrorCode::ImmutableChanged);
    // Named, because "reload failed" sends an operator reading logs rather than the one
    // line that says which setting, and what happened as a result.
    CHECK(outcome.error().context.contains("--slots"));
    CHECK(outcome.error().context.contains("nothing was applied"));

    CHECK(reloader.Current()->slots == 4);
}

TEST_CASE("One save touching both is refused whole, and names every unreloadable field", "[node][config][reload]")
{
    // **The case that decides reject-all is real.** The issue's two clauses are each
    // single-field, so both are satisfied by either semantics; only a save touching a
    // reloadable AND an unreloadable field distinguishes them. Partial application
    // would leave the file and the running process disagreeing field by field, with no
    // single artefact describing what is in force -- and the operator saved ONCE.
    Testing::ScratchDirectory const scratch { "node-reload-mixed" };
    auto const& dir = scratch.Path();
    auto const path = WriteNodeConfigFile(dir, "log_level: error\nslots: 9\nnode_class: dedicated\n");

    NodeConfig initial;
    initial.logLevel = LogLevel::Info;
    initial.slots = 4;

    auto reloader = MakeNodeReloader(initial, path);
    REQUIRE_FALSE(reloader.Reload().has_value());

    // The reloadable half did NOT sneak through. This assertion is the one that fails
    // under apply-the-reloadable-and-report-the-rest.
    CHECK(reloader.Current()->logLevel == LogLevel::Info);
    CHECK(reloader.Current()->slots == 4);

    // And EVERY unreloadable field that changed is named, never the first: a refusal
    // that stopped at one sends the operator round the same loop per field.
    auto const candidate = ReparseNodeConfig(path);
    REQUIRE(candidate.has_value());
    auto const changed = UnreloadableChanges(*reloader.Current(), *candidate);
    CHECK(std::ranges::contains(changed, std::string_view { "--slots" }));
    CHECK(std::ranges::contains(changed, std::string_view { "--node-class" }));
    // The reloadable one is absent from the refusal, or the report would tell an
    // operator to restart for something that did not need it.
    CHECK_FALSE(std::ranges::contains(changed, std::string_view { "--log-level" }));
}

TEST_CASE("A file that fails halfway is declined, never half-applied", "[node][config][reload]")
{
    // The clause nobody would think to write. A document whose first key is good and
    // whose second is not must leave the worker running what it had -- not "some of the
    // settings, up to the bad line", which is a configuration nobody wrote.
    //
    // It holds by CONSTRUCTION rather than by a guard: the reparse builds a fresh
    // NodeConfig and returns it only on success, so a partial application is discarded
    // with the candidate. That is why this asserts the live snapshot rather than a
    // rejection path -- there is no guard here that could later be removed.
    Testing::ScratchDirectory const scratch { "node-reload-halfparse" };
    auto const& dir = scratch.Path();
    auto const path = WriteNodeConfigFile(dir, "log_level: error\nslots: not-a-number\n");

    NodeConfig initial;
    initial.logLevel = LogLevel::Info;
    initial.slots = 4;

    auto reloader = MakeNodeReloader(initial, path);
    REQUIRE_FALSE(reloader.Reload().has_value());

    // The GOOD key that preceded the bad one is not in force either.
    CHECK(reloader.Current()->logLevel == LogLevel::Info);
    CHECK(reloader.Current()->slots == 4);
}

TEST_CASE("Every reloadable row is one the option table can actually compare", "[node][config][reload]")
{
    // A row marked `Reloadable::Yes` with no comparator would be skipped by
    // `UnreloadableChanges` for the wrong reason -- not because it may change, but
    // because nothing can tell whether it did. The static_assert beside the table
    // forbids that shape already; this states the property the reload path depends on,
    // so a reader of the reload code need not go and find that assertion.
    //
    // Derived from the table, never restated: a row added tomorrow is covered.
    std::vector<OptionSpec<NodeConfig> const*> reloadable;
    for (auto const& spec: NodeOptions())
        if (spec.reloadable == Reloadable::Yes)
            reloadable.push_back(&spec);

    // Not vacuous. Nothing being reloadable would satisfy every other case here, and
    // this is what fails if the one marked row is ever unmarked.
    REQUIRE(!reloadable.empty());
    for (auto const* spec: reloadable)
    {
        INFO("flag: " << spec->primary);
        CHECK(spec->same != nullptr);
        CHECK_FALSE(spec->yamlKey.empty());
    }
}

TEST_CASE("A toolchain change is reloadable, and says the fleet must be told", "[node][config][reload]")
{
    // #403. The band that is not local wiring but a CLAIM this worker made to the
    // scheduler at REGISTER. `--toolchain` is the one an operator actually edits: a
    // compiler was installed, or one is being retired from the fleet a machine at a
    // time.
    //
    // Two assertions, and the second is the one that matters. That the reload is
    // ACCEPTED is necessary and proves nothing on its own -- a row marked
    // `Reloadable::Yes` whose change never reaches the fleet is precisely the silent
    // failure the column exists to prevent, and it would satisfy the first assertion
    // perfectly. So the acceptance is paired with `AdvertisedClaimsDiffer`, which is
    // what the heartbeat thread reads to decide whether to re-derive and re-register.
    Testing::ScratchDirectory const scratch { "node-reload-toolchain" };
    auto const& dir = scratch.Path();
    auto const path = WriteRunnableNodeConfigFile(dir, "toolchain:\n  - /usr/bin/g++\n");

    auto initial = RunningNode();
    initial.toolchains = { "/usr/bin/clang++" };

    auto reloader = MakeNodeReloader(initial, path);
    auto const before = reloader.Current();
    auto const outcome = reloader.Reload();

    REQUIRE(outcome.has_value());
    CHECK(reloader.Current()->toolchains == std::vector<std::string> { "/usr/bin/g++" });

    // The half that fails open: accepted, adopted, and the scheduler never told.
    CHECK(Node::AdvertisedClaimsDiffer(*before, *reloader.Current()));
}

TEST_CASE("Turning discovery off is reloadable, and also a claim", "[node][config][reload]")
{
    // The band's other member, and it has to be turned off ALONGSIDE naming a
    // toolchain: on its own it is a worker with nothing to serve, which the startup
    // rules refuse and a reload may therefore not reach either. That case is asserted
    // separately below; this one is the legitimate edit -- an operator pinning the set
    // and telling the node to stop searching the machine.
    Testing::ScratchDirectory const scratch { "node-reload-discovery" };
    auto const& dir = scratch.Path();
    auto const path = WriteRunnableNodeConfigFile(dir, "no_toolchain_discovery: true\ntoolchain:\n  - /usr/bin/g++\n");

    auto initial = RunningNode();
    REQUIRE(initial.toolchainDiscovery);

    auto reloader = MakeNodeReloader(initial, path);
    auto const before = reloader.Current();
    REQUIRE(reloader.Reload().has_value());

    CHECK_FALSE(reloader.Current()->toolchainDiscovery);
    CHECK(Node::AdvertisedClaimsDiffer(*before, *reloader.Current()));
}

TEST_CASE("A log-level reload does not re-derive what this worker serves", "[node][config][reload]")
{
    // **The direction that costs minutes if it is wrong.** Re-deriving the served set
    // spawns a driver per compiler and walks every include tree -- over 300 s on a
    // cold machine (#354) -- and the moment an operator raises the log level is
    // mid-incident, which is the worst possible time to spend that.
    //
    // So `AdvertisedClaimsDiffer` is asked rather than assumed, and this is the
    // assertion that fails if it is ever replaced by "a reload happened".
    Testing::ScratchDirectory const scratch { "node-reload-loglevel-only" };
    auto const& dir = scratch.Path();
    // The file carries BOTH, and only the level moves. A file naming no toolchain at
    // all would not test this: a reload builds a FRESH configuration and applies the
    // file to it, so a `toolchain:` key that disappears from the file legitimately
    // returns this worker to discovery -- a real change, correctly reported as one.
    // Written the other way round, this case asserted that reverting to discovery is
    // NOT a change, which is the opposite of true and would have hidden it.
    auto const path = WriteRunnableNodeConfigFile(dir, "log_level: error\ntoolchain:\n  - /usr/bin/g++\n");

    auto initial = RunningNode();
    initial.logLevel = LogLevel::Info;
    initial.toolchains = { "/usr/bin/g++" };

    auto reloader = MakeNodeReloader(initial, path);
    auto const before = reloader.Current();
    REQUIRE(reloader.Reload().has_value());

    REQUIRE(reloader.Current()->logLevel == LogLevel::Error);
    CHECK_FALSE(Node::AdvertisedClaimsDiffer(*before, *reloader.Current()));
}

TEST_CASE("The capacity trio and --advertise stay unreloadable, each for its own reason", "[node][config][reload]")
{
    // #403 moved the toolchain pair and deliberately left the rest of the band where
    // it was. Asserted rather than left to the table, because "we decided not to" and
    // "we forgot" are the same diff.
    //
    // `--advertise` is the one that could not move even if the capacity trio did: it
    // is baked into every outstanding lease's MAC as `expected.endpoint`, so changing
    // it mid-life does not merely mislead the scheduler -- it invalidates every grant
    // already in a client's hands, which then fail `EndpointMismatch`.
    for (auto const* flag: { "--advertise", "--slots", "--node-class", "--reserve-cores" })
    {
        INFO("flag: " << flag);
        auto const rows = NodeOptions();
        auto const spec =
            std::ranges::find_if(rows, [flag](OptionSpec<NodeConfig> const& row) { return row.primary == flag; });
        REQUIRE(spec != rows.end());
        CHECK(spec->reloadable == Reloadable::No);
    }
}

TEST_CASE("A reload forces the survey a witness would never trigger", "[node][config][reload]")
{
    // **The gap between "the reload was accepted" and "anything happened".**
    // `RefreshToolchains` is driven by WITNESSES -- a compiler's stamp moving on disk
    // -- and editing a configuration file moves none of them. Without the forced
    // depth, a reload that changed `--toolchain` would be accepted, logged, and then
    // do nothing at all until the periodic sweep came round, which is up to
    // `SweepEveryBeats` heartbeats away. An operator would have saved a file, seen it
    // accepted, and watched the fleet keep dispatching the old set.
    //
    // This is a pure function precisely so it can be asserted: it lived as an
    // expression inside the heartbeat loop, in `main.cpp`, which is in no test target.
    using Node::ClaimsReloaded;
    using Node::RecheckDepth;
    using Node::RecheckDepthFor;

    // A reload forces it on ANY beat, including the ones the sweep would skip.
    CHECK(RecheckDepthFor(ClaimsReloaded::Yes, 1, 45) == RecheckDepth::Unconditional);
    CHECK(RecheckDepthFor(ClaimsReloaded::Yes, 44, 45) == RecheckDepth::Unconditional);

    // And without one, the ordinary cadence is unchanged -- the half that keeps a
    // heartbeat cheap. A beat that is not a sweep asks the witnesses and spawns
    // nothing.
    CHECK(RecheckDepthFor(ClaimsReloaded::No, 1, 45) == RecheckDepth::WhenEvidenceMoved);
    CHECK(RecheckDepthFor(ClaimsReloaded::No, 44, 45) == RecheckDepth::WhenEvidenceMoved);
    CHECK(RecheckDepthFor(ClaimsReloaded::No, 45, 45) == RecheckDepth::Unconditional);
    CHECK(RecheckDepthFor(ClaimsReloaded::No, 90, 45) == RecheckDepth::Unconditional);

    // Zero would be a division by zero on the heartbeat thread. It means "never
    // sweep", which leaves the witnesses and a reload as the two triggers -- not a
    // crash, and not a survey on every single beat either.
    CHECK(RecheckDepthFor(ClaimsReloaded::No, 7, 0) == RecheckDepth::WhenEvidenceMoved);
    CHECK(RecheckDepthFor(ClaimsReloaded::Yes, 7, 0) == RecheckDepth::Unconditional);
}

TEST_CASE("A malformed toolchain is declined by the reload, not applied", "[node][config][reload]")
{
    // The grammar used to be enforced at SURVEY time, which was harmless while this
    // flag could only arrive at startup: a bad value exited with a message and nothing
    // had been applied. Reloadable, it stopped being harmless -- the value passed the
    // applier, was published, and the heartbeat thread then discovered it was malformed
    // with nothing left to do but serve NOTHING, unrecoverably, because an empty served
    // set has no witnesses to notice a correction with.
    //
    // Both halves of a pinned toolchain have to be non-empty, which is exactly what
    // `SplitToolchain` says; this asserts the reload asks it.
    for (auto const* bad: { "toolchain:\n  - =/usr/bin/g++\n", "toolchain:\n  - abc=\n" })
    {
        INFO(bad);
        Testing::ScratchDirectory const scratch { "node-reload-bad-toolchain" };
        auto const path = WriteNodeConfigFile(scratch.Path(), bad);

        NodeConfig initial;
        initial.toolchains = { "/usr/bin/g++" };

        auto reloader = MakeNodeReloader(initial, path);
        CHECK_FALSE(reloader.Reload().has_value());

        // The live configuration is untouched, which is the half that matters: the
        // worker goes on serving what it was serving.
        CHECK(reloader.Current()->toolchains == std::vector<std::string> { "/usr/bin/g++" });
    }
}

TEST_CASE("A reload cannot reach a state startup would have refused", "[node][config][reload]")
{
    // `--no-toolchain-discovery` with no `--toolchain` is a worker with nothing to
    // serve. Startup refuses it by name; until #403 no reload could produce it, because
    // neither flag was reloadable. Both are now, so the reload path composes the
    // startup rules exactly as `NodeInstallRejection` does -- the same reasoning, since
    // whatever is accepted here becomes the configuration in force.
    Testing::ScratchDirectory const scratch { "node-reload-nothing-to-serve" };
    auto const path = WriteNodeConfigFile(scratch.Path(), "no_toolchain_discovery: true\n");

    NodeConfig initial;
    initial.toolchains = { "/usr/bin/g++" };

    auto reloader = MakeNodeReloader(initial, path);
    auto const outcome = reloader.Reload();

    REQUIRE_FALSE(outcome.has_value());
    // Named, so an operator is told which setting and not merely that something failed.
    CHECK(outcome.error().context.contains("--toolchain"));
    CHECK(reloader.Current()->toolchains == std::vector<std::string> { "/usr/bin/g++" });
}

TEST_CASE("NodeSecretFiles: every path-valued flag is classified, secret or not", "[node][config][secret]")
{
    // **The coverage guard, and it is mandatory rather than opt-in.** A list that is
    // exact about the flags it knows and silent about the ones it does not reads
    // identically to complete coverage (#492) -- and #752 is written about exactly
    // that failure: answering the narrow half while looking complete. So a further
    // `=<path>` row cannot be added without its author saying which kind it is --
    // `NodeOptions()` has nine of them today, four secret and five public.
    //
    // The join itself is `Testing::ClassifyPathFlags`, shared with the daemon's twin
    // since [#864](https://github.com/LASTRADA-Software/fastcached/issues/864) gave
    // that binary the same two tables -- one rule asked of two option tables, rather
    // than two Catch2 cases that drift on which directions they check.
    auto const secret = NodeSecretFileTable();
    auto const publicPaths = NodePublicPathFlags();
    auto const coverage = FastCache::Testing::ClassifyPathFlags<NodeConfig>(
        NodeOptions(), FastCache::Testing::FlagsOf(secret), FastCache::Testing::FlagsOf(publicPaths));

    SECTION("every =<path> row of NodeOptions() is in exactly one table")
    {
        // Three assertions rather than one, because "nobody classified it", "both
        // tables claim it" and "a table names a flag that no longer exists" are three
        // different repairs. A single verdict would name none of them -- and the last
        // is not decoration: a flag renamed in the option table leaves a row here
        // naming nothing, and the file it used to cover goes unasked about with both
        // tables still looking full.
        INFO("unclassified: " << FastCache::Testing::Join(coverage.unclassified));
        CHECK(coverage.unclassified.empty());

        INFO("classified twice: " << FastCache::Testing::Join(coverage.classifiedTwice));
        CHECK(coverage.classifiedTwice.empty());

        INFO("naming no row: " << FastCache::Testing::Join(coverage.namingNoRow));
        CHECK(coverage.namingNoRow.empty());

        // The positive control. A scan that matched nothing would leave all three
        // lists empty and pass, and "no violations found" and "the scan found nothing
        // to look at" are the two states this codebase keeps having to tell apart.
        CHECK(coverage.pathRows > 0);
        CHECK(coverage.pathRows == secret.size() + publicPaths.size());
    }

    SECTION("every public row says why")
    {
        for (auto const& row: publicPaths)
        {
            INFO("public row: " << row.flag);
            // The reason is a forcing function, not a dead field: a blank one would
            // spell "forgot" in the vocabulary of "decided".
            CHECK_FALSE(row.why.empty());
        }
    }

    SECTION("--tls-cert is classified as public, deliberately")
    {
        // Decided explicitly rather than by omission, because it is the one an author
        // would add by symmetry with `--tls-key`. A certificate is presented to every
        // client during the handshake, so warning about the mode of a file that is
        // MEANT to be readable is the alarm that teaches operators to ignore the four
        // that matter.
        CHECK(FastCache::Testing::Names(publicPaths, "--tls-cert"));
        CHECK(FastCache::Testing::Names(secret, "--tls-key"));
    }
}

TEST_CASE("NodeSecretFiles: a path-reached secret is not provenance-gated", "[node][config][secret]")
{
    // **#752's design decision, asserted where it can fail.** #384's rule is gated on
    // provenance because `--requirepass` can also arrive in argv, where the exposure is
    // `ps` and belongs to `InlineCredentialRejection`. A key FILE has no second route:
    // the path is not the secret and the file is. Wiring these four through the
    // provenance gate would silently skip every argv-named key file -- a change no test
    // asserting merely that "some warning arrives" could see.
    NodeConfig cfg;
    cfg.clusterKeyFile = "/etc/fastcached/cluster.key";
    cfg.schedulerTokenFile = "/etc/fastcached/scheduler.token";
    cfg.dashboardTokenFile = "/etc/fastcached/dashboard.token";
    cfg.tlsKeyFile = "/etc/fastcached/admin.key";
    cfg.tlsCertFile = "/etc/fastcached/admin.crt";

    SECTION("named in argv, with no configuration file at all")
    {
        // The provenance gate answers "no file" here, and all four must still be asked
        // about.
        auto const files = NodeSecretFiles(cfg, {}, /*secretNamedOnCommandLine*/ true);
        CHECK(files
              == std::vector<std::filesystem::path> {
                  cfg.clusterKeyFile, cfg.schedulerTokenFile, cfg.dashboardTokenFile, cfg.tlsKeyFile });
    }

    SECTION("the certificate is never asked about")
    {
        auto const files = NodeSecretFiles(cfg, {}, false);
        CHECK(std::ranges::find(files, cfg.tlsCertFile) == files.end());
    }

    SECTION("an unnamed flag contributes nothing")
    {
        NodeConfig bare;
        bare.clusterKeyFile = "/etc/fastcached/cluster.key";
        CHECK(NodeSecretFiles(bare, {}, false) == std::vector<std::filesystem::path> { bare.clusterKeyFile });
    }
}

TEST_CASE("NodeSecretFiles: the worker actually asks, and that is asserted", "[node][config][secret]")
{
    // **A check nothing constructs is the bug it was written to fix**, and the rule
    // has two call sites in this batch rather than one: the daemon's is asserted by
    // `SecretExposureWatcher_test`, and this is the worker's. `NodeSecretFiles` and its
    // coverage guard could be perfect with `main()` never calling either, and every
    // test above would still pass -- which is #752 answered on paper.
    //
    // A source scan because `main.cpp` is in no test target. Full-line comments are
    // stripped first: a COMMENT is not a call site, and the call site here sits under
    // twenty lines of comment that name the very function being looked for.
    std::ifstream source { std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node"
                           / "main.cpp" };
    REQUIRE(source.is_open());

    std::string code;
    for (std::string line; std::getline(source, line);)
    {
        auto const first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0)
            continue;
        code += line;
        code += '\n';
    }

    // The positive control: a renamed file, a moved body or an empty read would
    // otherwise report "not called" for a worker that calls it perfectly well. This
    // anchor predates the change and is the line the warning must follow.
    REQUIRE(code.contains("StartupPolicyRejection(cfg)"));

    CHECK(code.contains("NodeSecretFiles("));

    // BOTH moments, and they are separate assertions because they are separate
    // wirings: `ReportSecretExposure` is the arm for a worker with no configuration
    // file to reload, and `WatchSecretExposure` is the arm that follows one. A scan
    // for either alone would pass a `main` that had lost the other, which is #868 on
    // one side and #752 on the other.
    CHECK(code.contains("ReportSecretExposure<NodeConfig>("));
    CHECK(code.contains("WatchSecretExposure<NodeConfig>("));
}

TEST_CASE("NodeSecretFiles: the configuration file is gated on provenance", "[node][config][secret]")
{
    // The other half, and it is #384's rule unchanged: the file's mode is what protects
    // a secret only when the file is what supplied it.
    std::filesystem::path const configFile { "/etc/fastcached/fastcache-compile-node.yaml" };

    NodeConfig cfg;
    cfg.token = "hunter2";

    SECTION("a secret out of the file is asked about")
    {
        CHECK(NodeSecretFiles(cfg, configFile, false) == std::vector<std::filesystem::path> { configFile });
    }

    SECTION("a secret argv supplied is a different exposure")
    {
        CHECK(NodeSecretFiles(cfg, configFile, true).empty());
    }

    SECTION("no secret in force means the file's mode is not this worker's business")
    {
        NodeConfig quiet;
        CHECK(NodeSecretFiles(quiet, configFile, false).empty());
    }

    SECTION("a run that read no file names none")
    {
        CHECK(NodeSecretFiles(cfg, {}, false).empty());
    }

    SECTION("the configuration file comes first")
    {
        // It is the file an operator most often has open, and the order is the
        // caller's rather than the filesystem's.
        cfg.clusterKeyFile = "/etc/fastcached/cluster.key";
        CHECK(NodeSecretFiles(cfg, configFile, false)
              == std::vector<std::filesystem::path> { configFile, cfg.clusterKeyFile });
    }
}

TEST_CASE("An operator who edits --listen-raft is answered by the right rule", "[node][config][reload][consensus]")
{
    // **#1022's reload question, driven rather than inferred.** `--listen-raft` is
    // `Reloadable::No`, so an operator who edits it should be told the setting is
    // immutable. But `ValidateNodeReloadable` asks `StartupPolicyRejection(candidate)`
    // FIRST -- deliberately, and the comment there says why -- and four of the rows
    // #1022 rewrote read `RunsConsensus`, which is exactly what this key decides. So a
    // startup row can answer in the immutability check's place, and which answer an
    // operator gets is not obvious from either site alone.
    //
    // The measured answer is that the two cases are cleanly separated, and the line
    // that separates them is whether the edit MOVES `RunsConsensus`:
    //
    //   * An edit that leaves consensus on -- a host or a port change -- fires none of
    //     the five, and the operator is told the setting is immutable. That is the
    //     right answer: the file describes a node that could start, just not this one
    //     without a restart.
    //   * An edit that turns consensus ON or OFF necessarily produces a candidate that
    //     could not START either, so a startup row answers instead. That is also the
    //     right answer, and it is the STRONGER one: telling such an operator to restart
    //     would send them into a refusal at boot. The necessity is the interesting
    //     half -- a node running no consensus can carry no `--raft-self`, `--node-id`,
    //     `--raft-peer` or `--discovery` (each is refused at startup without
    //     `--listen-raft`), so adding the switch alone leaves nothing naming this node;
    //     and a node running consensus must name itself one of those two ways, so
    //     dropping the switch alone strands whichever one it used.
    //
    // Every section therefore asserts WHICH row answered and that the OTHER one did
    // not: the messages here share the words "peers dial", so a case matching on that
    // would pass for either and two refusals would be one passing test.
    Testing::ScratchDirectory const scratch { "node-listen-raft-reload" };

    SECTION("a port change on a node that already runs consensus is refused as immutable")
    {
        // `raft_self` is what names this node, so consensus is legitimately on at both
        // ends and the only difference between the two files is the port.
        auto const path = WriteRunnableNodeConfigFile(scratch.Path(), "listen_raft: 0.0.0.0:7000\nraft_self: 10.0.0.7\n");

        auto const previous = ReparseNodeConfig(path);
        REQUIRE(previous.has_value());

        auto reloader = MakeNodeReloader(previous.value(), path);
        (void) WriteRunnableNodeConfigFile(scratch.Path(), "listen_raft: 0.0.0.0:7001\nraft_self: 10.0.0.7\n");

        auto const reloaded = reloader.Reload();
        REQUIRE_FALSE(reloaded.has_value());
        CHECK(reloaded.error().code == ConfigErrorCode::ImmutableChanged);
        CHECK(reloaded.error().field == "--listen-raft");
        CHECK(reloaded.error().context.contains("not reloadable"));
        // No startup row got there first, which is the whole question.
        CHECK_FALSE(reloaded.error().context.contains("not applied:"));
    }

    SECTION("turning consensus ON is refused for naming no self, not for being immutable")
    {
        auto const path = WriteRunnableNodeConfigFile(scratch.Path(), "log_level: info\n");

        auto const previous = ReparseNodeConfig(path);
        REQUIRE(previous.has_value());
        REQUIRE_FALSE(RunsConsensus(previous.value()));

        auto reloader = MakeNodeReloader(previous.value(), path);
        (void) WriteRunnableNodeConfigFile(scratch.Path(), "log_level: info\nlisten_raft: 0.0.0.0:7000\n");

        auto const reloaded = reloader.Reload();
        REQUIRE_FALSE(reloaded.has_value());
        CHECK(reloaded.error().code == ConfigErrorCode::ParseError);
        CHECK(reloaded.error().context.contains("not applied:"));
        CHECK(reloaded.error().context.contains("turns consensus on and no --raft-peer names this node"));
        // Not the row below it, whose sentence is about a host with no port.
        CHECK_FALSE(reloaded.error().context.contains("no port to pair the host with"));
    }

    SECTION("turning consensus OFF is refused for stranding --raft-self, not for being immutable")
    {
        auto const path = WriteRunnableNodeConfigFile(scratch.Path(), "listen_raft: 0.0.0.0:7000\nraft_self: 10.0.0.7\n");

        auto const previous = ReparseNodeConfig(path);
        REQUIRE(previous.has_value());
        REQUIRE(RunsConsensus(previous.value()));

        auto reloader = MakeNodeReloader(previous.value(), path);
        (void) WriteRunnableNodeConfigFile(scratch.Path(), "raft_self: 10.0.0.7\n");

        auto const reloaded = reloader.Reload();
        REQUIRE_FALSE(reloaded.has_value());
        CHECK(reloaded.error().code == ConfigErrorCode::ParseError);
        CHECK(reloaded.error().context.contains("not applied:"));
        CHECK(reloaded.error().context.contains("no port to pair the host with"));
        // Not the row above it, which is the one that answers the other direction.
        CHECK_FALSE(reloaded.error().context.contains("turns consensus on and no --raft-peer names this node"));
    }
}

#if !defined(_WIN32)

TEST_CASE("A reload re-asks the filesystem about the worker's key files", "[node][config][secret][reload]")
{
    // **#868.** #753 made the DAEMON re-ask at every reload and left the worker with
    // the startup-only version -- on the binary holding FIVE such files rather than
    // one. Only half of #753 is reachable here: none of the worker's secret settings
    // is `Reloadable::Yes`, so a node file cannot GAIN a secret across a reload and
    // `ValidateNodeReloadable` refuses such a candidate by name. The other half is the
    // half no snapshot can answer -- a file's MODE is in no configuration, so an
    // operator who loosens `--cluster-key-file` an hour in produced two byte-identical
    // snapshots and total silence, for a key that MACs discovery proofs and lease
    // grants.
    //
    // Driven against the REAL `ConfigReloaderOf<NodeConfig>` and a real file on disk,
    // through the same `MakeNodeReloader` recipe `main` wires: the two things that
    // must be true are that the subscription is attached at all and that what it asks
    // reaches the filesystem, and a fake reloader would establish neither.
    Testing::ScratchDirectory const scratch { "node-secret-reload" };

    // `key` is what the case is about; the configuration file itself stays 0600 and
    // carries no `token:`, so it can contribute no warning of its own and anything
    // `said` holds came from the key.
    scratch.Write("cluster.key", "not-a-real-key\n");
    auto const key = scratch / "cluster.key";
    REQUIRE(::chmod(key.c_str(), S_IRUSR | S_IWUSR) == 0);

    auto const path = WriteRunnableNodeConfigFile(scratch.Path(), std::format("cluster_key_file: {}\n", key.string()));
    REQUIRE(::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);

    // **Declared before the reloader**, so it is destroyed after it: the report closure
    // lives in the subscriber list and refers to this vector, which is what
    // `WatchSecretExposure` means by "must outlive the reloader" and the same ordering
    // `main` gets by declaring its logger further up.
    std::vector<std::string> said;

    NodeConfig initial = RunningNode();
    // Matching the file, because `cluster_key_file` is `Reloadable::No` -- a seed that
    // disagreed would make the FIRST reload refuse by name, which looks nothing like
    // the case under test.
    initial.clusterKeyFile = key;

    auto reloader = MakeNodeReloader(initial, path);
    WatchSecretExposure<NodeConfig>(
        reloader,
        // `{}` and `false`: no configuration-file secret is in play here, which is
        // asserted by the fact that a case in this file drives that half separately.
        [](NodeConfig const& live) { return NodeSecretFiles(live, {}, false); },
        [&said](std::string_view warning) { said.emplace_back(warning); });

    SECTION("a mode that loosens while the node runs is reported at the next reload")
    {
        REQUIRE(said.empty());

        REQUIRE(::chmod(key.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0);
        REQUIRE(reloader.Reload().has_value());

        REQUIRE(said.size() == 1);
        CHECK(said.front().contains(key.string()));
        CHECK(said.front().contains("chmod o-r"));
        // Not the configuration file: it never moved, and a watcher that reported its
        // whole subject list on any transition would name it too.
        CHECK_FALSE(said.front().contains(path.string()));

        // **Asserted on the COUNT**, because a repeating implementation warns here as
        // well. Said once is what stops this becoming an alarm at every SIGHUP.
        REQUIRE(reloader.Reload().has_value());
        REQUIRE(reloader.Reload().has_value());
        CHECK(said.size() == 1);
    }

    SECTION("a run in which nothing changed says nothing at all")
    {
        // The control, and it is the half that decides whether the section above means
        // anything: without it, "reports a transition" and "reports at every reload"
        // are told apart by nothing, and a rule that warned about the ORDINARY 0600
        // key would fire on every deployment there is.
        REQUIRE(reloader.Reload().has_value());
        REQUIRE(reloader.Reload().has_value());
        CHECK(said.empty());
    }
}

#endif

TEST_CASE("A cache the operator NAMED is refused when no node surface can serve it", "[node-config]")
{
    // #229. The tier is served on the node surface, so with no `--listen-node`
    // there is nothing to reach it through -- and an operator who configured 64 GiB
    // of cache got none of it and one line at boot. The tier does say so now, and
    // names the flags, which is better than the silence originally reported; a
    // refusal is what the ticket asks for, because an Info line hours back in an
    // event log is not an answer.
    //
    // In `StartupPolicyRejection` and not the tier, so `--install-service` refuses
    // it too: a registration bakes the command line in and replays it at every
    // boot, into a log nobody reads.

    SECTION("an explicit --cache-memory with no port is refused, and names both flags")
    {
        auto cfg = Installable();
        cfg.nodeListen = {};
        cfg.cacheMemoryExplicit = true;
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--listen-node"));
        CHECK(Unwrap(refusal).contains("--cache-memory"));
    }

    SECTION("a --cache-dir with no port is refused")
    {
        auto cfg = Installable();
        cfg.nodeListen = {};
        cfg.cacheDir = "/var/lib/fastcache-node/cache";
        REQUIRE(StartupPolicyRejection(cfg).has_value());
    }

    // The other direction, and it is what stops this refusing a correct tree: a node
    // that asked for NO cache still starts without a port. `--cache-memory` has a
    // default, so keying on the value rather than on the explicit bit would refuse
    // every portless node in the tree.
    SECTION("a node that named no cache still starts without a port")
    {
        auto cfg = Installable();
        cfg.nodeListen = {};
        cfg.cacheMemoryExplicit = false;
        cfg.cacheDir.clear();
        auto const refusal = StartupPolicyRejection(cfg);
        INFO("refusal: " << refusal.value_or("<none>"));
        CHECK_FALSE(refusal.has_value());
    }

    // And a configured cache WITH a port is fine, which is the ordinary case.
    //
    // Named for what it asserts. It used to read "is not refused for THIS reason",
    // which is the honest name for a section that tolerates other refusals -- and
    // this one tolerates none: measured, `StartupPolicyRejection` returns nothing at
    // all for this configuration. Keeping the weaker name would leave a reader
    // auditing coverage by section name under-counting it, which is the same defect
    // as #1039's over-counting, pointing the other way.
    SECTION("a configured cache with a port starts")
    {
        auto cfg = Installable();
        cfg.cacheMemoryExplicit = true;
        cfg.cacheDir = "/var/lib/fastcache-node/cache";
        auto const refusal = StartupPolicyRejection(cfg);
        INFO("refusal: " << refusal.value_or("<none>"));
        CHECK_FALSE(refusal.has_value());
    }
}

TEST_CASE("A clustered scheduler needs no --fleet-member", "[node-config]")
{
    // #262. The refusal's premise -- that an empty member set declines everybody --
    // is false on a clustered node, because consensus supplies the set. It was
    // pushing operators toward `--fleet-open`, which admits the entire network: a
    // security downgrade taken to satisfy a startup check.
    //
    // Both directions, because relaxing this must not relax it for a STANDALONE
    // scheduler, which is what the rule was written for and where it is still right.

    SECTION("a clustered scheduler with no member list starts")
    {
        auto cfg = Installable();
        cfg.serveScheduler = true;
        cfg.fleetMembers.clear();
        cfg.fleetOpen = false;
        cfg.raftListen = "6680"; // what RunsConsensus asks about (#1022)
        // A clustered node must name the endpoint its peers dial, and this section is
        // about the MEMBER list rather than about that -- so it is named here rather
        // than left out. Without it the configuration does not start at all: it is
        // refused by `ConsensusNamesNoSelfPeerRefusal`, which is a different rule
        // saying something true. The conditional this section used to carry absorbed
        // exactly that -- `refusal.has_value()` was TRUE, the text did not contain
        // the fleet-member sentence, and the section passed while its name claimed a
        // configuration that this tree refuses (#1039). `--raft-self` and not a
        // `--raft-peer`, because that is the spelling a node whose identity was
        // MINTED has (#1024).
        cfg.raftSelf = "scheduler-01.internal";
        auto const refusal = StartupPolicyRejection(cfg);
        INFO("refusal: " << refusal.value_or("<none>"));
        CHECK_FALSE(refusal.has_value());
    }

    SECTION("a STANDALONE scheduler with no member list is still refused")
    {
        auto cfg = Installable();
        cfg.serveScheduler = true;
        cfg.fleetMembers.clear();
        cfg.fleetOpen = false;
        cfg.raftListen.clear();
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--serve-scheduler needs --fleet-member"));
    }
}

TEST_CASE("An --advertise that clients cannot dial is refused at startup", "[node-config]")
{
    // #208. The flag is `host:port clients should reach this worker on`, and nothing
    // parsed it: `--install-service --advertise=nope` installed cleanly, the worker
    // registered, heartbeated, was leased out, and every client failed to dial it.
    // Silent at BOTH ends -- the same failure the emptiness rule prevents, reached by
    // typing something instead of nothing.
    //
    // Judged by `ParseDialEndpoint`, the one author of "may I dial this?", so the
    // cases below are its three documented refusals rather than a second opinion.

    SECTION("text that is not an endpoint at all")
    {
        auto cfg = Installable();
        cfg.advertise = "nope";
        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--advertise"));
    }

    SECTION("a bare port, which would send the client back to itself")
    {
        auto cfg = Installable();
        cfg.advertise = "6675";
        CHECK(StartupPolicyRejection(cfg).has_value());
    }

    // NOT here: `:6675`. `ParseDialEndpoint` refuses it, but the WILDCARD rule owns
    // that case and says so in better words -- an empty host reaches `getaddrinfo` as
    // nullptr, which is the wildcard. Asserting it here would pass for the wrong
    // reason, and this row's own table warns about exactly that: one predicate
    // widening to cover several passes every case individually and leaves the operator
    // reading about something they did not write. This row is ordered AFTER the
    // wildcard row so the specific message wins.

    SECTION("a port out of range")
    {
        auto cfg = Installable();
        cfg.advertise = "worker.example:70000";
        CHECK(StartupPolicyRejection(cfg).has_value());
    }

    // The other direction, and it is the half that stops this becoming a refusal
    // nobody can satisfy: every shape an operator legitimately types still starts.
    SECTION("well-formed values are accepted")
    {
        for (auto const* good: { "worker.example:6675", "10.0.0.4:6675", "[::1]:6675" })
        {
            auto cfg = Installable();
            cfg.advertise = good;
            INFO("advertise = " << good);
            CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
        }
    }

    // Composes rather than duplicates: an EMPTY value is judged by the rule beside
    // this one, not by this one, so the two cannot disagree about the same flag.
    SECTION("an empty value is left to the emptiness rule")
    {
        // Both halves, because "another rule owns this" is a claim about TWO rules
        // and one of them alone cannot carry it. This used to assert only that IF
        // something was refused it was not this rule -- and nothing is refused on
        // this configuration, so the assertion never ran and a rule that started
        // answering here would not have been noticed (#1039).
        auto cfg = Installable();
        cfg.advertise = {};
        auto const refusal = StartupPolicyRejection(cfg);
        INFO("refusal: " << refusal.value_or("<none>"));
        CHECK_FALSE(refusal.has_value());

        // And the arrangement where the emptiness question DOES bite, which is what
        // makes the sentence above mean something: an empty `--advertise` on a node
        // that admits peers is refused -- by the wildcard rule, in its own words,
        // naming the flags the operator actually typed. Asserting the refusal's text
        // rather than its presence is the point: both rules refuse, and only the
        // words say which one did.
        cfg.fleetMembers = { "peer.example" };
        auto const admitting = StartupPolicyRejection(cfg);
        REQUIRE(admitting.has_value());
        CHECK(Unwrap(admitting).contains("the wildcard resolves to"));
        CHECK_FALSE(Unwrap(admitting).contains("clients can dial"));
    }
}

TEST_CASE("NodeConfig: the addresses this node DIALS are judged for shape, each by its own grammar",
          "[node][policy][dialled-address]")
{
    // [#968](https://github.com/LASTRADA-Software/fastcached/issues/968), the residual
    // #208 left behind. `StartupPolicyRejection`'s address loop walks
    // `NodeSurfaceTable()`, and a surface is a port this process BINDS -- so every
    // address the node asks somebody ELSE about was unjudged except `--advertise`.
    //
    // **The distinguishing assertion is that the rows do NOT share a grammar.** One
    // widened predicate would pass every case below that expects a refusal and then
    // refuse `--fleet-member=worker-01`, which is documented as legal and is what
    // discovery produces. So each direction is asserted for every row.
    auto const base = [] {
        NodeConfig cfg;
        cfg.scheduler = std::string { SchedulerEndpoint };
        cfg.toolchains = { "/usr/bin/g++" };
        return cfg;
    };

    SECTION("a dialled address that is not one is refused, naming the flag and the value")
    {
        auto const [flag, value] = GENERATE(table<std::string, std::string>({
            { "--scheduler", "not an address" },
            { "--scheduler", "6675" },
            { "--upstream", "cache.internal" },
            { "--upstream", ":6674" },
        }));
        INFO(flag << "=" << value);

        auto cfg = base();
        if (flag == "--scheduler")
            cfg.scheduler = value;
        else
            cfg.upstream = value;

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        // The FLAG and the VALUE, because five addresses were typed and a refusal that
        // named neither would leave an operator bisecting a command line. The surface
        // loop above echoes what was written for the same reason.
        CHECK(Unwrap(refusal).contains(flag));
        CHECK(Unwrap(refusal).contains(value));
    }

    SECTION("a bare port is refused for a DIALLED address, which is the decision #208 left open")
    {
        // Not a shape preference: `ParseEndpoint` supplies a default host, which is
        // right for a bind address an operator typed and wrong for text naming
        // somewhere else to ask -- a node told to dial `6675` would ask itself. The
        // same call `--discovery` and `--advertise` already make.
        auto cfg = base();
        cfg.scheduler = "6675";
        REQUIRE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a value nobody typed is not judged for shape")
    {
        // "Parses when GIVEN", never "must parse" -- the surface loop's own rule, and
        // the two questions belong to different rules on purpose. `--upstream` is
        // legitimately empty: a machine with no shared cache gets `NoUpstream`, which
        // is honest rather than broken.
        //
        // `--scheduler` is REQUIRED, by a rule of its own further down, and that is
        // why this section does not clear it. A shape row that also demanded presence
        // would answer "is not an address to dial" for a flag nobody typed, which
        // describes the wrong problem -- and it would make this case pass for a reason
        // that has nothing to do with shape.
        auto cfg = base();
        cfg.upstream.clear();
        cfg.fleetMembers.clear();
        auto const refusal = StartupPolicyRejection(cfg);
        INFO("refusal: " << refusal.value_or("<none>"));
        CHECK_FALSE(refusal.has_value());

        // And the required-ness IS asserted, so the two rules are seen to be separate
        // rather than assumed to be: an empty `--scheduler` is refused, by the other
        // rule and not by this one.
        auto missing = cfg;
        missing.scheduler.clear();
        auto const required = StartupPolicyRejection(missing);
        REQUIRE(required.has_value());
        CHECK(Unwrap(required).contains("--scheduler is required"));
    }

    SECTION("--fleet-member keeps its OWN grammar: a bare host is legal")
    {
        // The case a widened predicate breaks. `--fleet-member` is never dialled: it
        // is matched against a peer's source address through `HostOfEndpoint`, which
        // keeps an unsplittable value whole because a bare host is a legitimate
        // spelling for a peer whose port nobody recorded.
        auto const value = GENERATE(as<std::string> {}, "worker-01", "worker-01.internal:6674", "10.0.0.7");
        INFO("--fleet-member=" << value);

        // Admitting a peer on ANOTHER machine turns on two rules that have nothing to
        // do with `--fleet-member`'s spelling: the three advertise rows, because such
        // a node has told peers to dial it, and the cluster-key rule, because it will
        // have to check the lease signature of a client it did not vouch for. Both are
        // satisfied here rather than worked around -- a fixture that tripped either
        // would report a red for a row this case is not about, which is the trap
        // `SelfScheduler`'s own comment records one fixture up.
        auto cfg = base();
        cfg.nodeListen = "0.0.0.0:6674";
        cfg.nodeListenExplicit = true;
        cfg.advertise = "worker-99.internal:6674";
        cfg.advertiseExplicit = true;
        cfg.clusterKeyFile = "/etc/fastcached/cluster.key";
        cfg.fleetMembers = { value };

        auto const refusal = StartupPolicyRejection(cfg);
        INFO("refusal: " << refusal.value_or("<none>"));
        CHECK_FALSE(refusal.has_value());
    }

    SECTION("an EMPTY --fleet-member element is refused, and that is the silent one")
    {
        // `--fleet-member=` appends `""`. It matches no peer the kernel ever reports,
        // and it makes `fleetMembers` NON-empty -- so `HasMembershipPolicy` answers
        // yes and the "a scheduler with no membership policy" rule below does not
        // fire. The node starts, serves a scheduler, and admits nobody but its own
        // machine, with no error at either end. #208's silent shape, one flag along.
        auto cfg = base();
        cfg.serveScheduler = true;
        cfg.scheduler = std::string { SelfScheduler };
        cfg.clusterKeyFile = "/etc/fastcached/cluster.key";
        cfg.fleetMembers = { "" };

        auto const refusal = StartupPolicyRejection(cfg);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).contains("--fleet-member"));

        // **And it must be refused for ITS OWN reason.** The same configuration with
        // no membership flags at all is refused too -- by the scheduler rule -- so a
        // case asserting only "refused" would pass under the bug. This asserts WHICH.
        auto uncovered = cfg;
        uncovered.fleetMembers.clear();
        auto const other = StartupPolicyRejection(uncovered);
        REQUIRE(other.has_value());
        CHECK_FALSE(Unwrap(other).contains("--fleet-member="));
        CHECK(Unwrap(other) != Unwrap(refusal));
    }
}

TEST_CASE("A disk-only node holds back what its key index costs", "[node][config][capacity]")
{
    // #175: `ResidentCacheBytes` folds tier BUDGETS whose column says the budget is
    // RAM, which is the memory tier alone. A disk tier's budget is filesystem bytes,
    // so it contributes nothing -- correctly, since adding it would be wrong by the
    // ratio between two units -- and its in-memory key index was therefore reserved by
    // nothing at all.
    //
    // A node like that offered the fleet the whole machine while holding an index that
    // runs to hundreds of megabytes at a few million objects. Under-reserving is the
    // direction that over-commits: the jobs come back as refusals the client retries
    // locally, so the build gets slower while distribution looks like it is working.
    NodeConfig cfg;
    FakeHost const box { 8, 32ULL << 30 };

    // A disk tier and no memory tier: the shape the ticket is about.
    Distributed::NodeCacheCapacity diskOnly {};
    diskOnly.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)] = 40ULL << 30;

    auto const without = NodeCapacityOf(cfg, box, diskOnly);
    // The tier's own budget adds nothing to a MEMORY reserve, and that is right.
    CHECK(without.reservedMemoryBytes == 0);

    constexpr std::uint64_t IndexCost = 512ULL << 20;
    auto const with = NodeCapacityOf(cfg, box, diskOnly, IndexCost);

    // **The assertion.** The index is held back, and it is the only thing that changed.
    CHECK(with.reservedMemoryBytes == IndexCost);
    CHECK(with.totalMemoryBytes == without.totalMemoryBytes);
    CHECK(with.logicalCores == without.logicalCores);
}

TEST_CASE("The index reserve is added to the tiers' own budgets, not substituted", "[node][config][capacity]")
{
    // A node running BOTH tiers holds its memory budget AND its disk tier's index, and
    // the two are separately denominated. Asserted because the tempting shape --
    // reserving whichever is larger, or replacing one with the other -- passes any test
    // written about a single-tier node.
    NodeConfig cfg;
    FakeHost const box { 8, 32ULL << 30 };

    Distributed::NodeCacheCapacity both {};
    both.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] = 2ULL << 30;
    both.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)] = 40ULL << 30;

    constexpr std::uint64_t IndexCost = 512ULL << 20;
    auto const capacity = NodeCapacityOf(cfg, box, both, IndexCost);
    CHECK(capacity.reservedMemoryBytes == (2ULL << 30) + IndexCost);
}

TEST_CASE("A reserve larger than the machine is clamped to it", "[node][config][capacity]")
{
    // Both terms are bounded by the machine and their SUM is not, so a pathological
    // configuration could otherwise reserve more RAM than exists and offer negative
    // capacity. Clamped rather than trusted.
    NodeConfig cfg;
    FakeHost const small { 4, 1ULL << 30 };

    Distributed::NodeCacheCapacity mem {};
    mem.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] = 900ULL << 20;

    auto const capacity = NodeCapacityOf(cfg, small, mem, 900ULL << 20);
    CHECK(capacity.reservedMemoryBytes == (1ULL << 30));
}

// ---------------------------------------------------------------------------
// The operator's additions to the compile-argument allowlist (#293). The filter
// itself lives in `CompileJob.cpp` and is tested beside it; what is asked here is
// everything the CONFIGURATION side owes it -- the shape a spelling must have, that
// a file and a command line reach the same field through the same applier, that the
// row is classified as local rather than advertised wiring, and what gets said.

TEST_CASE("The allowlist flag is repeatable and reaches the same field from a file", "[node][config]")
{
    // The list-flag contract every repeatable row here has: a file APPENDS, and
    // naming any on the command line EMPTIES the file's list first rather than
    // extending it. Asserted for this row rather than assumed from the column,
    // because a `clear` left off is a flag that silently means the opposite.
    auto const fromArgv =
        ParseNodeArgv({ "--allow-compile-arg=-fno-semantic-interposition", "--allow-compile-arg=/Qspectre" });
    REQUIRE(fromArgv.has_value());
    CHECK(fromArgv->extraAllowedArgs == std::vector<std::string> { "-fno-semantic-interposition", "/Qspectre" });

    auto const fromFile = FromFileAndArgv({ Setting("allow_compile_arg", { "-fno-plt", "-mno-outline-atomics" }) }, {});
    REQUIRE(fromFile.has_value());
    CHECK(fromFile->extraAllowedArgs == std::vector<std::string> { "-fno-plt", "-mno-outline-atomics" });

    auto const both = FromFileAndArgv({ Setting("allow_compile_arg", { "-fno-plt" }) },
                                      { "--allow-compile-arg=-fno-semantic-interposition" });
    REQUIRE(both.has_value());
    CHECK(both->extraAllowedArgs == std::vector<std::string> { "-fno-semantic-interposition" });
}

TEST_CASE("An allowed compile argument is refused unless it can be one", "[node][config]")
{
    // Shape only. The point of the flag is to name a spelling no table here knows, so
    // there is nothing to check membership against -- which leaves exactly the
    // properties that make a spelling a spelling at all.
    SECTION("a bare word is an input FILE on a compiler command line")
    {
        // The one refusal with teeth: without it an operator could allow a path, which
        // is what the whole filter exists to deny.
        auto const refused = ParseNodeArgv({ "--allow-compile-arg=evil.cpp" });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().context.contains("evil.cpp"));
    }

    SECTION("a pattern is refused rather than silently matching nothing")
    {
        // Matching is whole and exact, so `-f*` would never match anything. Refusing
        // it is the difference between an operator being told and an operator having a
        // configuration line that does nothing for as long as it is there.
        auto const pattern = GENERATE(as<std::string> {}, "-f*", "-X?", "/Q*");
        INFO("pattern " << pattern);
        auto const flag = std::string { "--allow-compile-arg=" } + pattern;
        CHECK_FALSE(ParseNodeArgv({ flag.c_str() }).has_value());
    }

    SECTION("whitespace means it is more than one argument")
    {
        auto const refused = ParseNodeArgv({ "--allow-compile-arg=-wrapper env" });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().context.contains("whitespace"));
    }

    SECTION("empty names nothing")
    {
        CHECK_FALSE(ParseNodeArgv({ "--allow-compile-arg=" }).has_value());
    }

    SECTION("a path names a file on the WORKER")
    {
        // A dispatched compile arrives preprocessed, so nothing it names can be part of
        // the client's build -- what a path would reach is this machine. Refused here
        // AND again when the argument arrives, which is two doors; this is the one that
        // can say why while the operator is looking at their own file.
        auto const flag = GENERATE(as<std::string> {},
                                   "--allow-compile-arg=-I/usr/include",
                                   "--allow-compile-arg=-fplugin=/tmp/evil.so",
                                   "--allow-compile-arg=/FoC:\\out.obj",
                                   "--allow-compile-arg=--sysroot=/");
        INFO(flag);
        auto const refused = ParseNodeArgv({ flag.c_str() });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().context.contains("path"));

        // And the leading introducer is not a separator, or every MSVC option would be
        // refused as a path. This is the control that says the rule reads the VALUE.
        CHECK(ParseNodeArgv({ "--allow-compile-arg=/Qvec-report:2" }).has_value());
    }

    SECTION("and the shapes that ARE spellings are accepted")
    {
        // The other direction, because a rule that refuses everything passes every
        // case above. Both introducers, a value, and a trailing `-` -- all ordinary.
        auto const good = GENERATE(as<std::string> {}, "-fno-plt", "/Qspectre", "-mtune=generic", "/permissive-");
        INFO("spelling " << good);
        auto const flag = std::string { "--allow-compile-arg=" } + good;
        CHECK(ParseNodeArgv({ flag.c_str() }).has_value());
    }
}

TEST_CASE("The allowlist row is reloadable, and LOCAL rather than advertised", "[node][config][reload]")
{
    // Which of the two lists a `Reloadable::Yes` row is on decides whether a change to
    // it re-registers this worker, and that re-derivation is an include-tree walk
    // measured over 300 s cold. A registration says which TOOLCHAINS this worker
    // serves and has no field for which arguments it will accept, so re-registering
    // would spend those minutes telling the fleet nothing it can act on.
    auto const rows = NodeOptions();
    auto const row = std::ranges::find_if(rows, [](auto const& spec) { return spec.primary == "--allow-compile-arg"; });
    REQUIRE(row != rows.end());
    CHECK(row->reloadable == Reloadable::Yes);
    CHECK(std::ranges::contains(LocalReloadableFlags, std::string_view { "--allow-compile-arg" }));
    CHECK_FALSE(std::ranges::contains(AdvertisedReloadableFlags, std::string_view { "--allow-compile-arg" }));

    // And what those lists MEAN, asked of the functions that read them rather than of
    // the lists again: a reload that changes only this is accepted, and it does not
    // move what this worker advertises.
    NodeConfig previous;
    previous.scheduler = std::string { SchedulerEndpoint };
    auto candidate = previous;
    candidate.extraAllowedArgs = { "-fno-plt" };

    CHECK(ValidateNodeReloadable(previous, candidate).has_value());
    CHECK_FALSE(AdvertisedClaimsDiffer(previous, candidate));
}

TEST_CASE("AllowlistAnnouncement says what changed, and stays quiet when nothing did", "[node][config][reload]")
{
    // #293's third acceptance clause -- *each extension is logged* -- as a pure
    // function, because `main.cpp` is in no test target and a rule written there could
    // only be checked by reading it. The failure this guards is silent in the
    // direction nobody notices: a widening nobody was told about reads exactly like a
    // worker nobody widened.
    using Args = std::vector<std::string>;

    SECTION("a worker that was not widened says nothing at startup")
    {
        // The shipped state, on every start of every node. A line here is how the line
        // that matters gets filtered out.
        CHECK_FALSE(AllowlistAnnouncement(AllowlistMoment::Startup, {}, Args {}).has_value());
    }

    SECTION("a widened one names the count and every entry")
    {
        auto const said = AllowlistAnnouncement(AllowlistMoment::Startup, {}, Args { "-fno-plt", "/Qspectre" });
        REQUIRE(said.has_value());
        // Every entry, not a count: an operator reading a log after an incident needs
        // to know WHAT was allowed, and "2 entries" answers a different question.
        CHECK(Unwrap(said).contains("-fno-plt"));
        CHECK(Unwrap(said).contains("/Qspectre"));
        CHECK(Unwrap(said).contains("2"));
    }

    SECTION("a reload that did not move the list is silent")
    {
        // A reload is routine -- `--log-level` alone triggers one -- so narrating a
        // set nobody touched at WARN would train an operator to skip the line.
        Args const same { "-fno-plt" };
        CHECK_FALSE(AllowlistAnnouncement(AllowlistMoment::Reload, same, same).has_value());
        CHECK_FALSE(AllowlistAnnouncement(AllowlistMoment::Reload, Args {}, Args {}).has_value());
    }

    SECTION("a reload that moved it says so, emptying included")
    {
        auto const added = AllowlistAnnouncement(AllowlistMoment::Reload, Args {}, Args { "-fno-plt" });
        REQUIRE(added.has_value());
        CHECK(Unwrap(added).contains("-fno-plt"));

        // The half a silent implementation drops, because it looks like good news: an
        // operator who removed the last entry needs to see that it took effect as much
        // as one who added the first. And it must not render as a sentence ending in
        // nothing, which reads as a truncated message rather than as a set with no
        // members.
        auto const emptied = AllowlistAnnouncement(AllowlistMoment::Reload, Args { "-fno-plt" }, Args {});
        REQUIRE(emptied.has_value());
        CHECK(Unwrap(emptied).contains("(none)"));
        CHECK_FALSE(Unwrap(emptied).contains("-fno-plt"));

        // A reorder is a change: the file was edited, and an operator watching a
        // reload they asked for is owed the answer either way.
        auto const reordered = AllowlistAnnouncement(AllowlistMoment::Reload, Args { "-a1", "-b2" }, Args { "-b2", "-a1" });
        CHECK(reordered.has_value());
    }
}

TEST_CASE("ObservabilityAnnouncement tells a fleet node it opens no admin surface, and tells nobody else",
          "[node][config][observability]")
{
    // #1304. `--admin-listen` is off unless asked for, so a worker in a fleet runs with
    // no `/healthz`, no `/metrics` and no dashboard while everything else works: opt-in
    // monitoring whose failure mode is silence. The remark is the whole fix -- the
    // default is deliberately NOT flipped -- and it is a pure function because
    // `main.cpp` is in no test target (#909).

    // **Every case names `--scheduler`, and that is the point.** It is required of
    // every startable node, a pure `--serve-scheduler` included, so it is no evidence
    // of a fleet -- a predicate reading it would fire on every node there is, which is
    // exactly the single-machine install this remark has to stay off.
    auto const base = [] {
        NodeConfig cfg;
        cfg.scheduler = std::string { SchedulerEndpoint };
        return cfg;
    };

    SECTION("a single-machine install says nothing")
    {
        CHECK_FALSE(ObservabilityAnnouncement(base()).has_value());
    }

    SECTION("a loopback-only member list is still one machine")
    {
        // This is what picks `AdmitsRemotePeers` out of the three spellings of *is this
        // a fleet participant* that deliberately disagree: the reachability rows' gate
        // answers YES here, because `--fleet-member` was given at all.
        auto cfg = base();
        cfg.fleetMembers = { "127.0.0.1" };
        CHECK_FALSE(ObservabilityAnnouncement(cfg).has_value());
    }

    SECTION("a member named `localhost` is told, because nothing here may trust a name")
    {
        // **Not a hole, and not an oversight either way round.** `IsLoopbackHost`
        // deliberately does not call `localhost` loopback -- it answers security
        // questions, where a name a resolver decides must not be trusted -- so
        // `AdmitsRemotePeers` reads such a member as another machine and the remark
        // fires. That is the safe direction for a REMARK: the worst it costs a
        // one-machine install spelled this way is one line it did not need, where the
        // other reading would silence a real fleet whose member list happens to be
        // written with names. Asserted rather than left to be discovered, because it
        // is the one input where this case and the one above disagree.
        auto cfg = base();
        cfg.fleetMembers = { "localhost" };
        CHECK(ObservabilityAnnouncement(cfg).has_value());
    }

    SECTION("a node admitting another machine is told, and told what to type")
    {
        auto cfg = base();
        cfg.fleetMembers = { "10.0.0.2" };
        auto const said = ObservabilityAnnouncement(cfg);
        REQUIRE(said.has_value());

        // What is unavailable, all three of them, and the flag that provides it: the
        // whole of the ticket's acceptance clause.
        CHECK(Unwrap(said).contains("/healthz"));
        CHECK(Unwrap(said).contains("/metrics"));
        CHECK(Unwrap(said).contains("dashboard"));
        CHECK(Unwrap(said).contains("--admin-listen"));
        // The REMEDY, not just the flag's name. Dropping `--admin-listen=<port> opens
        // it` while keeping the sentence that explains what the flag serves left this
        // case GREEN -- measured -- because the name still appeared in the explanation.
        // The half an operator acts on is the half that has to be pinned.
        CHECK(Unwrap(said).contains("--admin-listen=<port>"));

        // And that the node is FINE. A remark read as a fault sends an operator to
        // inspect a healthy worker, which is worse than having said nothing.
        CHECK(Unwrap(said).contains("configured rather than broken"));
    }

    SECTION("--fleet-open and a consensus join reach it too")
    {
        // Three routes into `AdmitsRemotePeers` with no member list between them, so a
        // predicate written against `--fleet-member` alone passes the case above and
        // fails both of these.
        auto open = base();
        open.fleetOpen = true;
        CHECK(ObservabilityAnnouncement(open).has_value());

        auto joining = base();
        joining.raftJoin = true;
        CHECK(ObservabilityAnnouncement(joining).has_value());
    }

    SECTION("the same node with an admin surface says nothing")
    {
        // Asked of the surface ROW, so all three spellings of a served admin port count
        // -- a bare port, which binds loopback, exactly as an address does.
        auto cfg = base();
        cfg.fleetOpen = true;

        // `CAPTURE` rather than three spelled-out assertions: the loop is the same
        // three cases, and without the capture a failure would name the line instead
        // of the spelling, which is the one thing the separate statements gave away.
        for (auto const* const listen: { "9100", "127.0.0.1:9100", "0.0.0.0:9100" })
        {
            cfg.adminListen = listen;
            CAPTURE(listen);
            CHECK_FALSE(ObservabilityAnnouncement(cfg).has_value());
        }
    }
}

TEST_CASE("The worker parses --seed-config, and no file may carry it", "[node][config]")
{
    // #397. The verb exists on the WORKER rather than being `fastcached --seed-config`
    // pointed at a different path, because that action derives its destination from
    // `DaemonApplicationName` and can therefore only ever write the daemon's file.
    // That is why the MSI shipped no worker configuration at all: there was no second
    // action to add, only a first one that could not be reused.
    auto const parsed = ParseNodeArgv({ "--seed-config=/opt/etc/fastcache-compile-node.yaml.default" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->seedConfigTemplate == "/opt/etc/fastcache-compile-node.yaml.default");

    // Empty means the verb was not asked for, which is what makes an ordinary start
    // distinguishable from a seeding run without a second flag to keep in step.
    CHECK(ParseNodeArgv({}).value().seedConfigTemplate.empty());

    // A one-shot verb may carry no yamlKey: a file is read at every start, so a key
    // for this one would re-seed at every start -- an installer step replayed forever.
    // The compile-time guard beside the table proves it for the build; this states it
    // where somebody reading the tests will find it, and pins that the row is on the
    // exclusion list with a REASON rather than merely absent from the key column.
    auto const rows = NodeOptions();
    auto const row = std::ranges::find_if(rows, [](auto const& spec) { return spec.primary == "--seed-config"; });
    REQUIRE(row != rows.end());
    CHECK(row->yamlKey.empty());
    CHECK(row->reloadable == Reloadable::No);
}

TEST_CASE("No node flag can override how long a compile may take", "[node][config][lease]")
{
    // **The acceptance clause of #522 that is an ABSENCE, so it is a scan and the scan
    // needs a control.** #243 originally asked for a node-side flag letting a site with
    // long translation units raise the worker's per-request bound. That is the wrong
    // lever and putting it back would be worse than never having had it: a worker
    // serving past the grant produces an object for a key the scheduler has already
    // reclaimed and may have re-granted, so the fleet does the work twice and one of the
    // two results is thrown away -- with every counter normal. The lever is the
    // replicated `lease-lifetime`, and there is deliberately no local one.
    //
    // Asked of the option TABLE rather than of the source text, because the table is
    // what parsing and `--help` are both driven by: a flag that is not a row cannot be
    // typed, cannot come from a configuration file, and cannot ride into a service
    // registration. A grep over `NodeConfig.cpp` would answer about prose.
    auto const rows = NodeOptions();

    // **The positive control, and it is not decoration.** A scan that reports an absence
    // is worth nothing until it has been seen to find a presence: an empty span, a
    // renamed accessor or a predicate that never matches all read as "no override
    // exists", which is the one answer this case must not be able to give for the wrong
    // reason. So it first finds rows it must find.
    REQUIRE_FALSE(rows.empty());
    auto const names = [&rows](std::string_view needle) {
        return std::ranges::any_of(rows, [needle](auto const& row) { return row.primary == needle; });
    };
    REQUIRE(names("--slots"));       // a real row, and the one the rulebook names as
    REQUIRE(names("--listen-node")); // per-machine rather than replicated
    // And that the predicate below can say yes about a TIMEOUT-shaped needle, which is
    // the only kind it is asked about. `--drain-timeout` is a real row that legitimately
    // carries the word, so a `contains` that had stopped matching -- a renamed accessor,
    // a `primary` that is now empty -- fails here rather than reporting every absence
    // below as satisfied. A needle every row contains (`"--"`) would prove nothing about
    // the substrings that matter.
    REQUIRE(names("--drain-timeout"));
    REQUIRE(std::ranges::any_of(rows, [](auto const& row) { return row.primary.contains("timeout"); }));

    // Now the absence. Any row whose flag names a compile or lease duration would be a
    // local override of a fleet-wide agreement.
    for (auto const& row: rows)
    {
        INFO("row: " << row.primary);
        CHECK_FALSE(row.primary.contains("lease-timeout"));
        CHECK_FALSE(row.primary.contains("lease-lifetime"));
        CHECK_FALSE(row.primary.contains("compile-timeout"));
        CHECK_FALSE(row.primary.contains("request-timeout"));
        CHECK_FALSE(row.primary.contains("job-timeout"));
    }
}

TEST_CASE("The compression settings parse, and each names its own flag", "[node][config][compression]")
{
    // `none` is the only codec guaranteed to exist: lz4 and zstd are behind
    // FASTCACHED_ENABLE_COMPRESSION, so a build without them must still parse the
    // flag rather than refuse a value the table documents.
    auto const off = ParseNodeArgv({ "--scheduler=s:1", "--memory-compression=none", "--compression=none" });
    REQUIRE(off.has_value());
    CHECK(off->memoryCompression == CompressionCodec::Identity);
    CHECK(off->compression == CompressionCodec::Identity);

    for (auto const codec: { CompressionCodec::Lz4, CompressionCodec::Zstd })
    {
        if (!Compression::IsAvailable(codec))
            continue;
        auto const named = std::format("--memory-compression={}", Compression::NameOf(codec));
        auto const parsed = ParseNodeArgv({ "--scheduler=s:1", named.c_str() });
        REQUIRE(parsed.has_value());
        CHECK(parsed->memoryCompression == codec);
    }

    // The refusal names the flag that was typed. It matters that this is the
    // MEMORY one: the parser is shared with `--compression`, and the daemon's
    // private copy of it hard-coded `compression` as the field, which sent an
    // operator to a flag they had not written.
    auto const bad = ParseNodeArgv({ "--scheduler=s:1", "--memory-compression=brotli" });
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ConfigErrorCode::OutOfRange);
    CHECK(bad.error().field == "--memory-compression");
}

TEST_CASE("A compression setting that reaches no tier refuses the node", "[node][config][compression]")
{
    // A flag that configures a half this node does not build is inert, and inert is
    // invisible: the startup line drops the codec along with the half, so the one
    // surface that reports a codec says nothing at all about the one that was typed.
    // Same shape as `--dashboard-token-file` without `--dashboard`, and refused the
    // same way.
    auto startable = [] {
        auto cfg = Installable();
        cfg.cacheMemoryBytes = 8ULL * 1024 * 1024;
        return cfg;
    };

    SECTION("the memory trio with --cache-memory=0")
    {
        // Each of the three separately, because a rule written as one `||` chain and
        // a rule that reads only the codec are the same test otherwise -- and the
        // level and min-bytes flags are the ones a value comparison could never see,
        // their defaults being 3 and 4096 rather than anything falsy.
        for (auto const& [name, bit]: std::vector<std::pair<std::string_view, bool NodeConfig::*>> {
                 { "--memory-compression", &NodeConfig::memoryCompressionExplicit },
                 { "--memory-compression-level", &NodeConfig::memoryCompressionLevelExplicit },
                 { "--memory-compression-min-bytes", &NodeConfig::memoryCompressionMinBytesExplicit } })
        {
            INFO("flag: " << name);
            auto cfg = startable();
            cfg.cacheMemoryBytes = 0;
            cfg.*bit = true;

            auto const refusal = StartupPolicyRejection(cfg);
            REQUIRE(refusal.has_value());
            CHECK(Unwrap(refusal).contains("--cache-memory"));
        }
    }

    SECTION("the disk trio with no --cache-dir")
    {
        for (auto const& [name, bit]: std::vector<std::pair<std::string_view, bool NodeConfig::*>> {
                 { "--compression", &NodeConfig::compressionExplicit },
                 { "--compression-level", &NodeConfig::compressionLevelExplicit },
                 { "--compression-min-bytes", &NodeConfig::compressionMinBytesExplicit } })
        {
            INFO("flag: " << name);
            auto cfg = startable();
            cfg.cacheDir.clear();
            cfg.*bit = true;

            auto const refusal = StartupPolicyRejection(cfg);
            REQUIRE(refusal.has_value());
            CHECK(Unwrap(refusal).contains("--cache-dir"));
        }
    }

    SECTION("the same flags with the half they configure ARE accepted")
    {
        // The control. Without it "a named codec with no tier is refused" and "a
        // named codec is refused" are one passing test, and the second would refuse
        // every node that compresses anything.
        Testing::ScratchDirectory const scratch { "node-compression-reaches-a-tier" };

        auto cfg = startable();
        cfg.cacheDir = scratch.Path();
        cfg.memoryCompressionExplicit = true;
        cfg.memoryCompressionLevelExplicit = true;
        cfg.memoryCompressionMinBytesExplicit = true;
        cfg.compressionExplicit = true;
        cfg.compressionLevelExplicit = true;
        cfg.compressionMinBytesExplicit = true;

        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
    }

    SECTION("a DEFAULT nobody typed starts a node that asked for nothing")
    {
        // The other control, and the one the rule would be wrong without: all six
        // settings carry a default, the disk codec's being `zstd`, so a rule asking
        // what the VALUE is rather than whether an operator NAMED it would refuse the
        // ordinary memory-only worker at every boot.
        auto cfg = startable();
        cfg.cacheDir.clear();
        REQUIRE(cfg.compression == CompressionCodec::Zstd);
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());

        auto memoryOff = startable();
        memoryOff.cacheMemoryBytes = 0;
        memoryOff.cacheDir.clear();
        CHECK_FALSE(StartupPolicyRejection(memoryOff).has_value());
    }
}

TEST_CASE("The compression level is range-checked at both ends", "[node][config][compression]")
{
    for (auto const* good: { "--compression-level=1", "--compression-level=22" })
    {
        auto const parsed = ParseNodeArgv({ "--scheduler=s:1", good });
        INFO("flag: " << good);
        REQUIRE(parsed.has_value());
    }
    // Both ends, because a guard written as `> 22` alone accepts 0 and a guard
    // written as `== 0` alone accepts 23.
    for (auto const* bad: { "--compression-level=0", "--compression-level=23", "--memory-compression-level=0" })
    {
        auto const parsed = ParseNodeArgv({ "--scheduler=s:1", bad });
        INFO("flag: " << bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == ConfigErrorCode::OutOfRange);
    }
    auto const level = ParseNodeArgv({ "--scheduler=s:1", "--memory-compression-level=9" });
    REQUIRE(level.has_value());
    CHECK(level->memoryCompressionLevel == 9);
}

TEST_CASE("The compression floors take byte suffixes", "[node][config][compression]")
{
    auto const parsed =
        ParseNodeArgv({ "--scheduler=s:1", "--memory-compression-min-bytes=8k", "--compression-min-bytes=512" });
    REQUIRE(parsed.has_value());
    CHECK(parsed->memoryCompressionMinBytes == 8192);
    CHECK(parsed->compressionMinBytes == 512);

    // A disk budget as a fraction of RAM is meaningless, and so is a compression
    // floor: no host total is passed to the parser, so `%` is refused rather than
    // silently resolved against this machine.
    auto const percent = ParseNodeArgv({ "--scheduler=s:1", "--memory-compression-min-bytes=10%" });
    CHECK_FALSE(percent.has_value());
}

TEST_CASE("The compression defaults leave every existing node unchanged", "[node][config][compression]")
{
    // The whole safety argument for this feature in one case. The disk tier has
    // always compressed with zstd level 3 above 256 bytes, and the memory tier has
    // never compressed at all -- so these are the values that make an upgrade a
    // no-op for somebody who names none of the six flags.
    NodeConfig const defaults;
    CHECK(defaults.compression == CompressionCodec::Zstd);
    CHECK(defaults.compressionLevel == 3);
    CHECK(defaults.compressionMinBytes == 256);
    CHECK(defaults.memoryCompression == CompressionCodec::Identity);
    CHECK(defaults.memoryCompressionLevel == 3);
    CHECK(defaults.memoryCompressionMinBytes == 4096);

    // And they match what the storage layer would have used on its own, which is
    // the property that actually holds the claim up: a default stated here that
    // disagreed with `CowTreeStorage::Options` would change behaviour silently.
    CowTreeStorage::Options const storeDefaults;
    CHECK(defaults.compression == storeDefaults.compression);
    CHECK(defaults.compressionLevel == storeDefaults.compressionLevel);
    CHECK(defaults.compressionMinBytes == storeDefaults.compressionMinBytes);

    InMemoryLruStorage::CompressionOptions const memoryDefaults;
    CHECK(defaults.memoryCompression == memoryDefaults.codec);
    CHECK(defaults.memoryCompressionLevel == memoryDefaults.level);
    CHECK(defaults.memoryCompressionMinBytes == memoryDefaults.minBytes);
}

TEST_CASE("A registration carries the codec by name, not by number", "[node][config][compression][service]")
{
    // `emitIfExplicit` formats its value, and a CompressionCodec is a byte-wide
    // enum -- so a registration built without `NameOf` would bake in `--memory-
    // compression=2`, which the next start refuses. A service that installs and
    // then fails at every boot, with nobody watching.
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        SKIP("this build has no zstd, so there is no non-default codec to pin");

    auto parsed = ParseNodeArgv({ "--scheduler=s:1", "--toolchain=/usr/bin/g++", "--memory-compression=zstd" });
    REQUIRE(parsed.has_value());

    auto const spec = MakeNodeServiceSpec("/usr/bin/fastcache-compile-node", *parsed);
    CHECK(std::ranges::any_of(spec.arguments, [](std::string const& arg) { return arg == "--memory-compression=zstd"; }));
}

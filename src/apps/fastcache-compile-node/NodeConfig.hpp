// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Node
{

/// Everything this worker was told to be.
///
/// In a header rather than `main.cpp`'s anonymous namespace so that the things
/// derived FROM it can be tested. `main.cpp` is in no test target -- the lesson
/// `CacheProtocol.cpp`, `RootReconciler.cpp` and `AdminEndpoint.cpp` were each
/// extracted for -- and what is derived here is a *service registration*, where
/// a value that cannot survive its own parser produces a worker that registers
/// cleanly and then never starts again.
struct NodeConfig
{
    std::string scheduler; ///< host:port of the scheduler's dispatch endpoint.
    std::string advertise; ///< host:port clients should reach this worker on.
    std::string bindAddress { "0.0.0.0" };
    std::uint16_t port { 6676 };

    /// fingerprint=compilerPath, repeatable. A worker with none serves nothing,
    /// which is deliberate: there is no default compiler, because a default is how
    /// a job ends up running against something nobody chose.
    std::vector<std::string> toolchains;
    /// Concurrent compiles, or 0 to size the machine from `nodeClass` and its
    /// hardware. Enforced here as well as advertised, through the one shared
    /// `Distributed::OfferableSlots`: two implementations of that arithmetic is how
    /// a worker comes to accept more jobs than the scheduler believes it has.
    std::uint32_t slots { 0 };

    /// Cores held back from the fleet, when the operator named a number.
    ///
    /// Absent is not zero, and this is one of the places that distinction is the
    /// whole point: absent means "reserve whatever this node class reserves", while
    /// a zero the operator typed means "reserve nothing, drive this machine to its
    /// last core". A plain `std::uint32_t` cannot carry the difference, so a
    /// workstation whose operator wanted no reserve and one who simply did not
    /// mention it would be indistinguishable — and one of the two answers is
    /// somebody's desktop becoming unusable.
    std::optional<std::uint32_t> reservedCores;

    /// How hard this machine may be driven. See `Distributed::NodeClass`.
    ///
    /// Defaults to `Workstation`, which is the safe answer rather than the common
    /// one: a node whose class nobody set is somebody's desktop until proven
    /// otherwise, and getting that backwards is a failure the person experiences as
    /// "my editor stutters" and never connects to a build fleet.
    Distributed::NodeClass nodeClass { Distributed::NodeClass::Workstation };

    /// Where the admin endpoint listens, or empty to leave it off.
    ///
    /// One string rather than the daemon's address/port/enabled triple, because a
    /// worker has one reason to want this and an empty value is already the "off"
    /// it would otherwise need a flag for. Defaults to off, and to loopback when a
    /// bare port is given: a scrape endpoint reachable from the network is the
    /// operator's decision, not this program's.
    std::string adminListen;

    /// Where this node serves the fleet's scheduler verbs, or empty to leave it off.
    ///
    /// Off by default and for the same reason `--admin-listen` is: handing out other
    /// machines' CPU time is an operator's decision, not something they get by
    /// starting a worker. Unlike the admin endpoint, a bare port binds the WILDCARD
    /// rather than loopback -- a scheduler no peer can dial is a scheduler that does
    /// nothing, so loopback would be a default that silently cannot work, which is
    /// exactly the shape `--advertise` is refused for.
    std::string schedulerListen;

    /// Peers this node will schedule work onto, as `host:port`; repeatable.
    ///
    /// Only the host part is used -- a peer connecting to the scheduler comes from an
    /// ephemeral source port, so an endpoint is not something a connection can be
    /// matched against. The endpoint form is accepted because it is what discovery
    /// produces and what an operator has written down.
    std::vector<std::string> fleetMembers;

    /// Where this node keeps its own cache tier, or empty for memory only.
    ///
    /// The tier exists so a local rebuild on a slow or bad network does not reach
    /// the wire at all. Memory-only is a legitimate configuration and is the
    /// default: a disk tier is a resource an operator should have to name, and an
    /// in-memory one already removes the round trip for a working set that fits.
    std::filesystem::path cacheDir;

    /// Bytes the in-memory half of the tier may hold. 0 means "no local cache".
    ///
    /// Non-zero by default, unlike almost everything else here, because a local tier
    /// is what this program is *for*: the whole reason a developer runs a node rather
    /// than pointing at the shared cache is that a rebuild should not reach the wire.
    /// A default of zero would mean nobody got that unless they read this file.
    ///
    /// 256 MiB rather than a fraction of RAM: this runs on somebody's workstation
    /// beside their editor and their browser, and the daemon's quarter-of-the-machine
    /// default is sized for a host whose job is caching. Enough for a few thousand
    /// objects, small enough that nobody notices it.
    std::uint64_t cacheMemoryBytes { 256ULL * 1024ULL * 1024ULL };

    /// Where this node serves cache verbs to its local clients; empty turns it off.
    ///
    /// Defaults to the address `fastcache-cc` already looks at when nobody sets
    /// `FASTCACHE_ADDR`, which is what makes the local tier work with **no
    /// configuration at all**: start a node, build, and the launcher finds it. The
    /// alternative — an off-by-default port an operator has to discover, and a
    /// `FASTCACHE_ADDR` they then have to point at it — is two steps to get the
    /// behaviour that is the point of running the program.
    ///
    /// Loopback, unlike `--listen-scheduler`'s wildcard, and that asymmetry is the
    /// anti-leeching rule rather than a preference: a scheduler no peer can dial does
    /// nothing, while a cache any host can dial is this machine's entire build output
    /// served to strangers. Widening it is an operator's decision, and even then
    /// `CacheResponder` admits only this machine and this cluster's members.
    ///
    /// A port already taken is fatal when the operator **named** it and a warning when
    /// it is this default -- the same distinction the admin endpoint draws between an
    /// endpoint asked for and one got anyway. Typed, it is a promise, and a broken
    /// promise is fatal; defaulted, a node sharing a machine with `fastcached` would
    /// otherwise refuse to start over a convenience nobody requested, and the launcher
    /// reaches the daemon on that port instead. Never silently, either way.
    std::string cacheListen { "127.0.0.1:6674" };

    /// The shared `fastcached` this node reads through to, or empty for none.
    ///
    /// Empty is honest rather than broken -- one developer's machine has no shared
    /// cache, and `NoUpstream` is what that configuration gets.
    std::string upstream;

    std::string token;
    std::string user;
    LogLevel logLevel { LogLevel::Info };

    /// The name the platform's supervisor keys this worker's registration on.
    ///
    /// Distinct from the daemon's `FastCached` by default, because the two are
    /// separate services that a machine may well run both of -- sharing a name
    /// would make installing one silently displace the other.
    std::string serviceName { "FastCacheCompileNode" };

    /// Which supervisor domain `--install-service` registers into.
    ServiceScope serviceScope { ServiceScope::System };

    /// Where a POSIX daemonized run writes its pid, empty for none.
    std::string pidfile;

    /// Admit every caller to the fleet, rather than only `--fleet-member` hosts.
    ///
    /// The right answer for one machine, or a fleet whose network reachability is
    /// already its boundary. It is a *flag* rather than the behaviour you get by
    /// listing no members, because "no policy" and "a policy that admits everybody"
    /// have to be the same explicit decision -- listing nobody refuses everybody, and
    /// a scheduler that quietly served strangers would look identical to a healthy one
    /// from both ends.
    bool fleetOpen { false };
    bool daemon { false };           ///< Fork into the background / run under the SCM.
    bool installService { false };   ///< Register with the platform's supervisor and exit.
    bool uninstallService { false }; ///< Remove that registration and exit.
    bool help { false };
    bool version { false };
};

/// What this machine offers the fleet, from its configuration and its hardware.
///
/// Extracted from `main.cpp` — which is in no test target — for the reason
/// `CacheProtocol.cpp`, `RootReconciler.cpp` and `AdminEndpoint.cpp` were each
/// extracted: the rule it applies is worth checking. What it decides is which facts
/// come from the operator (`--node-class`, `--reserve-cores`) and which from the
/// machine (cores, memory), and getting that mapping wrong is invisible — the node
/// registers, heartbeats and is simply sized wrong forever.
///
/// The machine arrives through `IHostFactsSource` rather than through
/// `OnlineCpuCount()` and friends, so a two-core laptop and a 128-thread server are
/// both assertable from one test run.
/// @param cfg The parsed configuration.
/// @param host What the machine says about itself.
/// @return The capacity to advertise, and to size this worker's own limit by.
[[nodiscard]] Distributed::NodeCapacity NodeCapacityOf(NodeConfig const& cfg, IHostFactsSource const& host);

/// Every accepted option, one row each.
///
/// The same table idiom the daemon and the launcher use, so an accepted spelling
/// is necessarily a documented one and adding a flag is adding a row.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept;

/// Describe this worker as a service to register.
///
/// The worker's half of the `ServiceSpec` seam, mirroring the daemon's
/// `MakeDaemonServiceSpec`. Hand-written for the reason that one is: an
/// `OptionSpec` says how to PARSE a flag and carries no way to read a value back
/// out, so "emit every field that differs from its default" cannot be written
/// once generically. `NodeConfig_test` walks `NodeOptions()` and requires every
/// non-excluded row to be emitted, which is what keeps this from drifting.
///
/// `--requirepass` is never emitted, for the reason it is never emitted for the
/// daemon: a supervisor records its launch arguments where every local account
/// can read them.
/// @param exePath Absolute path to the fastcache-compile-node executable.
/// @param cfg Effective configuration to embed in the launch arguments.
/// @return The spec a supervisor is registered from.
[[nodiscard]] ServiceSpec MakeNodeServiceSpec(std::filesystem::path const& exePath, NodeConfig const& cfg);

/// Why @p cfg must not be registered as a service, if it must not.
///
/// Install-time rules that are the WORKER's rather than the platform's, checked
/// alongside `ServiceRegistrationRejection`. Every one of them describes a
/// registration that would succeed and then produce a service which cannot do
/// its job -- which is the worst shape this system has, because the failure is
/// silent from both ends: the operator is told the service was installed, and a
/// scheduler that leases the worker out sees clients fail to reach it with no
/// error anywhere.
///
/// `--advertise` is the one worth spelling out. Left empty it defaults to
/// `{--bind}:{--port}`, and `--bind` defaults to `0.0.0.0`, which is not an
/// address any client can dial. A worker registered that way registers happily,
/// heartbeats happily, is leased, and never answers.
/// @param cfg Configuration about to be baked into a registration.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> NodeServiceRejection(NodeConfig const& cfg);

/// Why this worker's scheduler configuration cannot work, if it cannot.
///
/// A *startup* rule rather than an install-time one, and the split is deliberate:
/// each of these is fatal every time the process runs, not only when a registration
/// is written, so gating them on `--install-service` would let a hand-started
/// scheduler make the identical mistake with nothing saying so.
///
/// Every rule here describes a configuration that would **start successfully** and
/// then not work -- silent from both ends, which is the shape this codebase refuses
/// at the one moment an operator is watching.
/// @param cfg The parsed configuration.
/// @return The refusal and its remedy, or nullopt when the configuration can work.
[[nodiscard]] std::optional<std::string> SchedulerPolicyRejection(NodeConfig const& cfg);

/// Render the usage text from the same rows the parser matches.
/// @param color Whether to emit ANSI colour.
/// @return The complete help text.
[[nodiscard]] std::string HelpText(UsageColor color);

} // namespace FastCache::Node

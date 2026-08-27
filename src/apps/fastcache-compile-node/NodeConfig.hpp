// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
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
/// What the operator asked this process to do to the cluster, if anything.
///
/// An `enum class` and not three booleans, because the three are mutually
/// exclusive: a command line naming two of them is a mistake somebody made rather
/// than a request, and `None` -- the ordinary case, a worker starting up -- has to
/// be one of the states rather than "all three happen to be false".
enum class ClusterAction : std::uint8_t
{
    None = 0, ///< Serve, rather than administer.
    Status,   ///< Print what the cluster has agreed.
    Set,      ///< Change one replicated setting.
    Forget,   ///< Remove a member.
    Admit,    ///< Add a member, or record that one has moved.
};

/// One cluster-administration request, as parsed from the command line.
struct ClusterRequest
{
    ClusterAction action { ClusterAction::None };

    /// The setting name for `Set`, the member id for `Forget` and `Admit`.
    std::string key;

    /// The setting's new value for `Set`, the consensus endpoint for `Admit`,
    /// empty otherwise.
    std::string value;
};

/// Split `name=value` as `--cluster-set` takes it.
///
/// At the FIRST `=`, so a value may contain one and a name may not -- the same
/// rule `ParsePeerSpec` applies and for the same reason. Splitting at the last one
/// would read `upstream=cache=1:6674` as a setting called `upstream=cache`, which
/// is refused as unknown while naming something the operator did not type.
/// @param text What the operator wrote.
/// @return The pair, or nullopt when it is not one.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> ParseSettingAssignment(std::string_view text);

struct NodeConfig
{
    std::string scheduler; ///< host:port of the scheduler's dispatch endpoint.
    std::string advertise; ///< host:port clients should reach this worker on.
    std::string bindAddress { "0.0.0.0" };
    std::uint16_t port { 6676 };

    /// fingerprint=compilerPath, repeatable. An OVERRIDE: naming any pins this
    /// worker to exactly that set, and naming none means "serve what this machine
    /// has".
    ///
    /// There is still no default COMPILER -- a default is how a job ends up running
    /// against something nobody chose -- and the distinction is the whole of #139:
    /// "no default" and "no discovery" are different claims. Which compilers a
    /// machine holds is a fact the node can establish, and it is the half of the
    /// configuration that has to be redone after every toolchain upgrade.
    std::vector<std::string> toolchains;

    /// Whether a worker given no `--toolchain` surveys the machine for compilers.
    ///
    /// On by default, because the whole point is that installing the package is the
    /// setup. `--no-toolchain-discovery` is for the operator who wants the empty set
    /// to stay empty, and it is not a null flag: with it set and no `--toolchain`,
    /// `NodeServiceRejection` still refuses to register a service that provably
    /// cannot start, which is the guard that used to apply unconditionally.
    bool toolchainDiscovery { true };
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

    /// File holding the credential the dashboard requires, or empty for none.
    ///
    /// A FILE and not a flag, for the reason `--cluster-key-file` is one: a command
    /// line is readable through `ps`. And a credential of its own rather than
    /// `--requirepass`, which points the other way -- that is the secret this node
    /// *presents* to the scheduler, held by every member of the fleet, so reusing it
    /// would let any worker read every other node's fleet map.
    ///
    /// Required when the admin surface is not on loopback: a fleet map on a public
    /// port with no credential is what this flag exists to stop somebody doing by
    /// accident.
    std::filesystem::path dashboardTokenFile;

    /// Certificate the admin surface serves TLS with, or empty for plaintext.
    ///
    /// Spelled as the daemon spells it, because an operator copies these between
    /// the two binaries. There is deliberately **no `--tls` boolean**: TLS is on by
    /// naming a certificate and a key, which removes the state "TLS requested, no
    /// material" that a boolean makes reachable.
    std::filesystem::path tlsCertFile;

    /// Private key for `tlsCertFile`. Both or neither.
    std::filesystem::path tlsKeyFile;

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
    /// **A quarter of host RAM**, clamped to [512 MiB, 8 GiB] -- the same
    /// `DefaultMaxMemoryBytes()` the daemon uses, rather than a second opinion about
    /// the same question. This was a flat 256 MiB, chosen on the argument that a node
    /// shares a workstation with somebody's editor and browser. That argument aged
    /// badly: the machines this runs on now start at 16 GB, one object file is
    /// routinely megabytes, and a few hundred of them is the whole cache -- so the
    /// tier missed on exactly the rebuild it exists to serve, on every machine, and
    /// only an operator who read this file ever found out.
    ///
    /// The clamp is what keeps the old argument's substance: the floor means even a
    /// small laptop gets a cache worth having, and the ceiling means a 512 GB build
    /// server does not quietly take 128 GB resident for a cache nobody asked for.
    /// Memoised in `DefaultMaxMemoryBytes()`, which matters here because this struct
    /// is default-constructed freely -- including for the `defaults` instance
    /// `MakeNodeServiceSpec` diffs against.
    std::uint64_t cacheMemoryBytes { DefaultMaxMemoryBytes() };

    /// Bytes the on-disk half may hold. 0 means "grow as needed".
    ///
    /// Only consulted when `cacheDir` names a path; without one there is no disk
    /// tier for a budget to bound. Unbounded by default, matching the daemon's
    /// `--storage-max-disk` -- a cache asked to survive restarts is usually asked
    /// to keep what it has -- but a node runs on somebody's workstation, so this
    /// is the flag that exists because that default is not always the right one.
    std::uint64_t cacheDiskBytes { 0 };

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

    /// This node's identity in the cluster, or empty to run without consensus.
    ///
    /// The switch for the whole consensus tier, and empty is the default because the
    /// common deployment is one machine. A node alone leads trivially -- it schedules
    /// for itself and nobody else -- and requiring an operator to configure a
    /// one-member cluster to get that would be ceremony for the ordinary case.
    ///
    /// Given, it must appear in the cluster with an endpoint, which is what
    /// `--raft-peer` supplies.
    std::string nodeId;

    /// Where this node answers its peers' Raft traffic.
    ///
    /// A bare port binds the WILDCARD, like `--listen-scheduler` and unlike
    /// `--listen-cache`: peers are on other machines by definition, so a loopback
    /// default would be one that silently cannot work.
    std::string raftListen;

    /// The cluster's members as `id=host:port`; repeatable.
    ///
    /// Both halves in one token because they are one fact. A member id without an
    /// address is a node the cluster counts towards quorum and cannot reach -- the
    /// residual `RaftMembership` recorded, and the reason `Cluster::ClusterMember`
    /// pairs them.
    ///
    /// This is the BOOTSTRAP set only. Once the cluster is running, membership is a
    /// replicated log entry and this list is not consulted again -- which is what
    /// makes a node that was admitted at runtime survive a restart without anybody
    /// editing a config file on every other machine.
    std::vector<std::string> raftPeers;

    /// Where consensus keeps its durable state, empty for a default beside the cache.
    ///
    /// Durable by necessity rather than by preference: a node that answered a vote
    /// and forgot it would vote twice in one term after a restart, which is two
    /// leaders in one term.
    std::filesystem::path clusterDir;

    /// Which fleet this node belongs to, for discovery.
    ///
    /// Routing, not authentication, and saying so keeps it honest: it is plain
    /// text in every beacon, so treating it as a credential would be the mistake.
    /// What it buys is that two unrelated fleets on one segment ignore each other,
    /// which holds even when somebody shares a key across fleets -- which they
    /// should not.
    std::string clusterId { "fastcache" };

    /// Where discovery beacons go, empty to leave discovery off.
    ///
    /// Off by default because a cluster does not need it: `--raft-peer` is a list
    /// an operator typed, and that works. Discovery is what makes a *changing*
    /// fleet possible -- a machine that joins without anybody editing a file on
    /// every other machine.
    std::string discoveryAddress;

    /// Port this node's peers unicast their challenges and proofs to; 0 lets the
    /// kernel choose.
    ///
    /// **Not the beacon port, and it cannot be.** A beacon is a broadcast, so
    /// every node on the segment binds the same port -- co-hosted nodes on one
    /// machine included -- and only one of the sockets sharing a port is handed a
    /// unicast. A node that answered there would be answering for its whole
    /// machine, which is why two nodes on one host never finished proving the key.
    ///
    /// Kernel-chosen by default, because that always works and needs nobody to
    /// pick a number. It exists for one deployment where that is not enough: a
    /// host firewall scoped to the beacon port alone passes beacons and drops
    /// every challenge and proof, which presents as peers that are seen and never
    /// admitted. Pinning this is what such a site opens instead -- one port per
    /// node on the machine, since two nodes cannot share it.
    std::uint16_t discoveryReplyPort { 0 };

    /// File holding the cluster's pre-shared key.
    ///
    /// A path rather than the key itself, and that is a security decision rather
    /// than a convenience: a command line is readable through `ps` on every POSIX
    /// system and through the process list on Windows, and a service's arguments
    /// end up in a unit file or a registry key that more accounts can read than
    /// can read a mode-0600 file. A leaked key admits a node, and an admitted node
    /// returns objects the whole fleet then caches.
    std::filesystem::path clusterKeyFile;

    /// The name the platform's supervisor keys this worker's registration on.
    ///
    /// Distinct from the daemon's `FastCached` by default, because the two are
    /// separate services that a machine may well run both of -- sharing a name
    /// would make installing one silently displace the other.
    std::string serviceName { "FastCacheCompileNode" };

    /// Where a POSIX daemonized run writes its pid, empty for none.
    std::string pidfile;

    // ---------------------------------------------------------------------------
    // Every member below is one byte wide, and they are kept in a single run rather
    // than beside the setting each belongs to.
    //
    // This struct is almost entirely `std::string` and `std::filesystem::path`, so a
    // lone `bool` or byte-wide enum between two of them costs SEVEN bytes of padding
    // rather than one. Four of them scattered through it -- `dashboard`,
    // `tlsSelfSigned`, `logLevel`, `serviceScope` -- put the struct 32 bytes over
    // `clang-analyzer-optin.performance.Padding`'s 24-byte budget and failed the
    // build, which is why they live here and not where they read most naturally.
    //
    // Add the next flag HERE rather than next to what it configures. Its doc comment
    // is what carries the reader across; the position is a layout constraint.

    /// How chatty the log is.
    LogLevel logLevel { LogLevel::Info };

    /// Which supervisor domain `--install-service` registers into.
    ServiceScope serviceScope { ServiceScope::System };

    /// Whether `--cache-memory` was typed rather than derived.
    ///
    /// **Provenance, not value.** `MakeNodeServiceSpec` emits a flag only when it
    /// differs from the default, which is exactly right for a default that is a
    /// constant and quietly wrong for one that is a share of host RAM: an operator
    /// who reads the startup line and types that number back to pin it produces a
    /// value *equal* to the default on that machine, so the flag is dropped from the
    /// unit and the service re-derives from RAM on every start. The budget then
    /// moves under a VM resize or a memory upgrade -- silently, and precisely for the
    /// operator who took the trouble to pin it.
    ///
    /// So what is emitted follows whether they said it, not whether it differs.
    bool cacheMemoryExplicit { false };

    /// Whether the admin surface also serves the fleet dashboard.
    ///
    /// Off unless asked for, like every other surface this program serves. The page
    /// lists every member's hostname, endpoint and capacity -- a fleet map -- so an
    /// operator turns it on deliberately rather than acquiring it by naming a port
    /// they wanted `/metrics` on.
    ///
    /// Served on `--admin-listen`, and answered in full only while this node LEADS:
    /// a follower's registry holds whatever registered against it rather than the
    /// fleet, so it renders a page naming the leader instead of a partial picture.
    bool dashboard { false };

    /// Generate a self-signed certificate at startup instead of naming one.
    ///
    /// For an internal deployment where obtaining a certificate is the only thing
    /// between an operator and an encrypted admin surface. It is a boolean where
    /// `--tls-cert` is a path, and that is not a hole in the "TLS is on by naming
    /// material" rule but the same rule kept: the point of that rule is that no
    /// configuration can ask for TLS this node cannot then serve, and asking for
    /// this one *produces* the material.
    ///
    /// **Confidentiality, not identity.** Nothing signs it, so a client that has
    /// not been told its fingerprint out of band cannot tell this node from
    /// anything else answering on that address. The credential is still required
    /// off loopback for exactly that reason.
    bool tlsSelfSigned { false };

    /// Admit every caller to the fleet, rather than only `--fleet-member` hosts.
    ///
    /// The right answer for one machine, or a fleet whose network reachability is
    /// already its boundary. It is a *flag* rather than the behaviour you get by
    /// listing no members, because "no policy" and "a policy that admits everybody"
    /// have to be the same explicit decision -- listing nobody refuses everybody, and
    /// a scheduler that quietly served strangers would look identical to a healthy one
    /// from both ends.
    bool fleetOpen { false };

    /// Start with no cluster and wait to be admitted to one.
    ///
    /// The shape a machine being added to a running fleet has to have, and the only
    /// one that can be added at all. Without it a node named in `--raft-peer`
    /// bootstraps a cluster of its own: it elects itself, takes a term and a log,
    /// and afterwards refuses `AppendEntries` from every leader its configuration
    /// does not name — so the cluster that admitted it would be counting towards
    /// quorum a node that answers nobody. Two clusters cannot be merged by any local
    /// rule, which is why the joining node must never form one.
    ///
    /// It changes what `--raft-peer` MEANS rather than how much of it there is:
    /// those entries become nodes this one can reach rather than a cluster it
    /// belongs to. It still needs the cluster's addresses, because a leader
    /// admitting a member starts replicating at its own last index and only walks
    /// back to the beginning when the joiner's refusal reaches it.
    ///
    /// Additive rather than a change of meaning for the flag's absence: every
    /// existing deployment bootstraps, and inverting that would turn the documented
    /// single-node cluster into a node waiting forever for an invitation.
    bool raftJoin { false };
    bool daemon { false };           ///< Fork into the background / run under the SCM.
    bool installService { false };   ///< Register with the platform's supervisor and exit.
    bool uninstallService { false }; ///< Remove that registration and exit.

    /// Convert the `--cache-dir` store to this build's on-disk record layout
    /// and exit, instead of serving.
    ///
    /// Like `--install-service`, a mode rather than a serving option -- and,
    /// like it, deliberately absent from `BuildServiceArgv`: a worker that
    /// converted its store at every boot would replay one operator's decision
    /// forever, on a store that after the first run has nothing left to convert.
    bool migrateCache { false };
    bool help { false };
    bool version { false };

    /// What to do to the cluster instead of serving, when anything.
    ///
    /// A mode rather than a serving option, like `--install-service`: the process
    /// asks one question of a running cluster and exits. It is deliberately NOT
    /// part of a service registration -- a worker that administered the cluster at
    /// every boot would replay one operator's decision forever.
    ClusterRequest cluster;
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

/// Why this worker's configuration cannot work, if it cannot.
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
[[nodiscard]] std::optional<std::string> StartupPolicyRejection(NodeConfig const& cfg);

/// Render the usage text from the same rows the parser matches.
/// @param color Whether to emit ANSI colour.
/// @return The complete help text.
[[nodiscard]] std::string HelpText(UsageColor color);

} // namespace FastCache::Node

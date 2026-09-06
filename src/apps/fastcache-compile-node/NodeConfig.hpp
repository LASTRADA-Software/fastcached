// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    /// Seconds a stop waits for compiles still running, or 0 to wait forever.
    ///
    /// A stopping worker has to wait for something: a compile legitimately holds its
    /// slot for seconds, and abandoning one loses work a client is still waiting on.
    /// But an unbounded wait hands the decision to the supervisor, which answers it
    /// with `SIGKILL` and no diagnostic -- on Windows an SCM stop timeout an operator
    /// reads as "the service is hung" rather than "a compile is still running"
    /// (#239).
    ///
    /// A flag rather than a constant because the right value is how long *this*
    /// site's compiles legitimately run, which nothing in this process can know --
    /// and a compile-time answer on a binary whose `--install-service` replays its
    /// command line forever is a value nobody can move afterwards.
    ///
    /// Zero is "wait forever", which is what this did before the flag existed. It
    /// stays reachable so an operator who prefers the supervisor's timeout to this
    /// one can say so, rather than discovering the change as a behaviour they cannot
    /// turn off.
    std::uint32_t drainTimeoutSeconds { 30 };

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

    /// File holding the credential the SCHEDULER verbs require, or empty for none.
    ///
    /// The inbound half of `--requirepass`, which is outbound only -- that flag is
    /// the secret this node *presents* when it registers, and until #289 nothing on
    /// the receiving side ever checked one. So a scheduler port reachable from the
    /// network served `Register`, `Lease` and the cluster verbs to anyone who could
    /// open a socket to it; membership is an anti-leeching rule about which hosts an
    /// operator listed, not a credential.
    ///
    /// A FILE for the reason `dashboardTokenFile` is one: a command line is readable
    /// through `ps`. Unlike the dashboard's, this secret is deliberately the SAME one
    /// every member already holds as `--requirepass` -- that is what it is for, and a
    /// separate one would mean distributing two.
    ///
    /// **A bearer token, so its confidentiality rests on the transport.** Anyone who
    /// can read the wire can replay it, exactly as for `--requirepass` and the
    /// dashboard credential. That is a property of the scheme rather than a defect in
    /// it, and a MAC would not fix it: this credential authenticates a connection
    /// this process terminates, so there is nothing for a signature to bind that the
    /// connection does not already establish.
    std::filesystem::path schedulerTokenFile;

    /// Certificate the admin surface serves TLS with, or empty for plaintext.
    ///
    /// Spelled as the daemon spells it, because an operator copies these between
    /// the two binaries. There is deliberately **no `--tls` boolean**: TLS is on by
    /// naming a certificate and a key, which removes the state "TLS requested, no
    /// material" that a boolean makes reachable.
    std::filesystem::path tlsCertFile;

    /// Private key for `tlsCertFile`. Both or neither.
    std::filesystem::path tlsKeyFile;

    /// Whether this node serves the fleet's scheduler verbs.
    ///
    /// Off by default and for the same reason `--admin-listen` is: handing out other
    /// machines' CPU time is an operator's decision, not something they get by
    /// starting a worker.
    ///
    /// A flag rather than an address since the surfaces merged (#290) -- the scheduler
    /// verbs are answered on `nodeListen`, beside the cache verbs, so there is no
    /// second address left for it to name. What it still decides is where a bare
    /// `--listen-node` binds: a node that schedules takes the WILDCARD, because a
    /// scheduler no peer can dial is a scheduler that does nothing, and one that does
    /// not takes loopback. See `NodeListenDefaultHost`.
    bool serveScheduler { false };

    /// Peers this node serves, as `host:port`; repeatable.
    ///
    /// Gates **all three** of this node's surfaces through one `NodeMembership`: the
    /// scheduler decides who may spend the fleet's CPU, the compile port decides who
    /// may spend *this machine's*, and the cache tier decides who may read what those
    /// compiles produced. So a plain worker running no scheduler needs this exactly
    /// as much as a scheduler does -- without it, its compile port admits its own
    /// machine and refuses every dispatched job (#235).
    ///
    /// Only the host part is used -- a peer connecting comes from an ephemeral source
    /// port, so an endpoint is not something a connection can be matched against. The
    /// endpoint form is accepted because it is what discovery produces and what an
    /// operator has written down.
    ///
    /// Kept for the process's life on a clustered node too: consensus ADDS its member
    /// set to what is listed here rather than replacing it, because this list is how a
    /// machine that never joins consensus -- a developer's laptop, a CI runner -- is
    /// admitted at all (#251).
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
    /// Loopback on a node that does not schedule, the wildcard on one that does, and
    /// that asymmetry is the anti-leeching rule rather than a preference: a scheduler
    /// no peer can dial does nothing, while a cache any host can dial is this machine's
    /// entire build output served to strangers. It used to be a difference between two
    /// surfaces; since they merged (#290) it is a difference between two
    /// configurations of one, and `NodeListenDefaultHost` is where it is decided.
    ///
    /// Widening it is an operator's decision, and even then `CacheResponder` admits
    /// only THIS MACHINE (#287) -- not this cluster's members, which is what it
    /// admitted until locality became a property of the verb. So widening this address
    /// buys reaching the tier from this host under another address, and nothing else:
    /// on a scheduling node, whose port faces the network by default, the cache verbs
    /// are closed by that policy alone rather than by the socket.
    ///
    /// A port already taken is fatal when the operator **named** it and a warning when
    /// it is this default. Typed, it is a promise, and a broken promise is fatal;
    /// defaulted, a node sharing a machine with `fastcached` would otherwise refuse to
    /// start over a convenience nobody requested, and the launcher reaches the daemon
    /// on that port instead. Never silently, either way.
    ///
    /// Which of the two applies is `nodeListenExplicit`, and it has to be: comparing
    /// this value against the default cannot see the operator who typed the default.
    /// `--admin-listen` needs no such bit and draws no such distinction -- its default
    /// is *empty*, so there is no address to arrive at without asking, and every bind
    /// failure on it is unconditionally fatal.
    std::string nodeListen { "127.0.0.1:6674" };

    /// The shared `fastcached` this node reads through to, or empty for none.
    ///
    /// Empty is honest rather than broken -- one developer's machine has no shared
    /// cache, and `NoUpstream` is what that configuration gets.
    std::string upstream;

    /// The credential this node PRESENTS, from `--requirepass`.
    ///
    /// Outbound only: it is what the launcher half of this binary sends to an
    /// upstream `fastcached`, and what a worker sends when it registers. What this
    /// node REQUIRES of its own callers is `--scheduler-token-file`, and the two are
    /// deliberately separate settings -- a node that presented and demanded the same
    /// secret would make every client of its cache a peer of its scheduler.
    ///
    /// There is no username beside it. One was declared here, parsed by nothing and
    /// read by nothing, and it is removed rather than left: a dead field next to a
    /// live credential is what somebody later wires up on the assumption it was
    /// always meant to work, and a half-wired username on an authentication path is
    /// worse than no username at all
    /// ([#385](https://github.com/LASTRADA-Software/fastcached/issues/385)). If one
    /// is ever wanted it arrives as a row in `NodeOptions()`, like everything else.
    std::string token;

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
    /// A bare port binds the WILDCARD, like a scheduling node's `--listen-node`
    /// and unlike a worker's: peers are on other machines by definition, so a loopback
    /// default would be one that silently cannot work.
    std::string raftListen;

    /// The cluster's members, from `--raft-peer=<id>=<host>:<port>`; repeatable.
    ///
    /// Both halves in one token because they are one fact. A member id without an
    /// address is a node the cluster counts towards quorum and cannot reach -- the
    /// residual `RaftMembership` recorded, and the reason `Cluster::ClusterMember`
    /// pairs them.
    ///
    /// Stored PARSED rather than as the tokens an operator typed, which is what makes
    /// a malformed one unrepresentable: the grammar is `Cluster::ParseMemberSpec` and
    /// it runs in the option table, so a token that names no member is refused where
    /// it was typed. It used to be refused inside `ConsensusTier::Start` instead -- a
    /// layer `--install-service` returns long before reaching, so a registration
    /// carrying one was written happily and then died at every boot (#168).
    ///
    /// This is the BOOTSTRAP set only. Once the cluster is running, membership is a
    /// replicated log entry and this list is not consulted again -- which is what
    /// makes a node that was admitted at runtime survive a restart without anybody
    /// editing a config file on every other machine.
    std::vector<Cluster::ClusterMember> raftPeers;

    /// Where consensus keeps its durable state, empty for a default beside the cache.
    ///
    /// Durable by necessity rather than by preference: a node that answered a vote
    /// and forgot it would vote twice in one term after a restart, which is two
    /// leaders in one term.
    std::filesystem::path clusterDir;

    /// Which fleet this node belongs to.
    ///
    /// **Still not a credential**, and saying so keeps it honest: it is plain text in
    /// every beacon, so anybody on the segment can read it and anybody can claim it.
    /// What it buys in discovery is that two unrelated fleets on one segment ignore
    /// each other.
    ///
    /// **It is also inside every lease grant's MAC since
    /// [#322](https://github.com/LASTRADA-Software/fastcached/issues/322)**, and that
    /// is a second job rather than a promotion to credential. A grant naming another
    /// fleet is refused *after* its MAC has verified -- so what the id decides is
    /// whose authentic grants this node honours, never whether a grant is authentic.
    /// Two fleets that share a `--cluster-key-file` and differ here stop compiling for
    /// each other; two that share both are one fleet as far as leases are concerned,
    /// which is what the default means for everybody who never set it.
    ///
    /// So it now has an operational consequence: an operator running two fleets from
    /// one key file must give them different ids, and the default is what makes that
    /// a thing they have to do rather than get.
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
    ///
    /// Read by **two** surfaces, which is worth stating because it used to be one:
    /// discovery proves the cluster's identity with it, and the scheduler signs
    /// lease grants with it (`Distributed/LeaseToken.hpp`). Without one a grant is a
    /// bare serial and a worker has nothing to check, so anybody who can reach a
    /// compile port can spend it.
    std::filesystem::path clusterKeyFile;

    /// The name the platform's supervisor keys this worker's registration on.
    ///
    /// Distinct from the daemon's `FastCached` by default, because the two are
    /// separate services that a machine may well run both of -- sharing a name
    /// would make installing one silently displace the other.
    std::string serviceName { "FastCacheCompileNode" };

    /// Where a POSIX daemonized run writes its pid, empty for none.
    std::string pidfile;

    /// The configuration file the operator named, or empty to look for one.
    ///
    /// Read BEFORE the rest of this structure is filled in, by a first parse whose
    /// only purpose is to find this field -- so it is the one setting that cannot
    /// come from a file. A `config:` key inside a configuration file names the file
    /// to read, which is either the file itself or another one, and neither answer
    /// is one an operator should have to reason about.
    ///
    /// Empty is not "no file". A named path is strict: absent, unreadable or
    /// malformed is a refusal, because an operator who typed a path is owed the news
    /// that it did not arrive. An empty one falls back to the machine-wide candidate
    /// this application looks up, which is skipped when it is not there -- the rule
    /// `Config/DefaultConfigPath` already states for the daemon, applied to the
    /// second binary that has a file.
    std::string configPath;

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

    /// Whether every console line carries an ISO 8601 UTC time.
    ///
    /// **On by default under macOS, off elsewhere** (#496), from the one
    /// `DefaultLogTimestamps` both binaries read. The reason is the sink rather than
    /// the binary: launchd redirects a service's output to a plain file nothing
    /// stamps, journald stamps every line on Linux, and the Windows service path
    /// never reaches this logger at all. The four-sink table is in
    /// `.agent/rules/platform-service-and-config.md` and is not repeated here.
    ///
    /// A DEFAULT, so `log_timestamps: false` in a configuration file still wins by
    /// ordinary precedence. That is the difference between this and having the
    /// installer append `--log-timestamps`, which sets the explicit bit and would
    /// silently kill a key the shipped reference configuration documents.
    ///
    /// Off elsewhere is not a claim that times do not matter there: a foreground run,
    /// a redirected file and a CI artefact are unstamped on every platform, and a
    /// completed run cannot be asked afterwards (#457). It is a claim that the
    /// *supervised* case is already covered.
    bool logTimestamps { DefaultLogTimestamps };

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

    /// Whether `--listen-node` was typed rather than defaulted.
    ///
    /// **Provenance, not value**, like `cacheMemoryExplicit` above -- and here it
    /// decides whether the node STARTS at all. What the two answers are, and why they
    /// differ, is on `nodeListen`; this is the bit that picks between them, and it
    /// has to be a bit, because `--listen-node=127.0.0.1:6674` is a promise whose
    /// value equals the default (#286).
    bool nodeListenExplicit { false };

    /// Whether each remaining service-argv flag was TYPED rather than defaulted.
    ///
    /// One bit per flag `MakeNodeServiceSpec` emits, because emitting on a value
    /// COMPARISON is only sound while every default is a compile-time constant the
    /// next start re-derives identically (#713). That is a property of today's
    /// constants and not of the mechanism, and this binary has already watched it
    /// move once: `DefaultLogTimestamps` became platform-dependent in #496, and the
    /// `--log-timestamps` pair below still carries the comment explaining what that
    /// cost. The daemon's audit under #349 found FIVE rows once somebody looked,
    /// not the one that had been reported.
    ///
    /// So the bits are not added where a default looks risky today -- that judgement
    /// is what has to be re-made correctly every time a constant changes, by whoever
    /// changes it, without the change looking like it touches registration at all.
    /// They are added everywhere, and the mechanism stops depending on the
    /// constants.
    ///
    /// Set only by the ARGV parse: `--install-service` builds its spec from
    /// `cliOnly`, so a key in a config file never reaches these and never gets baked
    /// into a unit that would then outrank the file it came from.
    bool schedulerExplicit { false };
    bool advertiseExplicit { false };
    bool slotsExplicit { false };
    bool nodeClassExplicit { false };
    bool adminListenExplicit { false };
    bool cacheDiskBytesExplicit { false };
    bool nodeIdExplicit { false };
    bool raftListenExplicit { false };
    bool clusterIdExplicit { false };
    bool discoveryAddressExplicit { false };
    bool discoveryReplyPortExplicit { false };
    bool upstreamExplicit { false };
    bool drainTimeoutSecondsExplicit { false };
    bool logLevelExplicit { false };

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

    /// Admit every caller to this node, rather than only `--fleet-member` hosts.
    ///
    /// The right answer for a fleet whose network reachability is already its
    /// boundary. Like `--fleet-member` it governs every surface this node serves,
    /// worker included. It is a *flag* rather than the behaviour you get by listing
    /// no members, because "no policy" and "a policy that admits everybody" have to
    /// be the same explicit decision -- listing nobody refuses everybody, and a node
    /// that quietly served strangers would look identical to a healthy one from both
    /// ends.
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

    /// List every port this configuration would open, and exit.
    ///
    /// A mode rather than a serving option, like `--version` -- and deliberately
    /// absent from a service registration, since a worker that printed its ports at
    /// every boot instead of serving them would be a service that never starts.
    ///
    /// It renders the RESOLVED configuration rather than the defaults, which is what
    /// makes it worth having: an operator building a firewall list needs the ports
    /// this invocation would bind, and a list showing `127.0.0.1` for a node started
    /// with `--listen-node 0.0.0.0:6674` would tell them a surface is loopback-only
    /// when it is open to the network.
    bool printSurfaces { false };

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
///
/// **The cache arrives as what the tier BECAME, never as the flag that asked for
/// one** -- the rule this function exists to hold, and the one it got wrong. The
/// memory a node holds back from compiles is the memory its tier actually holds, and
/// `cacheMemoryBytes` is only ever the request: `--listen-node=` builds no tier at
/// all, `--cache-memory 0` builds no memory half, and a DEFAULT cache port already
/// held by a `fastcached` on the same machine is a warning the node carries on past.
/// None of the three is visible in the flag, so sizing from it reserved a quarter of
/// RAM for a tier that was never built and offered the fleet fewer slots than the
/// machine has -- silently, since nothing reports a reservation for a tier that does
/// not exist (#167).
///
/// Hence a **required** parameter rather than a defaulted convenience: a caller that
/// could omit it is a caller that can quietly go back to guessing. It must also be
/// obtained AFTER the tier has been started, which no signature can enforce --
/// `CacheCapacityOf(nullptr)` and "not built yet" are the same record. `WorkerBody`
/// is the one caller and derives both together, below the tier.
/// @param cfg The parsed configuration.
/// @param host What the machine says about itself.
/// @param cache What this node's cache tiers hold, from `CacheCapacityOf`; a
///        default-constructed record is a node with no tier.
/// @return The capacity to advertise, and to size this worker's own limit by.
[[nodiscard]] Distributed::NodeCapacity NodeCapacityOf(NodeConfig const& cfg,
                                                       IHostFactsSource const& host,
                                                       Distributed::NodeCacheCapacity const& cache);

/// Build a configuration from a file and a command line, in that order.
///
/// The whole of "the command line wins": the file's values and the command
/// line's reach the same fields through the SAME appliers, and precedence is
/// which loop runs second. There is no per-field merge, no per-field presence
/// bit and no second list to keep in step with the option table -- which is the
/// shape the daemon has, and which has shipped a flag that parsed but never
/// merged four times.
///
/// Pure with respect to I/O: the caller has already read the file, so this can be
/// driven from a test on every platform rather than only where a temporary file
/// and a spawned process are cheap. `main` reads and then calls this, and has no
/// other path to a merged configuration.
///
/// @param settings What the file carried, from `ReadYamlSettings`.
/// @param path The file, for error attribution only.
/// @param args The command line, program name already removed. It must be the
///        SAME argv the caller parsed to find the config path -- it is re-applied
///        here, so anything the first parse accepted this one accepts too.
/// @param result Populated on success; left in an unspecified state on failure,
///        because a file that failed halfway has applied part of a document
///        nobody wrote.
/// @return Nothing, or the first setting that could not be applied.
[[nodiscard]] std::expected<void, ConfigError> ApplyNodeConfiguration(std::vector<YamlSetting> const& settings,
                                                                      std::filesystem::path const& path,
                                                                      std::span<char const* const> args,
                                                                      NodeConfig& result);

/// Every accepted option, one row each.
///
/// The same table idiom the daemon and the launcher use, so an accepted spelling
/// is necessarily a documented one and adding a flag is adding a row.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept;

/// Every setting a candidate configuration changes that cannot take effect live.
///
/// **EVERY one, never the first.** A reload that reports one unreloadable field and
/// stops sends the operator round the same loop per field: they fix it, save, and are
/// refused again for the next. The whole answer in one refusal is the difference
/// between a diagnosis and a guessing game.
///
/// Driven by `NodeOptions()`'s own `reloadable` and `same` columns, so a row added
/// tomorrow is covered without touching this — and a row that forgets its comparator
/// does not compile, which is the guard beside the table.
///
/// @param previous What the worker is running with.
/// @param candidate What the file now says.
/// @return The `--flag` spellings that changed and may not, in table order. Empty
///         means the candidate may be published.
[[nodiscard]] std::vector<std::string_view> UnreloadableChanges(NodeConfig const& previous, NodeConfig const& candidate);

/// The immutability rule `ConfigReloaderOf<NodeConfig>` runs, as a `ConfigError`.
///
/// A thin shape over `UnreloadableChanges` because the reloader speaks errors and the
/// table speaks field names; the list is joined into one sentence naming all of them.
/// @param previous What the worker is running with.
/// @param candidate What the file now says.
/// @return Nothing, or which settings may not change at runtime.
[[nodiscard]] std::expected<void, ConfigError> ValidateNodeReloadable(NodeConfig const& previous,
                                                                      NodeConfig const& candidate);

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
/// `--advertise` is the one worth spelling out. Left empty it defaults to whatever
/// the `Node` surface resolves to, which is loopback on a node that does not
/// schedule -- not an address any other machine can dial. A worker registered that
/// way registers happily, heartbeats happily, is leased, and never answers.
/// @param cfg Configuration about to be baked into a registration.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> NodeServiceRejection(NodeConfig const& cfg);

/// The reloadable flags that are CLAIMS this worker made to the fleet.
///
/// Changing one means re-deriving what this node serves and re-registering, so the
/// scheduler stops dispatching against a set this worker no longer has.
inline constexpr std::array<std::string_view, 2> AdvertisedReloadableFlags { "--toolchain", "--no-toolchain-discovery" };

/// The reloadable flags that are local wiring and reach no other machine.
///
/// A list rather than "whatever is not advertised", so that adding a reloadable row
/// forces a choice instead of defaulting into the cheap answer.
inline constexpr std::array<std::string_view, 1> LocalReloadableFlags { "--log-level" };

/// Whether a reload changed something this worker had TOLD the fleet.
///
/// The band #403 made reloadable is not local wiring: `--toolchain` and
/// `--no-toolchain-discovery` decide the fingerprints this node registers, so adopting
/// one without telling the scheduler leaves it dispatching against a set this worker no
/// longer serves. This is what separates "re-derive and re-register" from "apply and
/// carry on", and `--log-level` -- the third reloadable row -- is squarely the latter.
///
/// **Asked rather than assumed**, because the re-survey is the expensive thing this
/// process does: a driver spawned per compiler and a walk of every include tree, over
/// 300 s cold (#354). Re-running it because somebody raised the log level mid-incident
/// would be the worst possible moment to spend that.
///
/// **Driven off the table rather than comparing fields by hand.** The rows it consults
/// are named once, in `AdvertisedReloadableFlags`, and the comparison is each row's own
/// `same` -- so a flag's idea of "changed" is written in exactly one place, beside the
/// flag. Spelled out here instead, the two `!=` expressions would be a second copy of
/// two `FieldEq` comparators sitting in the very rows this is about.
///
/// A `static_assert` beside the table requires every `Reloadable::Yes` row to appear on
/// that list or on the local-only one, so a fourth reloadable row cannot be added
/// without its author saying which kind it is.
/// @param previous The configuration that was in force.
/// @param candidate The configuration just adopted.
/// @return Whether what this worker advertises has changed.
[[nodiscard]] bool AdvertisedClaimsDiffer(NodeConfig const& previous, NodeConfig const& candidate);

/// What a bare `--listen-raft` binds.
///
/// The wildcard, like a scheduling node's `--listen-node` and unlike a worker's:
/// peers are on other machines by definition, so a loopback default would be one that
/// silently cannot work. Named because two places have to agree on it -- the policy row that
/// refuses an unusable `--listen-raft` and the tier that binds it -- and a default
/// they disagreed about is a row accepting what the tier then refuses.
inline constexpr std::string_view RaftListenDefaultHost = "0.0.0.0";

/// What a bare `--listen-node` binds on a node that serves the scheduler verbs.
///
/// The wildcard, for the reason `--listen-raft` takes one: a scheduler hands out other
/// machines' work, so the callers are on other machines by definition.
inline constexpr std::string_view SchedulingNodeListenDefaultHost = "0.0.0.0";

/// What a bare `--listen-node` binds on a node that does not.
///
/// Loopback, the OPPOSITE of the two above and deliberately: this is the surface
/// `fastcache-cc` on this machine talks to, and a node's private cache reachable from
/// the network is a decision rather than something an operator gets by typing a port.
inline constexpr std::string_view WorkerNodeListenDefaultHost = "127.0.0.1";

/// Where a bare `--listen-node` binds under @p cfg.
///
/// **One surface, two defaults**, which is the shape #290 left behind. The cache verbs
/// and the scheduler verbs are answered on one listener now, and they pulled its
/// address in opposite directions while they had one each: a scheduler no peer can
/// dial does nothing, and a cache every host can dial is this machine's whole build
/// output served to strangers.
///
/// Making it follow `--serve-scheduler` keeps BOTH of the answers the two surfaces
/// gave. A worker or a plain cache node binds loopback exactly as `--listen-cache`
/// did; a scheduling node binds the wildcard exactly as `--listen-scheduler` did. What
/// changes is only that a scheduling node's cache verbs are now closed by
/// `CacheResponder`'s locality rule (#287) rather than additionally by the socket --
/// and that node's scheduler port faced the network before the merge anyway.
///
/// A function rather than a constant because a surface row's `defaultHost` is one
/// value and this depends on the configuration; the row delegates here rather than
/// carrying a second copy of the rule.
///
/// **It follows `--serve-scheduler` and NOT "is this node a fleet participant",** which
/// #463 asked for and which is deliberately refused here rather than left unwritten:
///
/// - It would save one flag, and only for an operator who has already typed three.
///   Widening the bind is what makes `CompilePortFacesTheNetwork` true, so the
///   `--cluster-key-file` row starts refusing; and the widened bind becomes the
///   advertised endpoint, so `AdvertisesWildcard` refuses until `--advertise` is named
///   too. The participant still cannot start without `--scheduler`, a membership flag,
///   `--advertise` and `--cluster-key-file`.
/// - It would silence the refusal that teaches. `AdvertisesPastALoopbackBind` answers
///   the operator who named `--advertise` and left the bind alone, and its message is
///   `--listen-node=0.0.0.0:6674` -- the ergonomics fix, delivered while they are
///   watching.
/// - **A defaulted `--listen-node` whose port is taken is a WARNING, not fatal**, and
///   such a node runs with no `0xFC` port at all while still registering and
///   advertising. Today a fleet worker must TYPE the address, so that collision is
///   fatal; under a widened default the fleet-facing case would land on the warning
///   path, be leased out, and answer nothing.
/// - "Is this a fleet participant" already has three spellings that deliberately
///   disagree -- the reachability rows' gate, `AdmitsRemotePeers` (which excludes a
///   loopback-only member list and includes `--raft-join`) and
///   `CompilePortFacesTheNetwork`. A default computed from a fourth would be a bind
///   decided by one predicate and judged by another.
///
/// What the ticket correctly found is that a worker typing neither flag was refused by
/// nothing; that is `AdvertisesLoopbackToARemoteScheduler`, a row rather than a wider
/// default.
/// @param cfg What the operator asked for.
/// @return The host a bare port falls back to.
[[nodiscard]] inline std::string_view NodeListenDefaultHost(NodeConfig const& cfg) noexcept
{
    return cfg.serveScheduler ? SchedulingNodeListenDefaultHost : WorkerNodeListenDefaultHost;
}

/// The endpoint this node tells other machines to dial.
///
/// **One derivation, because three consumers must agree or the fleet breaks in a way
/// none of them can see.** What a lease's MAC covers is this endpoint, so the property
/// the compile surface's `AuthRequired == false` rests on is:
///
///   the endpoint the scheduler SIGNS == the endpoint the worker VERIFIES ==
///   the endpoint clients actually REACH
///
/// `main` passes this value to `MakeWorkerLeaseValidator` and registers it with the
/// scheduler, and `AdvertisesWildcard` refuses a configuration on it at startup. Those
/// three read one fact, and it was written out by hand in two places -- so a startup
/// refusal could already judge a different endpoint from the one signed into every
/// lease, before any of them moved. Two authors of one endpoint is not a tidiness
/// problem here; it is a credential whose subject two components disagree about.
///
/// Judged on the endpoint the node WOULD advertise rather than on whether the flag was
/// typed, so an operator who spells the default out is answered identically.
/// @param cfg What the operator asked for.
/// @return The advertised `host:port`.
[[nodiscard]] std::string AdvertisedEndpoint(NodeConfig const& cfg);

/// What a bare `--admin-listen` binds.
///
/// Loopback, and it is what the dashboard's credential rule turns on: reaching loopback
/// already means being on the machine, so a bare port needs no token while a bind an
/// operator deliberately exposed does.
inline constexpr std::string_view AdminListenDefaultHost = "127.0.0.1";

/// Why a `--node-id` that names no `--raft-peer` cannot work.
///
/// A named constant rather than prose written into the policy row it fills, because
/// `ConsensusTier::Start` answers with **this** string. The invariant is decided by
/// the startup table -- which is what makes an operator hear it while they are
/// watching -- and the tier is that same answer arriving at the boot of a
/// `NodeConfig` nobody parsed from an argv. Two spellings of one rule is what this
/// codebase's table idiom exists to prevent.
///
/// It ends without a full stop, alone among the rows: `main.cpp` prints a tier's
/// refusal as `"{}; refusing to start"`, and the tier's other messages are written
/// as fragments for exactly that. One message serving two callers has to suit the
/// one that appends.
inline constexpr std::string_view NodeIdNamesNoPeerRefusal =
    "--node-id names no --raft-peer: this node must name the endpoint its peers dial, whether it bootstraps a "
    "cluster or joins one, and consensus cannot start without one";

/// The `--raft-peer` entry `--node-id` names, if the list names it at all.
///
/// The predicate behind `NodeIdNamesNoPeerRefusal`, shared for the reason that
/// constant is: the startup table asks it for a verdict and `ConsensusTier::Start`
/// asks it for the member itself, and a rule asked two ways is one that drifts.
///
/// Answers for the list as typed, so a node with no `--node-id` at all names no
/// member -- which is not a refusal on its own: an empty id means this node runs no
/// consensus, and the table's own row is what decides whether that is a mistake.
/// @param cfg The parsed configuration.
/// @return A pointer into `cfg.raftPeers`, valid for as long as `cfg` is, or nullptr.
[[nodiscard]] Cluster::ClusterMember const* ClusterSelfMember(NodeConfig const& cfg) noexcept;

/// Whether this node runs consensus, and therefore whether anything will ever tell
/// its scheduler what term it is in.
///
/// **A named predicate because two tiers have to agree about it, and they are built
/// apart** ([#613](https://github.com/LASTRADA-Software/fastcached/issues/613)).
/// `StartConsensusOrExplain` answers "is there a cluster to start" and
/// `SchedulerTier` answers "will somebody publish my role"; those are the same
/// question, and while each spelled `cfg.nodeId.empty()` for itself they were two
/// authors of one rule -- the shape `ClusterSelfMember` above exists to prevent, one
/// flag along.
///
/// The disagreement is not hypothetical: the scheduler assumed nobody would, published
/// standalone leadership at term 0, and then consensus published a real term over the
/// top of it -- leaving a window in which the surface answered `Lease` as a leader that
/// had not been elected.
///
/// It reads `--node-id` alone, which is exactly what `StartConsensusOrExplain` reads:
/// a node with no id runs no consensus, which is the one-machine deployment and by far
/// the common one. Whether the rest of the cluster flags make SENSE is
/// `StartupPolicyRejection`'s question and is asked before any tier is built.
/// @param cfg The parsed configuration.
/// @return True when a consensus driver will run and report a role.
[[nodiscard]] bool RunsConsensus(NodeConfig const& cfg) noexcept;

/// Who this node admits, as one line an operator reads at startup.
///
/// One spelling for two callers -- the scheduler tier's ready line and the worker's
/// -- because the policy is the **node's** rather than any one surface's, and a
/// phrase each of them built separately is one that drifts. Read off the
/// configuration rather than off the oracle: the count is a property of what the
/// operator wrote, and the oracle is shared by three surfaces and no longer any one
/// tier's to inspect.
///
/// It says "this machine" out loud, because that admission is unconditional and an
/// operator reading "2 member host(s)" would otherwise not know their own builds
/// were covered. And it names the flag that would fix it when there is no policy at
/// all, which is the whole of #235's second half: such a worker starts, logs a
/// healthy line and refuses every dispatched compile, so the one line an operator
/// reads has to say that the port is closed. A scheduler cannot reach that case --
/// `--serve-scheduler` with no policy is refused at startup -- so it costs its line
/// nothing.
///
/// *What* it says depends on whether `--node-id` turned consensus on, because such a
/// node is about to admit hosts nobody typed and a line reading as a final answer
/// would mislead. Both remedies are named either way: the agreed member set ADDS to
/// what an operator listed rather than replacing it (#251), so `--fleet-member` is a
/// working answer on a clustered node too -- and it is the only route by which a
/// client machine, which is no cluster peer, is admitted at all.
/// @param cfg The parsed configuration.
/// @return A phrase naming who this node admits.
[[nodiscard]] std::string AdmissionSummary(NodeConfig const& cfg);

/// One path-valued flag whose file holds a secret.
///
/// A row rather than a branch, so a fifth secret-bearing flag is a fifth row and
/// the loop that asks about them is written once.
struct NodeSecretFile
{
    std::string_view flag;                    ///< The `--flag` spelling, and the key the coverage guard joins on.
    std::filesystem::path NodeConfig::* path; ///< Where the operator's answer lands.
};

/// One path-valued flag whose file is deliberately NOT a secret.
///
/// **Mandatory classification with a named opt-out, rather than an opt-in list.**
/// An opt-in list is exact about the flags it knows and silent about the ones it
/// does not, and silence reads identically to complete coverage -- which is #492,
/// and which is precisely how #752 describes the narrow answer that "looks
/// complete". `ctest -R "every path-valued flag is classified"` requires every
/// `=<path>` row of `NodeOptions()` to appear in exactly one of the two tables, so
/// a sixth path-valued flag does not compile into silence: its author has to say
/// which kind it is.
struct NodePublicPathFlag
{
    std::string_view flag; ///< The `--flag` spelling.
    std::string_view why;  ///< Why its file holds no secret. A forcing function, not a dead field.
};

/// Every path-valued flag whose file holds a secret.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<NodeSecretFile const> NodeSecretFileTable() noexcept;

/// Every path-valued flag whose file holds no secret, and why.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<NodePublicPathFlag const> NodePublicPathFlags() noexcept;

/// Every file this worker's secrets live in, in the order to report them.
///
/// **Two rules, not one, and only the first is #384's.** The configuration file is
/// provenance-gated: `--requirepass` can arrive in argv instead, where the exposure
/// is `ps` rather than a mode, and that is a different problem with a different
/// owner. The four files `NodeSecretFileTable()` names are not gated at all --
/// the path is not the secret and the file is, so a world-readable cluster key is
/// exposed however its path was named
/// ([#752](https://github.com/LASTRADA-Software/fastcached/issues/752)).
///
/// **A named file is reported whether or not a tier reads it**, and that is the
/// lesson the `--cluster-key-file` refusal in `StartupPolicyRejection` was narrowed
/// twice to learn: whether a surface exists is not a fact about the configuration,
/// so a rule whose premise is "somebody will read this" cannot state its premise
/// without guessing. The file holds a secret on disk either way.
///
/// @param cfg The merged configuration.
/// @param configFile The configuration file that was actually read, or empty when
///        none was. The RESOLVED path, since a discovered file is named by no flag.
/// @param secretNamedOnCommandLine Whether argv supplied `--requirepass`.
/// @return The paths, config file first when it qualifies; empties are kept out.
[[nodiscard]] std::vector<std::filesystem::path> NodeSecretFiles(NodeConfig const& cfg,
                                                                 std::filesystem::path const& configFile,
                                                                 bool secretNamedOnCommandLine);

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

/// Why this configuration cannot be REGISTERED, if it cannot.
///
/// Both tables, because an install is judged more strictly than a start rather than
/// differently. `--install-service` bakes the command line in and replays it at
/// every boot, so a startup rule -- every one of which is a pure invariant of that
/// command line -- is just as fatal to a registration as an install-time rule is,
/// and far more expensive: a start refuses once in front of the operator who typed
/// it, while a registration refuses forever into a log nobody reads.
///
/// The two tables stay separate on purpose and this only composes them.
/// `StartupPolicyRejection` must keep running at startup as well, or a hand-started
/// worker makes the identical mistake with nothing saying so; and
/// `NodeServiceRejection` is asked first, because its rules name the action being
/// taken and so read better against `--install-service` than a startup rule does.
///
/// Lives here rather than in `main()` because `main()` is in no test target, which
/// is precisely how the gap this closes survived (#166).
/// @param cfg Configuration about to be baked into a registration.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> NodeInstallRejection(NodeConfig const& cfg);

/// Render the usage text from the same rows the parser matches.
/// @param color Whether to emit ANSI colour.
/// @return The complete help text.
[[nodiscard]] std::string HelpText(UsageColor color);

} // namespace FastCache::Node

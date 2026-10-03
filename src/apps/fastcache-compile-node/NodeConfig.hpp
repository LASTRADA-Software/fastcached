// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeDefaults.hpp"
#include "NodeFormation.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <cstddef>
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

    /// `Admit`, recording the member as a LEARNER (#1449): replicated to, counted by no
    /// quorum, never a candidate. On a voter this demotes it; `Admit` on a learner
    /// promotes it.
    AdmitLearner,

    /// Admit a WORKER principal by its id and identity key, with no enrollment window (#178):
    /// `--cluster-admit-worker=<id>@<key>`, with the two lines the worker's `--print-identity`
    /// printed.
    AdmitWorker,
};

/// One cluster-administration request, as parsed from the command line.
struct ClusterRequest
{
    ClusterAction action { ClusterAction::None };

    /// The setting name for `Set`, the member id for `Forget`, `Admit` and
    /// `AdmitLearner`, and the worker's id for `AdmitWorker`.
    std::string key;

    /// The setting's new value for `Set`, the consensus endpoint for `Admit` and
    /// `AdmitLearner`, empty otherwise.
    std::string value;

    /// The member's identity key for `Admit` and `AdmitLearner` when the operator typed
    /// `@<key>` (#178), disengaged otherwise -- which the leader reads as *no opinion* and
    /// which keeps whatever key is recorded, never as a key to clear. Always engaged for
    /// `AdmitWorker`, whose grammar requires the key: a worker is admitted BY it.
    std::optional<Ed25519PublicKey> publicKey;
};

/// What an operator asked of a seed's enrollment window, instead of serving.
///
/// Its own enum rather than five more `ClusterAction` rows, because the two reach
/// different verbs on the wire and are gated differently at the far end -- and a single
/// enum would make `ClusterRequest::key` mean *a setting, a member, or a joiner*
/// depending on which arm read it. `None` is the ordinary case: a worker starting up.
///
/// A private enum: nothing transmits or persists these ordinals. It is parsed from argv
/// into `EnrollCommand` and keys a table through `Last`, and no byte of it reaches a
/// wire or a file -- so the order is free to change, and the `= 0` on `None` states the
/// value a default-constructed `EnrollCommand` holds rather than any contract with a
/// reader elsewhere. Said out loud because this tree holds both kinds and a declaration
/// that says neither reads as either (#308).
enum class EnrollAction : std::uint8_t
{
    None = 0,       ///< Serve, rather than administer a window.
    List,           ///< Print the window and everything waiting.
    Approve,        ///< Admit the named machine and let it collect the roster.
    Reject,         ///< Refuse the named machine.
    AutoApprove,    ///< Arm the leader's auto-approve deadline, or re-arm it from now.
    AutoApproveOff, ///< End the leader's auto-approve deadline.
    Clear,          ///< Drop every request nobody decided about.

    Last, ///< Not an action: the length of a table keyed by one.
};

/// What an operator asked this machine's own worker to do about a cordon, instead of
/// serving (#1303).
///
/// A private enum: parsed from argv and read by one early verb, and no byte of it reaches
/// a wire or a file -- the wire has its own `CompileCacheWire::CordonAction`. `None` is the
/// ordinary case, a worker starting up.
enum class CordonCommand : std::uint8_t
{
    None = 0, ///< Serve, rather than cordon.
    Cordon,   ///< Refuse new compiles and let the running ones finish.
    Lift,     ///< Take compiles again.
};

/// One enrollment-administration request, as parsed from the command line.
struct EnrollCommand
{
    EnrollAction action { EnrollAction::None };

    /// The joiner's id for `Approve` and `Reject`; empty for the rest.
    std::string subject;

    /// The key an `Approve` admits -- the one the operator compared, named with the id so a row
    /// that was replaced under that id is refused rather than admitted. Absent for the rest.
    std::optional<Ed25519PublicKey> key {};

    /// How long to arm the deadline for, for `AutoApprove`; zero for the rest.
    std::chrono::seconds duration {};
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

// The Disk row is REQUIRED to carry a shared-cache default: `DefaultSharedCacheDiskBytes`
// below has no other answer to give, and 0 is not a safe stand-in -- it is
// `--cache-memory`'s spelling of UNBOUNDED, so a fallback to it would let the shared
// tier grow until the disk is full rather than fail. Asserted here, at compile time,
// against the table itself, so the function's runtime check can never actually be
// false: the check stays (clang-tidy's `bugprone-unchecked-optional-access` cannot see
// a static_assert two declarations away), but what it guards is unreachable.
static_assert(TraitsFor(StorageTier::Disk).sharedTierDefaultBytes.has_value(),
              "the Disk tier must carry a shared-cache default byte budget");

/// `StorageTierTable`'s default byte budget for the shared tier this node may serve:
/// `TraitsFor(StorageTier::Disk).sharedTierDefaultBytes`, unwrapped.
///
/// A CHECKED read rather than a bare dereference, which is what keeps this clean under
/// `bugprone-unchecked-optional-access` -- the analyser has no way to know the
/// static_assert above already rules the empty case out. It is ruled out there and not
/// answered here with a fallback value, because 0 reads as UNBOUNDED for this budget:
/// the only correct answer to "the Disk row has no default" is a build that never
/// produces this binary, not a large number silently standing in for a small one.
/// @return The Disk tier's shared-cache default in bytes.
[[nodiscard]] std::uint64_t DefaultSharedCacheDiskBytes() noexcept;

struct NodeConfig
{
    /// host:port of the scheduler's dispatch endpoint, repeatable, in the order tried.
    ///
    /// A LIST because a registration replays its command line forever (#1310): one
    /// value is one machine's address baked into every unit file of the fleet, and
    /// retiring that machine then means re-registering every service. Several values
    /// are fallbacks for REACHING the fleet, never several fleets -- the heartbeat walks
    /// them in one round until one answers, and a `NotLeader` from any of them is
    /// followed to the endpoint it names without consulting this list.
    std::vector<std::string> schedulers;

    /// The identity keys of the cluster's voters, as a worker that runs no consensus is
    /// told them: `--voter-key`, repeatable (#178).
    ///
    /// Its trust ROOT and nothing more: the first roster it adopts must be endorsed by a
    /// strict majority of these, and from then on the roster it holds certifies its successor
    /// and these are never read again -- as the replicated state wins over the keys
    /// a bootstrap roster names. Keys and not addresses, because `--scheduler` may be a name that
    /// fronts several machines, and a key belongs to exactly one.
    std::vector<Ed25519PublicKey> voterKeys;
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

    /// Extra compile-argument spellings this site accepts, on top of the built-in
    /// per-driver-family table.
    ///
    /// EXTENDS the table and can never shrink it: `IsAcceptableJobArgument` consults
    /// these only after no built-in row matched, and a refusing row has already
    /// answered by then. A config that could remove a refusal would reintroduce #240,
    /// which is the hole the allowlist replaced a denylist to close.
    std::vector<std::string> extraAllowedArgs;

    /// Whether a worker given no `--toolchain` surveys the machine for compilers.
    ///
    /// On by default, because the whole point is that installing the package is the
    /// setup. `--no-toolchain-discovery` is for the operator who wants the empty set
    /// to stay empty, and it is not a null flag: with it set and no `--toolchain`,
    /// `NodeServiceRejection` still refuses to register a service that provably
    /// cannot start, which is the guard that used to apply unconditionally.
    bool toolchainDiscovery { true };
    /// How hard this machine may be driven. See `Distributed::NodeClass`.
    ///
    /// Defaults to `Workstation`, which is the safe answer rather than the common
    /// one: a node whose class nobody set is somebody's desktop until proven
    /// otherwise, and getting that backwards is a failure the person experiences as
    /// "my editor stutters" and never connects to a build fleet.
    ///
    /// Beside `toolchainDiscovery` for LAYOUT: a byte-wide member between the two
    /// 4-aligned `optional`s and `drainTimeout` below cost the struct past the
    /// padding budget clang-tidy enforces once `slots` became an `optional` (#206).
    Distributed::NodeClass nodeClass { Distributed::NodeClass::Workstation };
    /// Concurrent compiles this node offers the fleet, when the operator named a number.
    ///
    /// Absent sizes the machine from `nodeClass` and its hardware. A count is enforced
    /// here as well as advertised, through the one shared `Distributed::OfferableSlots`:
    /// two implementations of that arithmetic is how a worker comes to accept more jobs
    /// than the scheduler believes it has.
    ///
    /// **Zero is a real answer, and it is the absence of a worker component** (#206):
    /// see `RunsWorker`. It is the sizing value rather than a `--no-worker` beside it for
    /// #1022's reason -- a boolean beside the value is a second thing that can disagree
    /// with it -- and the same shape `--cache-memory 0` already has for the cache tier.
    /// An `optional` for `reservedCores`' reason: absent and zero are different
    /// instructions, and the `optional` is this row's provenance, so it needs no bit.
    std::optional<std::uint32_t> slots;

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

    /// How long a stop waits for compiles still running, in whole seconds, or zero to wait forever.
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
    /// Zero (`0s`) is "wait forever", which is what this did before the flag existed. It
    /// stays reachable so an operator who prefers the supervisor's timeout to this
    /// one can say so, rather than discovering the change as a behaviour they cannot
    /// turn off.
    std::chrono::seconds drainTimeout { 30 };

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
    /// A FILE and not a flag, for the reason every secret this node reads is one: a
    /// command line is readable through `ps`. And a credential of its own rather than
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

    /// Bytes the SHARED tier this node serves may hold, when the fleet's
    /// `shared-cache` setting names this machine.
    ///
    /// Defaults from `StorageTierTable`'s own column rather than a literal here, so
    /// the one place that answers "how large by default" stays the table. A present
    /// zero means "grow as needed", `--cache-memory`'s rule and not `cacheDiskBytes`'s
    /// -- the private tier defaults to unbounded because a cache asked to survive
    /// restarts is usually asked to keep what it has, while the shared tier defaults
    /// to a cap because it answers other machines' builds, on a filesystem an
    /// operator did not necessarily size for the whole fleet.
    std::uint64_t sharedCacheDiskBytes { DefaultSharedCacheDiskBytes() };

    /// Codec effort for the on-disk tier, and for the in-memory one.
    ///
    /// Both default to 3, zstd's speed/ratio knee and the value
    /// `CowTreeStorage::Options` already uses -- so a node that names neither
    /// behaves exactly as it did before these settings existed.
    int compressionLevel { 3 };

    /// Effort for `memoryCompression`. See `compressionLevel`.
    int memoryCompressionLevel { 3 };

    /// Values below this are written to disk uncompressed.
    ///
    /// 256 bytes, matching `CowTreeStorage::Options`' own default: below it the CPU
    /// rarely pays for itself and a codec can make a small record LARGER.
    std::size_t compressionMinBytes { 256 };

    /// Values below this are kept in memory uncompressed.
    ///
    /// 4096 rather than the disk tier's 256, matching `InMemoryLruStorage`'s own
    /// default. The two differ on purpose: an in-memory read pays the decompress on
    /// every hit, where a disk read has already paid for the I/O it saves.
    std::size_t memoryCompressionMinBytes { 4096 };

    /// Where this node serves cache verbs to its local clients; empty turns it off.
    ///
    /// Defaults to the address `fastcache-cc` already looks at when nobody sets
    /// `FASTCACHE_ADDR`, which is what makes the local tier work with **no
    /// configuration at all**: start a node, build, and the launcher finds it. The
    /// alternative — an off-by-default port an operator has to discover, and a
    /// `FASTCACHE_ADDR` they then have to point at it — is two steps to get the
    /// behaviour that is the point of running the program.
    ///
    /// **The wildcard on every node** (`DefaultNodeListen`), because every node is a fleet
    /// participant: a worker bound to loopback would advertise an address no other machine
    /// can dial. It was loopback on a node that did not schedule until the zero-config
    /// defaults; what keeps that safe is admission rather than the socket. `CacheResponder`
    /// admits only THIS MACHINE to the cache verbs (#287), whatever this address is, and
    /// every other verb refuses a caller that is not a member -- listing nobody refuses
    /// everybody.
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
    std::string nodeListen { DefaultNodeListen };

    /// The shared `fastcached` this node reads through to, or empty for none.
    ///
    /// Empty is honest rather than broken -- one developer's machine has no shared
    /// cache, and `NoUpstream` is what that configuration gets.
    std::string upstream;

    /// The credential this node PRESENTS, from `--requirepass`.
    ///
    /// Outbound only: it is what the launcher half of this binary sends to an
    /// upstream `fastcached`, and what a worker sends when it registers. This node
    /// REQUIRES no password of its own callers: a machine is admitted by the key it proves
    /// or the ticket it presents, never by a shared secret.
    ///
    /// There is no username beside it. One was declared here, parsed by nothing and
    /// read by nothing, and it is removed rather than left: a dead field next to a
    /// live credential is what somebody later wires up on the assumption it was
    /// always meant to work, and a half-wired username on an authentication path is
    /// worse than no username at all
    /// ([#385](https://github.com/LASTRADA-Software/fastcached/issues/385)). If one
    /// is ever wanted it arrives as a row in `NodeOptions()`, like everything else.
    ///
    /// **Renamed from `token` and retyped, together, at
    /// [#1125](https://github.com/LASTRADA-Software/fastcached/issues/1125).** The type is
    /// the fix: this is a credential held in a config struct, and `NodeConfig` is kept as
    /// TWO live snapshots by `ConfigReloader`, so as a `std::string` a rotated secret
    /// survived in the previous snapshot for as long as anything held it, released to the
    /// general allocator with its characters intact.
    ///
    /// The name is what makes the fix hold. `scripts/check-credential-containers.sh` finds
    /// credential holders by NAME, and `token` cannot be one of its rows: this tree spells
    /// a parser token, a CPU-feature token and a lease's public identifier the same way, so
    /// that row would refuse hundreds of correct declarations. `requirePass` names the flag
    /// it comes from, matches the daemon's field for the same flag, and is a row -- which
    /// is the script's own argument for renaming a holder rather than widening a
    /// vocabulary. The old name also had to be explained away in the paragraph above,
    /// because the node's OTHER credential is the one an operator calls a token.
    SecureString requirePass;

    /// This node's identity in the cluster, or empty to run without consensus.
    ///
    /// The switch for the whole consensus tier, and empty is the default because the
    /// common deployment is one machine. A node alone leads trivially -- it schedules
    /// for itself and nobody else -- and requiring an operator to configure a
    /// one-member cluster to get that would be ceremony for the ordinary case.
    ///
    /// Given, it is recorded, and consensus runs under it; the endpoint peers dial it at
    /// is `--raft-self` and `--listen-raft`'s.
    std::string nodeId;

    /// Where this node answers its peers' Raft traffic.
    ///
    /// A bare port binds the WILDCARD, like `--listen-node`: peers are on other machines by
    /// definition, so a loopback default would be one that silently cannot work.
    ///
    /// **On by default** (`DefaultRaftListen`): a node with no flags is a one-voter cluster
    /// of itself. The formation record decides whether the port OPENS (`RunsConsensus`, the
    /// mode's `raftListener` column); this says where, and an empty `--listen-raft=` closes it,
    /// which on a mode that listens is a node running no consensus.
    std::string raftListen { DefaultRaftListen };

    /// The HOST this node's peers dial it at, from `--raft-self=<host>`.
    ///
    /// **Not a duplicate of `raftListen`, and the difference is why this flag has to
    /// exist at all.** A bare `--listen-raft` binds the wildcard, so the address this
    /// node BINDS is routinely not one any peer could dial -- and a member must name
    /// the endpoint its peers dial. So the host is stated here and the port comes from
    /// `--listen-raft`, and `ConsensusDialAddressOf` is where the two become one.
    ///
    /// A HOST and never an endpoint: the port is not the operator's to repeat, and a
    /// value carrying one would produce `host:port:port`. No grammar row for the same
    /// reason `--bind` has none -- a host is only checkable by binding it.
    std::string raftSelf;

    /// This node's state directory as the operator named it, empty for the platform's default.
    ///
    /// Durable by necessity rather than by preference: a node that answered a vote
    /// and forgot it would vote twice in one term after a restart, which is two
    /// leaders in one term. Empty is not "no directory": every node has one now, and
    /// `stateDirectory` holds the default this one resolved. `ChosenStateDirectory` (and
    /// `NodeStateDirectory` over it) is the one reader of the two for anything the node
    /// keeps; this field alone answers only what the operator TYPED -- the option row, and
    /// what a service registration replays and hands over.
    std::filesystem::path clusterDir;

    /// The state directory this process resolved for a node that names no `--cluster-dir`,
    /// and why that one.
    ///
    /// **Not a flag**: `ApplyNodeStateDirectory` writes it from the environment and the
    /// process's privilege at startup, into every configuration this process builds, as
    /// `ApplyNodeIdentity` writes the id. Disengaged until then -- and left so when nothing
    /// resolves, which the start refuses by name. A registration never carries it: the next
    /// start resolves it again, as the process it will be.
    std::optional<NodeStateDirectoryChoice> stateDirectory;

    /// This host's DNS names as the start resolved them, or disengaged before it has.
    ///
    /// **Not a flag.** `ApplyHostNaming` writes it into every configuration this process
    /// builds. Disengaged means *not resolved yet* -- a parse, an install, a
    /// `--print-surfaces` -- and every rule judging an address derived from it answers that as
    /// *supplied at startup* rather than as a name that is missing. What `--advertise` and
    /// `--raft-self` fall back to.
    std::optional<NodeHostNames> hostNames;

    /// What the formation record says, as this configuration reads it, or disengaged when no
    /// record shaped it.
    ///
    /// **Not a flag.** `ApplyFormation` writes it, into every configuration this process builds,
    /// before any rule judges one or any tier starts: the record where one is kept, and before
    /// the first start mints one, the solitary record that start will mint. Disengaged is a
    /// configuration nothing formed -- a bare test configuration -- and it runs no consensus,
    /// because there is no mode to say it should (`RunsConsensus`).
    std::optional<NodeFormationView> formation;

    /// Which fleet this node belongs to.
    ///
    /// **Still not a credential**, and saying so keeps it honest: it is plain text in
    /// every beacon, so anybody on the segment can read it and anybody can claim it.
    /// What it buys in discovery is that two unrelated fleets on one segment ignore
    /// each other.
    ///
    /// **It is also inside every lease grant's signature since
    /// [#322](https://github.com/LASTRADA-Software/fastcached/issues/322)**, and that
    /// is a second job rather than a promotion to credential. A grant naming another
    /// fleet is refused *after* its signature has verified -- so what the id decides is
    /// whose authentic grants this node honours, never whether a grant is authentic.
    /// Since #178 a grant is signed by a voter's own key and verified against the roster a
    /// worker holds, so two fleets share nothing a grant could verify under unless a voter
    /// belongs to both; where one does, this id is what keeps their grants apart.
    std::string clusterId { "fastcache" };

    /// Where discovery beacons go, empty to leave discovery off.
    ///
    /// **On by default** (`DefaultDiscoveryAddress`'s port, sent to every up interface's directed
    /// broadcast; a NAMED address is used exactly -- `BeaconDestinationsFor`): discovery is
    /// what makes a fleet with no configuration possible -- a machine that joins without
    /// anybody editing a file on any machine. It runs only beside consensus, so a node that
    /// turns consensus off turns this off with it; only a TYPED `--discovery` is refused there.
    std::string discoveryAddress { DefaultDiscoveryAddress };

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

    /// Machines in the fleet to ask, `host:port` each, normalized by `Cluster::NormalizeSeed`.
    ///
    /// For the office no beacon reaches -- across a VPN, or a routed segment. Tried after the
    /// fleet this node remembers and before the domain's SRV record, and only while the node is
    /// alone. A seed is somewhere to ASK, never a member: what it answers is joined by approval.
    ///
    /// No provenance bit, for `schedulers`' reason: a list's default is empty, so every value
    /// present is one the operator typed and a registration replays each of them.
    std::vector<std::string> fleetSeeds;

    /// The name the platform's supervisor keys this worker's registration on.
    ///
    /// Distinct from the daemon's `FastCached` by default, because the two are
    /// separate services that a machine may well run both of -- sharing a name
    /// would make installing one silently displace the other.
    std::string serviceName { "FastCacheCompileNode" };

    /// `--firewall-allow` scopes: the remote addresses the firewall rules `--install-service`
    /// creates admit; empty admits any address. Install-time only, never replayed into the
    /// registration. A vector, so it sits outside the byte-wide run below.
    std::vector<std::string> firewallAllow;

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
    /// A DEFAULT, so a configuration file still wins by ordinary precedence. That is the
    /// difference between this and having the installer append `--log-timestamps`, which
    /// sets the explicit bit and would silently kill a key the shipped reference
    /// configuration documents. Each key is its flag: `log_timestamps: true` turns it on,
    /// `no_log_timestamps: true` turns it off, and `false` on either passes nothing, which
    /// leaves this default.
    ///
    /// Off elsewhere is not a claim that times do not matter there: a foreground run,
    /// a redirected file and a CI artefact are unstamped on every platform, and a
    /// completed run cannot be asked afterwards (#457). It is a claim that the
    /// *supervised* case is already covered.
    bool logTimestamps { DefaultLogTimestamps };

    /// Which supervisor domain `--install-service` registers into.
    ServiceScope serviceScope { ServiceScope::System };

    /// How `--install-service` registers this worker; never replayed into the registration.
    ServiceStart serviceStart { ServiceStart::Auto };

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
    /// has to be a bit, because `--listen-node=0.0.0.0:6674` (`DefaultNodeListen`) is a promise whose
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
    bool advertiseExplicit { false };
    bool nodeClassExplicit { false };
    bool toolchainDiscoveryExplicit { false };
    bool adminListenExplicit { false };
    bool cacheDiskBytesExplicit { false };
    bool sharedCacheDiskBytesExplicit { false };
    bool raftListenExplicit { false };
    bool raftSelfExplicit { false };
    bool discoveryAddressExplicit { false };
    bool discoveryReplyPortExplicit { false };
    bool upstreamExplicit { false };
    bool drainTimeoutExplicit { false };
    bool logLevelExplicit { false };
    bool compressionExplicit { false };
    bool compressionLevelExplicit { false };
    bool compressionMinBytesExplicit { false };
    bool memoryCompressionExplicit { false };
    bool memoryCompressionLevelExplicit { false };
    bool memoryCompressionMinBytesExplicit { false };

    /// Codec for values this node writes to its on-disk cache tier.
    ///
    /// `Zstd` because that is what `CowTreeStorage::Options` has always defaulted to
    /// and this flag only exposes it: any other default here would silently re-encode
    /// every existing node's disk tier on upgrade. Changing it is safe on a store
    /// already written -- each record carries its own codec tag, so reads decode by
    /// what they were written with and only later writes follow the new setting.
    ///
    /// Byte-wide, so it lives in this run rather than beside `cacheDir`; see the note
    /// above `logLevel`.
    CompressionCodec compression { CompressionCodec::Zstd };

    /// Codec for values held in this node's in-memory cache tier.
    ///
    /// `Identity` -- off -- because the tier has never compressed and turning it on
    /// by default would change the CPU cost of every existing deployment on upgrade.
    /// Worth naming when the working set exceeds the budget: `InMemoryLruStorage`
    /// charges its budget in STORED bytes, so a codec makes `--cache-memory` hold
    /// materially more, at a decompress per read.
    ///
    /// Independent of `compression`, which is the on-disk one.
    CompressionCodec memoryCompression { CompressionCodec::Identity };

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

    /// Admit every caller to this node, keyed or not, rather than only this machine and
    /// the machines the roster admits by key or ticket.
    ///
    /// The right answer for one machine, or for a fleet whose network reachability is
    /// already its boundary. It governs every surface this node serves, worker
    /// included. It is a *flag* rather than the behaviour an absent roster decays to,
    /// because a node that quietly served strangers would look identical to a healthy
    /// one from both ends. A revoked key is still refused under it.
    bool fleetOpen { false };

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
    /// Template `--seed-config` copies to the machine-wide config location.
    ///
    /// Empty means the verb was not asked for. The WORKER seeds its own file
    /// rather than `fastcached` seeding it, and that is the same argument the
    /// daemon's own action carries: the destination comes from the table this
    /// binary's startup lookup walks (`NodeApplicationName`), so the seeded path
    /// and the read path cannot drift. A packaging format with no conffile
    /// mechanism -- the MSI, the .pkg -- has no other way to ship a default that
    /// survives its own upgrades (#397).
    std::string seedConfigTemplate;

    bool printSurfaces { false };

    /// `--print-identity`: print this node's id, its public key and the `--cluster-admit` line
    /// that admits it, minting whatever the state directory does not hold yet, and exit
    /// (#178). What an operator admitting a member needs from it BEFORE it is admitted, since
    /// an admission names the key the cluster will verify it by.
    bool printIdentity { false };

    /// This node's identity key as the start resolved it, or absent on a node that holds
    /// none (#178).
    ///
    /// **Not a flag**: nothing a command line or a file says reaches it. `ApplyNodeIdentity`
    /// writes it, from what `ResolveNodeKey` read or minted, into every configuration this
    /// process builds -- the running one and every reload candidate -- for the reason `nodeId`
    /// is written the same way: a candidate built without it would hold a key that has
    /// CHANGED. Only the public half: the secret never enters a configuration.
    ///
    /// Among the byte-wide members rather than beside `nodeId`, because it is 33 bytes with
    /// no alignment of its own, and between two eight-aligned members it would cost seven
    /// bytes of padding the struct's budget does not have.
    std::optional<Ed25519PublicKey> identityPublicKey;

    /// Whether to cordon this machine's own worker, or lift its cordon, instead of serving.
    ///
    /// A mode rather than a serving option, like `--install-service`, and for the reason
    /// that matters most here: a cordon that a service registration or a configuration
    /// file replayed at every start would be exactly the machine that silently never
    /// comes back to the fleet -- which is why the cordon itself lives in the running
    /// worker's memory and nowhere else.
    CordonCommand cordon { CordonCommand::None };

    /// What to do to the cluster instead of serving, when anything.
    ///
    /// A mode rather than a serving option, like `--install-service`: the process
    /// asks one question of a running cluster and exits. It is deliberately NOT
    /// part of a service registration -- a worker that administered the cluster at
    /// every boot would replay one operator's decision forever.
    ClusterRequest cluster;

    /// The seed to ask for admission, instead of serving, when non-empty.
    ///
    /// A mode rather than a serving option, exactly as `cluster` above is: this
    /// process mints its identity, asks one machine to let it in, waits for a person,
    /// writes the key it is given, and exits. It opens NO surface at all while doing
    /// it, which is what keeps it out of every widening question a running node has.
    ///
    /// Deliberately NOT part of a service registration and deliberately unreachable
    /// from a configuration file: a worker that re-enrolled at every boot would ask
    /// forever after the one time it needed to.
    std::string enrollFrom;

    /// What to do to a seed's enrollment window instead of serving, when anything.
    ///
    /// The OPERATOR's half of the pair `enrollFrom` is the joiner's half of: that one
    /// is a machine asking to be let in, this one is a person at a terminal deciding
    /// whether to let it. Asked of `--scheduler`, like every other cluster verb.
    ///
    /// A mode rather than a serving option, and out of every service registration for
    /// the same reason the cluster verbs are: a node that armed an auto-approve window
    /// at every boot would replay one operator's decision forever, on the one surface
    /// where the consequence is admitting a machine nobody compared.
    EnrollCommand enroll;
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
/// all, and `--cache-memory 0` builds no memory half. Neither is visible in the flag
/// (a held port no longer is a third: it refuses the start), so sizing from it
/// reserved a quarter of RAM for a tier that was never built and offered the fleet
/// fewer slots than the machine has -- silently, since nothing reports a reservation
/// for a tier that does not exist (#167).
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
                                                       Distributed::NodeCacheCapacity const& cache,
                                                       std::uint64_t indexReserveBytes = 0);

/// The slots this node's worker offers, or nothing when it runs no worker.
///
/// **The one door from `--slots` to `OfferableSlots`, and the reason it exists is the
/// direction a mistake fails in** (#206). `OfferableSlots` derives a count when the
/// operator named none, so a `--slots=0` that reached it as "named nothing" would build
/// a FULL worker on the machine an operator had just excluded -- registered, leased and
/// compiling, with nothing anywhere saying so. Asked here, a node that runs no worker
/// has no slot count at all, and `WorkerBody` builds no worker from an absent one.
/// @param cfg The parsed configuration.
/// @param capacity What `NodeCapacityOf` made of this machine.
/// @return The count to advertise and enforce, or absent when `RunsWorker` is false.
[[nodiscard]] std::optional<std::uint32_t> WorkerSlotsOf(NodeConfig const& cfg,
                                                         Distributed::NodeCapacity const& capacity) noexcept;

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

/// A flag this binary retired, and the step that replaced it.
///
/// **Not a shim**: nothing here is accepted. A retired flag is refused exactly as any unknown one
/// is; the row only says, in that refusal, what to do instead -- because a service registration
/// replays its command line at every boot, and `unrecognised argument` is all an operator reading
/// that log would otherwise get.
struct RetiredNodeFlag
{
    std::string_view flag; ///< Its spelling on a command line, e.g. `--raft-join`.
    /// Its configuration-file key, e.g. `raft_join`. Not named `yamlKey`: that column is what
    /// `check-node-config-reference` reads as a key the shipped reference must document.
    std::string_view fileKey;
    std::string_view step; ///< What replaced it, as the operator's next step.
};

/// Where the retired flags and their steps are written up for an operator.
inline constexpr std::string_view RetiredNodeFlagsGuide =
    "docs/operations/upgrading-a-fleet.md, \"The flags that carried a cluster's shape\"";

/// Every flag this binary retired, one row each.
///
/// Consulted ONLY where a flag or a key has already been refused as unknown
/// (`ExplainRetiredNodeOption`), so a row can never make anything parse.
/// @return The rows; stable for the life of the process.
[[nodiscard]] std::span<RetiredNodeFlag const> RetiredNodeFlags() noexcept;

/// An unknown-flag or unknown-key refusal, with the retired row's step when it names a retired
/// flag or key; every other error unchanged.
///
/// The code and the field stay what the parser said, so whatever reads them reads the same
/// refusal; only the words an operator reads gain the step.
/// @param error What the parser refused.
/// @return The same refusal, explained when a retired row names it.
[[nodiscard]] ConfigError ExplainRetiredNodeOption(ConfigError error);

/// Parse this binary's command line through `NodeOptions()`, a retired flag's refusal explained
/// (`ExplainRetiredNodeOption`).
///
/// The one door `main` parses argv through, so what an operator reads for a retired flag is what
/// a test reads.
/// @param args The command line, program name already removed.
/// @param result Populated as the flags are applied.
/// @return Whether to keep going, or the first refusal.
[[nodiscard]] std::expected<ParseFlow, ConfigError> ParseNodeCommandLine(std::span<char const* const> args,
                                                                         NodeConfig& result);

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
/// @param probe Where the machine-wide state directory the registration owns is resolved.
/// @return The spec a supervisor is registered from.
[[nodiscard]] ServiceSpec MakeNodeServiceSpec(std::filesystem::path const& exePath,
                                              NodeConfig const& cfg,
                                              IConfigPathProbe const& probe);

/// The state directory a service registration of @p registration owns and secures.
///
/// The `--cluster-dir` the registration names, else the machine-wide default. ONE derivation
/// for both halves of an install: `MakeNodeServiceSpec` hands this directory over and secures it,
/// and the install reads the formation record from this directory and no other
/// (`InstallNodeService`). Two derivations disagreed: the reader took the MERGED configuration's
/// directory, so a `cluster_dir:` in the file had the install read a directory nothing secured.
/// @param registration The command-line configuration a registration is built from.
/// @param probe Where the machine-wide default is resolved.
/// @return The directory, or none where no machine-wide default resolves.
[[nodiscard]] std::optional<std::filesystem::path> RegisteredStateDirectory(NodeConfig const& registration,
                                                                            IConfigPathProbe const& probe);

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
///
/// **This is one of THREE kinds, not one of two**, and the third arrived because a flag
/// existed that neither list described: `--advertise` changes the ADDRESS a registration
/// is filed under while changing nothing about what is served, so it must re-register
/// (this list's consequence) and must not re-survey (the local list's). Named
/// `AddressReloadableFlags` below.
inline constexpr std::array<std::string_view, 2> AdvertisedReloadableFlags { "--toolchain", "--no-toolchain-discovery" };

/// The reloadable flags that change WHERE this worker is registered, not WHAT it
/// registers.
///
/// The scheduler's registry is keyed per `(fingerprint, endpoint)`, so these move the
/// second half of that key. The consequence is the expensive half of
/// `AdvertisedReloadableFlags` -- withdraw the old entry, register the new one -- WITHOUT
/// its cheap-looking half, the re-derivation: an endpoint is a string this node already
/// holds, and spending a 300 s include-tree walk to move one would be minutes of work
/// telling the fleet nothing the same beat could have told it at once.
///
/// **A list a `static_assert` reads rather than one any function walks, and that is the
/// whole of its job today.** The DECISION to re-announce is made by comparing the
/// DERIVED endpoint (`AdvertisedEndpointChange`), not by comparing this row: a save that
/// clears `--advertise` where its value equalled the `Node` surface's resolved address
/// moves this row and changes nothing the scheduler keys on, and re-registering a fleet
/// for that is the spurious direction. So the list forces the classification at the
/// table and the comparison happens where the fact lives
/// ([#1279](https://github.com/LASTRADA-Software/fastcached/issues/1279)).
///
/// Stated because the alternative reads as an omission: an entry here is not a promise
/// that something walks it, and the guard below is what keeps it honest by requiring the
/// row it names to be real, comparable and `Reloadable::Yes`.
inline constexpr std::array<std::string_view, 1> AddressReloadableFlags { "--advertise" };

/// The reloadable flags that are local wiring and reach no other machine.
///
/// A list rather than "whatever is not advertised", so that adding a reloadable row
/// forces a choice instead of defaulting into the cheap answer.
///
/// `--allow-compile-arg` is local for a reason worth stating, because it is the row
/// most likely to be read as advertised: it widens what this worker will RUN, and a
/// worker's registration says which toolchains it serves, never which arguments it
/// will accept. The scheduler has no field for this and dispatches no differently
/// because of it, so re-registering on a change would spend a 300 s include-tree
/// walk telling the fleet nothing it can act on.
///
/// `--requirepass` is the row most likely to be read as advertised for the OTHER
/// reason: it is a credential, and a credential travels. What travels is the secret
/// itself, on the next exchange, through `Node::ICredentialSource` -- and a
/// registration's CONTENT is unchanged by it. A rotation therefore needs no
/// re-survey, which matters more here than anywhere else on this list: rotating a
/// fleet's shared secret is a fleet-wide event, and making every worker walk its
/// include trees at the same moment is the one way to turn a routine rotation into
/// an incident.
///
/// `--fleet-open` is local for the reason `--allow-compile-arg` is, and the parallel is
/// exact: both decide what this worker will do for a caller, and a registration
/// describes the TOOLCHAINS it serves rather than whom it serves them to. The scheduler
/// has no field for it, so re-registering on a change would tell the fleet nothing it
/// could act on -- at the price of an include-tree walk.
inline constexpr std::array<std::string_view, 4> LocalReloadableFlags {
    "--log-level", "--allow-compile-arg", "--requirepass", "--fleet-open"
};

/// Both reloadable-flag lists, so the guards walk a derived SET rather than naming
/// each list by hand.
///
/// **This exists because the omission it prevents has already happened.** The converse
/// guard was written for `AdvertisedReloadableFlags` alone, with a comment explaining
/// precisely why it was needed, and `LocalReloadableFlags` -- three lines away -- got
/// none of it. A guard sitting next to the place it is missing is the shape that
/// survives review, because the reader sees the assert and reads it as coverage
/// ([#1027](https://github.com/LASTRADA-Software/fastcached/issues/1027)).
///
/// So the lists are enumerated ONCE, here, and the assertions in `NodeConfig.cpp` walk
/// this. A third list is covered by being added to this array; naming the two lists in
/// each assertion instead is the same duplication one level up, and is how the first
/// guard came to stand alone.
inline constexpr std::array<std::span<std::string_view const>, 3> ReloadableFlagLists {
    std::span<std::string_view const> { AdvertisedReloadableFlags },
    std::span<std::string_view const> { AddressReloadableFlags },
    std::span<std::string_view const> { LocalReloadableFlags },
};

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

/// Which of the two moments an allowlist is being applied at.
enum class AllowlistMoment : std::uint8_t
{
    Startup, ///< The set the process starts with.
    Reload,  ///< A set adopted from an accepted reload.
};

/// The line to log about the operator's compile-argument allowlist, or nothing.
///
/// **Pure, and here rather than an expression in `main.cpp`, for `RecheckDepthFor`'s
/// reason: that file is in no test target (#909), so a rule written there can only be
/// checked by reading it.** This one is #293's third acceptance clause -- *each
/// extension is logged* -- and it is a SECURITY announcement, which is the kind that
/// is wrong silently: a widening nobody was told about reads exactly like a worker
/// nobody widened.
///
/// The two moments answer differently and that is the whole reason the moment is a
/// parameter rather than something inferred from @p previous being empty:
///
/// - **Startup** says nothing for an empty set. That is the shipped state, it is what
///   almost every worker runs, and a line saying a set was not extended on every start
///   of every node is how the line that MATTERS gets filtered out.
/// - **A reload** says something whenever the set MOVED, emptying included. An
///   operator who removed the last entry needs to see that it took effect as much as
///   one who added the first -- and *removed the widening* is the half a silent
///   implementation would drop, since it is the half that looks like good news.
///
/// A reload that changed nothing else about this list says nothing, because a reload
/// is a routine event: `--log-level` alone must not narrate a set nobody touched.
///
/// @param moment Whether this is the starting set or one adopted at a reload.
/// @param previous What was in force before; empty at startup.
/// @param current What is in force now.
/// @return The message to log at WARN, or nullopt when there is nothing to say.
/// The line to log about a reload that changed who this node admits, or nothing.
///
/// **The narrowing half is what this is for, and it is the half a silent
/// implementation drops** ([#405](https://github.com/LASTRADA-Software/fastcached/issues/405)).
/// The one admission setting a reload can move is `--fleet-open`, and both directions
/// are said. Turning it ON announces itself the moment a stranger's build is served.
/// Turning it OFF has no such signal: admission succeeding is the ordinary case, so a
/// narrowing that did not take looks exactly like one that did, forever -- and every
/// caller the roster does not admit has just been refused, which no list can
/// enumerate. So that direction says so in words.
///
/// A pure function, here rather than an expression in `main.cpp`, for
/// `AllowlistAnnouncement`'s reason: that file is in no test target (#909), so a rule
/// written there can only be checked by reading it. And this is a SECURITY
/// announcement, the kind that is wrong silently.
///
/// A reload that left the flag alone says nothing, because a reload is a routine
/// event and a `--log-level` change must not narrate a policy nobody edited.
/// @param previous The admission policy that was in force.
/// @param current The one just adopted.
/// @return The message to log at WARN, or nullopt when nothing about admission moved.
[[nodiscard]] std::optional<std::string> AdmissionAnnouncement(NodeConfig const& previous, NodeConfig const& current);

[[nodiscard]] std::optional<std::string> AllowlistAnnouncement(AllowlistMoment moment,
                                                               std::span<std::string const> previous,
                                                               std::span<std::string const> current);

/// The line to log when a node that works for other machines opens no admin surface.
///
/// `--admin-listen` is off unless asked for, and its own help calls `/healthz` *"the
/// liveness probe this worker otherwise has none of"*. A node without one caches,
/// compiles and registers exactly as configured while being invisible to anything not
/// on its own machine -- so over a fleet that is opt-in monitoring whose failure mode
/// is silence
/// ([#1304](https://github.com/LASTRADA-Software/fastcached/issues/1304)).
///
/// **A remark and NOT a flipped default**, which is the ticket's own position and the
/// right one: binding a port nobody asked for is the decision this binary refuses
/// everywhere. A default `--admin-listen` would also reach the dashboard-credential
/// row in `StartupPolicyRejection`, so it would either serve the fleet page
/// unauthenticated or make `--dashboard-token-file` mandatory on every install.
///
/// **The fleet question is `AdmitsRemotePeers`, an EXISTING spelling rather than a
/// fourth one.** *Is this a fleet participant* already has three spellings that
/// deliberately disagree, and a value decided by one
/// predicate and judged by another is how they come apart. What this needs is *does
/// this node's policy admit a machine that is not this one*, which is that function's
/// exact subject. It is also what keeps the remark off the single-machine install:
/// `--scheduler` is required of EVERY shape -- a scheduler registers with itself --
/// so naming one says nothing about a fleet, and a predicate reading it would fire on
/// every node there is. And a key route alone is no evidence either: every node naming
/// `--scheduler` keeps a state directory, so it could hold a roster on the single-machine
/// install too. So it asks `CompileVerbsReachOtherMachines`, which adds that the node port
/// faces the network. That is no longer the difference on its own, since every node binds
/// the wildcard by default; what makes a node a fleet's is its members, and a node running
/// consensus knows them from its formation record (`RosterPresence::Formed`): a solitary
/// node serves its own machine only, a pending, learner or voter node serves a fleet. Any
/// other node is asked with `RosterPresence::Unknown`: this is said at startup, before any
/// roster has been read, and a remark erring towards being said costs one line where erring
/// the other way costs the probe an operator never set up.
///
/// Whether the surface is on is asked of its own ROW, for the reason the
/// dashboard-credential rule gives: a second reader of that question eventually judges
/// an address the surface no longer binds.
///
/// Pure, and here rather than an expression in `main.cpp`, for
/// `AllowlistAnnouncement`'s reason: that file is in no test target (#909). Unlike its
/// two siblings this is INFO and not WARN -- nothing was widened and nothing is wrong,
/// and warning about a port an operator deliberately did not ask for is how the
/// warnings that matter get filtered out.
/// @param cfg The configuration this node is starting with.
/// @return The message to log at INFO, or nullopt when there is nothing to say.
[[nodiscard]] std::optional<std::string> ObservabilityAnnouncement(NodeConfig const& cfg);

/// What a bare `--listen-raft` binds.
///
/// The wildcard, like a scheduling node's `--listen-node` and unlike a worker's:
/// peers are on other machines by definition, so a loopback default would be one that
/// silently cannot work. Named because two places have to agree on it -- the policy row that
/// refuses an unusable `--listen-raft` and the tier that binds it -- and a default
/// they disagreed about is a row accepting what the tier then refuses.
inline constexpr std::string_view RaftListenDefaultHost = "0.0.0.0";

/// What a bare `--listen-node` binds: the wildcard, on every node.
///
/// It was loopback on a node that did not schedule and the wildcard on one that did, until
/// the zero-config defaults made every node a fleet participant: a worker bound to loopback
/// advertises an address no other machine can dial. The cache verbs answer this machine alone
/// whatever the bind (#287), and every other verb refuses a caller that is not a member.
inline constexpr std::string_view NodeSurfaceDefaultHost = "0.0.0.0";

/// Whether @p host names every interface rather than a machine.
///
/// The two spellings of "every interface", plus the empty host -- which reaches `getaddrinfo`
/// as nullptr under AI_PASSIVE and is therefore the wildcard as well, the case
/// `--listen-node=:6674` is refused for. Brackets are the caller's to strip (`HostOfEndpoint`
/// does), so `[::]` arrives here as `::`.
/// @param host A host, unbracketed.
/// @return True for a wildcard.
[[nodiscard]] constexpr bool IsWildcardHost(std::string_view host) noexcept
{
    return host.empty() || host == "0.0.0.0" || host == "::";
}

/// The endpoint this node tells other machines to dial.
///
/// **One derivation, because three consumers must agree or the fleet breaks in a way
/// none of them can see.** What a lease's MAC covers is this endpoint, so the property
/// the compile surface's needing no connection credential rests on is:
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
///
/// `--advertise` when given; otherwise the node surface, with a WILDCARD host replaced by
/// this machine's fully qualified name once the start has resolved it (`hostNames`).
/// @param cfg What the operator asked for.
/// @return The advertised `host:port`.
[[nodiscard]] std::string AdvertisedEndpoint(NodeConfig const& cfg);

/// Whether the advertised endpoint is still waiting for this machine's name.
///
/// True for a node that names no `--advertise`, binds the wildcard, and has not resolved its
/// names yet -- a parse, an install, `--print-surfaces`. `AdvertisedEndpoint` then answers the
/// wildcard it binds, which the start will replace; a rule refusing an undialable advertised
/// address asks this first, so it refuses a RESOLVED answer and never the placeholder.
/// @param cfg What the operator asked for.
/// @return True while the name is awaited.
[[nodiscard]] bool AdvertisedNameAwaited(NodeConfig const& cfg);

/// Whether the advertised endpoint is WITHHELD: this node names no `--advertise`, binds the
/// wildcard, and this machine's name reaches only itself (`NodeHostNames::withheld`).
/// `AdvertisedEndpoint` then offers no peer that name: loopback to a scheduler on this machine,
/// which reaches it there, and nothing at all to one elsewhere, which is refused
/// (`WorkerNameReachesOnlyThisMachineRefusal`).
/// @param cfg The configuration, with its host names applied.
/// @return True while there is no name to advertise.
[[nodiscard]] bool AdvertisedNameWithheld(NodeConfig const& cfg);

/// Where the readiness line says this node listens: the node surface as it binds, or the socket a
/// supervisor handed it, which no setting describes.
/// @param cfg The resolved configuration.
/// @param socketHandedOver Whether the node surface was adopted rather than bound.
/// @return The text, never empty.
[[nodiscard]] std::string DescribeListeningEndpoint(NodeConfig const& cfg, bool socketHandedOver);

/// What the readiness line says this node advertises: the endpoint, or why there is none --
/// `withheld (<name>)` for a name that reaches only this machine, never an empty field.
/// @param cfg The resolved configuration, its host names applied.
/// @return The text, never empty.
[[nodiscard]] std::string DescribeAdvertisedEndpoint(NodeConfig const& cfg);

/// Whether some `--scheduler` this worker registers with names a host that is not this machine.
///
/// ANY value, never the first (#1310): the heartbeat falls back through every one of them. A
/// value that is not `host:port` is not counted; `ParseDialEndpoint` answers its shape.
/// @param cfg The parsed configuration.
/// @return True when a scheduler on another machine would be handed this worker's address.
[[nodiscard]] bool SchedulerIsRemote(NodeConfig const& cfg);

/// Whether the consensus address is WITHHELD: this node names no `--raft-self`, and this
/// machine's name reaches only itself. A node whose
/// `--listen-raft` is defaulted then runs no consensus -- the raft surface does not resolve --
/// and one that asked for consensus is refused by name.
/// @param cfg The configuration, with its host names applied.
/// @return True while there is no address to offer peers.
[[nodiscard]] bool ConsensusNameWithheld(NodeConfig const& cfg);

/// One setting an operator chooses that needs OTHER machines to reach this node's consensus.
///
/// A row per setting, because every one of them meets a downstream refusal if consensus stands
/// down on a withheld name -- `--discovery` "needs --listen-raft" -- which sends the operator to
/// add a flag and only then tells them the cause.
/// So the named refusal (`ConsensusNameReachesOnlyThisMachineRefusal`) keys on the table, and
/// the refusal's own text names every row's `setting`, which a test holds it to.
struct ConsensusPeerAsk
{
    std::string_view setting;            ///< How the refusal names it.
    bool (*asks)(NodeConfig const& cfg); ///< Whether this configuration chose it.
};

/// Every setting that needs other machines to reach this node's consensus.
/// @return The rows.
[[nodiscard]] std::span<ConsensusPeerAsk const> ConsensusPeerAsks() noexcept;

/// Whether @p cfg chose any setting of `ConsensusPeerAsks()`.
/// @param cfg The configuration, its formation applied.
/// @return True when some row asks.
[[nodiscard]] bool AsksPeersToReachConsensus(NodeConfig const& cfg);

/// Whether peers are told to dial this node's consensus port at a host off this machine while
/// `--listen-raft` binds loopback, so no peer's dial is ever answered.
/// @param cfg The configuration.
/// @return True when the stated consensus address contradicts a loopback bind.
[[nodiscard]] bool ConsensusPastALoopbackBind(NodeConfig const& cfg);

/// Why a loopback consensus bind with an address off this machine is refused.
inline constexpr std::string_view ConsensusPastALoopbackBindRefusal =
    "--listen-raft binds loopback, so only this machine can reach this node's consensus port, and peers are told to "
    "dial it at a host off this machine (--raft-self, or this machine's name): every one of them would go "
    "unanswered, and be counted in a quorum it is absent from. Bind --listen-raft to an address they reach, or the "
    "wildcard, or say --raft-self=127.0.0.1 for a cluster that never leaves this machine";

/// Whether a `--discovery` the operator TYPED would announce a consensus address every peer
/// resolves to itself (`Cluster::AnnouncesOnlyThisMachine`) -- a loopback bind, or a
/// `--raft-self` naming loopback or `localhost`.
///
/// A cluster that never leaves this machine is a legitimate configuration, and a DEFAULTED
/// discovery stands down on it (`StartDiscoveryOrExplain`); a typed one asks for exactly what
/// such a node must never do, and is refused here, where an install is judged too.
/// @param cfg The configuration.
/// @return True when the typed discovery would beacon an address that reaches only its dialler.
[[nodiscard]] bool DiscoveryAnnouncesOnlyThisMachine(NodeConfig const& cfg);

/// Why a typed `--discovery` on a node whose consensus address reaches only this machine is
/// refused. One text, spent by the startup table and by the discovery tier alike.
inline constexpr std::string_view DiscoveryAnnouncesOnlyThisMachineRefusal =
    "--discovery would announce this node's consensus address, which reaches only this machine: every peer "
    "resolves it to itself, so a beacon would send each of them to dial itself. Name an address other machines "
    "reach -- bind --listen-raft to it or to the wildcard, and correct --raft-self if you typed one -- or drop "
    "--discovery for a cluster that never leaves this machine";

/// Why a worker whose advertised name would reach only this machine is refused -- only when it
/// registers with a scheduler on ANOTHER machine. One on this machine reaches it at loopback.
inline constexpr std::string_view WorkerNameReachesOnlyThisMachineRefusal =
    "this worker names no --advertise and binds the wildcard, registers with a scheduler on another machine, and this "
    "machine's name reaches only itself (localhost, a name under .localhost, or a loopback address): a client leased "
    "to it would dial its OWN machine. Name --advertise with an address or a name other machines resolve, or give "
    "this machine a real host name";

/// Why a node that asked for consensus, and whose name would reach only this machine, is refused.
inline constexpr std::string_view ConsensusNameReachesOnlyThisMachineRefusal =
    "--listen-raft, --discovery or this node's recorded mode (a voter, which the fleet's other members dial) makes "
    "this node a consensus member other machines reach, which must name the "
    "address its peers dial, and this "
    "machine's name reaches only itself (localhost, a name under .localhost, or a loopback address): "
    "a peer told to dial it would dial ITSELF. Give --raft-self=<a host other machines resolve> (127.0.0.1 when no "
    "other machine ever will)";

/// The host a node dials ITSELF at: its own worker's scheduler, and the address a worker whose
/// name is withheld advertises to a scheduler on this machine.
///
/// Loopback whatever the node port binds: the wildcard answers on loopback, and a loopback bind
/// is loopback. A name here would be the one thing that could fail to resolve.
inline constexpr std::string_view ThisMachineLoopbackHost = "127.0.0.1";

/// What a bare `--admin-listen` binds.
///
/// Loopback, and it is what the dashboard's credential rule turns on: reaching loopback
/// already means being on the machine, so a bare port needs no token while a bind an
/// operator deliberately exposed does.
inline constexpr std::string_view AdminListenDefaultHost = "127.0.0.1";

/// How an operator states the address peers dial a consensus node at: the remedy for a node that
/// states none, worded ONCE.
///
/// Two texts tell an operator this rule -- the startup refusal below and `--print-surfaces`'
/// `NOT STATED` line -- and they drifted once: the refusal named one way out while the worksheet
/// named another, which sends an operator to a flag that does not fit. Since the zero-config
/// defaults there is one way, `--raft-self` (the host another member dials; the port is
/// `--listen-raft`'s), and both texts carry this phrase, with a `static_assert` beside each
/// keeping them from drifting apart again.
inline constexpr std::string_view ConsensusDialRemedy = "give --raft-self";

/// Why a node running consensus that names no address its peers dial it at cannot work.
///
/// A named constant rather than prose written into the policy row it fills, because
/// `ConsensusTier::Start` answers with **this** string. The invariant is decided by
/// the startup table -- which is what makes an operator hear it while they are
/// watching -- and the tier is that same answer arriving at the boot of a
/// `NodeConfig` nobody parsed from an argv. Two spellings of one rule is what this
/// codebase's table idiom exists to prevent.
///
/// It was `NodeIdNamesNoPeerRefusal` and named `--node-id` as the thing that turns
/// consensus on, which stopped being true at
/// [#1022](https://github.com/LASTRADA-Software/fastcached/issues/1022). A message
/// naming the wrong flag sends an operator to add the flag they already have; the
/// rule itself did not move, only the flag that reaches it.
///
/// It ends without a full stop, alone among the rows: `main.cpp` prints a tier's
/// refusal as `"{}; refusing to start"`, and the tier's other messages are written
/// as fragments for exactly that. One message serving two callers has to suit the
/// one that appends.
inline constexpr std::string_view ConsensusNamesNoDialAddressRefusal =
    "this node runs consensus and names no address its peers dial it at: give --raft-self=<host>, the host another "
    "member would dial (127.0.0.1 when none ever will); the port is --listen-raft's, and consensus cannot start "
    "without one";

static_assert(ConsensusNamesNoDialAddressRefusal.contains(ConsensusDialRemedy),
              "the startup refusal names the way to state the dial address, in the worksheet's words");

/// Why a node that runs no consensus may not name `--discovery`.
///
/// One sentence for the startup table and for the discovery tier's belt, which spelled it
/// separately and named `--node-id` as the switch long after #1022 made it `--listen-raft` --
/// the flag an operator would then go and add without effect.
inline constexpr std::string_view DiscoveryNeedsConsensusRefusal =
    "--discovery needs --listen-raft: discovery finds peers for a CLUSTER, and without a consensus port this node "
    "is not in one. It would broadcast, be answered, prove its identity and have nowhere to put the answer.";

/// Whether this build can terminate TLS: a property of the BUILD, which the startup table asks
/// beside the configuration so a line naming TLS material on a build without it is refused where
/// an operator is watching -- `--print-surfaces`, `--install-service` -- and not only at boot.
#if defined(FC_TLS_ENABLED)
inline constexpr bool BuildServesTls = true;
#else
inline constexpr bool BuildServesTls = false;
#endif

/// Why TLS material is refused on a build that cannot serve it.
///
/// Refused rather than warned about, and the daemon answers the same way: a node that started in
/// the clear after being told to serve TLS is one an operator believes is encrypted.
inline constexpr std::string_view TlsUnavailableRefusal =
    "--tls-self-signed or --tls-cert was given, and this build has no TLS support (rebuild with "
    "-DFASTCACHED_ENABLE_TLS=ON): the admin surface would be served in the clear while an operator believed it "
    "was encrypted.";

/// Why a worker that could verify no lease is refused, when other machines can reach it (#178).
///
/// Answered by the startup check of the state directory (`NodeRoster::Build`), the one moment the
/// answer can be known: a worker names a scheduler, so it keeps a state directory, and only the
/// directory says whether it holds a roster. There is no configuration's half any more: every
/// node has a state directory to keep one in.
inline constexpr std::string_view RosterlessWorkerRefusal =
    "a node that admits peers on other machines checks the lease a client presents to its worker, against a roster "
    "its cluster's voters certify -- and this node runs no consensus, names no --voter-key and holds no roster, so "
    "it could verify nothing: it would compile for anybody who can reach its port, and report nothing wrong while "
    "doing it. Name the voters' keys with --voter-key (each voter's --print-identity prints its key). A node "
    "admitting only its own machine needs none, because a process on this host already has this host's compiler";

/// Why `--voter-key` on a consensus node is refused (#178).
inline constexpr std::string_view VoterKeyOnConsensusNodeRefusal =
    "--voter-key names the voters a worker that runs NO consensus trusts before it holds a roster; this node runs "
    "consensus (--listen-raft), so it verifies every grant against the state it applies and would never read "
    "these keys. Drop --voter-key";

/// Why a node with no worker, no scheduler, no consensus and no cache tier is refused.
inline constexpr std::string_view NodeRunsNothingRefusal =
    "--slots=0 runs no worker, and this node runs no scheduler, no consensus and no cache tier either: it would "
    "start and serve nobody. Open consensus (--listen-raft) so its mode can serve the scheduler, add a cache tier, "
    "or give --slots a count to run a worker.";

/// Whether this node runs consensus, and therefore whether anything will ever tell
/// its scheduler what term it is in.
///
/// **A named predicate because two tiers have to agree about it, and they are built
/// apart** ([#613](https://github.com/LASTRADA-Software/fastcached/issues/613)).
/// `StartConsensusOrExplain` answers "is there a cluster to start" and
/// `SchedulerTier` answers "will somebody publish my role"; those are the same
/// question, and while each spelled `cfg.nodeId.empty()` for itself they were two
/// authors of one rule.
///
/// The disagreement is not hypothetical: the scheduler assumed nobody would, published
/// standalone leadership at term 0, and then consensus published a real term over the
/// top of it -- leaving a window in which the surface answered `Lease` as a leader that
/// had not been elected. Since #178 there is no standalone leadership at all -- a
/// scheduler without consensus is refused at startup -- and this predicate is what that
/// refusal asks.
///
/// **A mode is the STATE held in the cluster dir** (`NodeFormation.hpp`). The switch was
/// `--node-id` until [#1022](https://github.com/LASTRADA-Software/fastcached/issues/1022):
/// while the id's ABSENCE carried the mode, the id could never be given a default, because
/// any default makes `nodeId.empty()` false forever and the one-machine deployment would be
/// refused at every boot. #1022 moved it to the port; zero-config formation moves it again,
/// to the record, for the same reason one level up -- a port that has a DEFAULT can no longer
/// say whether this machine runs consensus, and a record written by the node's own
/// transitions can. Every mode runs consensus, so this answers whether a record shaped this
/// configuration at all, and then two orthogonal facts folded in this one place:
///
/// - a mode that DIALS IN (a learner) runs consensus with no port of its own, and needs no
///   name other machines resolve, since nobody dials it;
/// - a mode that OPENS the port runs consensus exactly when the Raft surface row resolves
///   it. That row is where an explicitly empty `--listen-raft=` and the localhost stand-down
///   (`ConsensusNameWithheld`, when the port was never typed) close it, so this and
///   `--print-surfaces` cannot disagree about whether the port is served.
///
/// Not a column per mode for the stand-down: the name is a fact about the MACHINE, and every
/// mode that opens a port is subject to it, so a column would be the same value written on
/// every row that listens.
///
/// A value that is not an address resolves to nothing and is refused by the grammar walk at
/// the top of `StartupPolicyRejection`, which runs before any rule that consults this and
/// before any tier is built.
/// @param cfg The parsed configuration, with its formation applied.
/// @return True when a consensus driver will run and report a role.
[[nodiscard]] bool RunsConsensus(NodeConfig const& cfg) noexcept;

/// What a caller of a fleet predicate knows about the roster this node verifies keys against.
///
/// PRIVATE: never transmitted and never persisted. Three answers because the configuration
/// cannot see a roster a state directory kept: only `NodeRoster::Build` and what runs after it
/// know `Held` or `Absent`, and a question asked before that is `Unknown`. A fourth for a node
/// running consensus, whose roster is the state it applies and whose members its formation
/// record already names: `Formed` asks the record, where `Unknown` would count the fleet the
/// node MAY found later -- the fail-closed reading a guard wants and a statement about the
/// present must not make.
enum class RosterPresence : std::uint8_t
{
    Held,    ///< A roster is held, so a proof or a ticket can admit another machine.
    Absent,  ///< No roster is held, so no key can admit anybody.
    Unknown, ///< Asked before any roster was read; the configuration decides (`AdmitsByKey`).
    Formed,  ///< Consensus's own roster: it admits exactly the members the formation record's mode names.
};

/// Whether a key -- a proof or a ticket -- could ever admit a machine that is not this one.
///
/// A key admits only against a roster, and three things in a configuration may give this node
/// one: consensus, whose roster is the state it applies; `--voter-key`, the anchors a roster is
/// adopted against; and the state directory, which may hold a roster an earlier run adopted --
/// the one `ChosenStateDirectory` names, typed or the default the start resolved, since that is
/// where `NodeRoster::Build` reads a kept roster back. It answers "may", never "does": a
/// directory that turns out empty is `NodeRoster::Build`'s finding, and until that runs the
/// fail-closed reading is that a key route is live.
/// @param cfg The parsed configuration.
/// @return Whether any clause of the configuration may give this node a roster.
[[nodiscard]] bool AdmitsByKey(NodeConfig const& cfg);

/// Whether this node runs a compile worker: surveys its toolchains, claims a scratch
/// root, serves the compile verbs and registers with a scheduler.
///
/// **`--slots=0` is the one configuration that says no** (#206). A machine that should
/// only schedule -- a small always-on box, a VM whose cores belong to something else --
/// used to be given a toolchain identity no client could match, because every node was
/// a worker and there was no way to offer the fleet nothing. That trick still
/// registered, and under a fingerprint a client DID compute it took the work the
/// operator believed they had excluded.
///
/// A mode on a value rather than on a port, because the worker has had no port of its
/// own since #290 -- the compile verbs share the node's `0xFC` listener -- and never on
/// the absence of a name (#1022): neither an empty `--scheduler` nor an empty toolchain
/// set may carry it, since both are ordinary states a worker passes through.
///
/// Such a node registers NOTHING, so it is never picked and the registry never sees a
/// zero slot count. It may still name `--scheduler`, which then tells only the cluster
/// and enrollment commands run on this machine where to ask. It appears under a cluster's members when it runs consensus,
/// and not among the fleet page's machines, which are built from worker registrations; its history is not handed to a leader
/// either, because that rides the worker heartbeat.
/// @param cfg The parsed configuration.
/// @return True unless the operator asked for zero slots.
[[nodiscard]] bool RunsWorker(NodeConfig const& cfg) noexcept;

/// The worker, as the component column of `NodeOptions()` and the scope column of the
/// startup, install and reload rules name it.
inline constexpr OptionComponent<NodeConfig> WorkerComponent {
    .name = "worker",
    .runs = &RunsWorker,
    .absentBecause = "--slots=0 runs none",
    .remedy = "give --slots a count to run one",
};

/// What a node is told when it names a setting only a component it does not run reads.
///
/// ONE sentence for every such row, generated from the row and its component, because
/// the rule is one rule: the setting would be accepted and reach nothing (#206).
/// @param spec A row of `NodeOptions()` whose `component` is not null.
/// @return The refusal.
[[nodiscard]] std::string UnrunComponentRefusal(OptionSpec<NodeConfig> const& spec);

/// Whether this configuration asks for a cache tier: a port to serve it on, and memory
/// or a directory to keep objects in.
///
/// The configuration half of what `StartCacheTierOrExplain` builds. A tier can still
/// fail to start -- a directory that will not open refuses the node -- so this is what
/// the startup table may ask before any tier exists, never a substitute for the
/// pointer that says what actually started.
/// @param cfg The parsed configuration.
/// @return True when a cache tier will be built unless starting it fails.
[[nodiscard]] bool ConfiguresCacheTier(NodeConfig const& cfg) noexcept;

/// Where the shared tier's store lives: `<state directory>/shared-cache/objects.cow`.
///
/// A directory of its own inside the node's state directory (`NodeStateDirectory`: the
/// `--cluster-dir` typed, else the default resolved at startup), never `--cache-dir`: the
/// private tier's store is claimed exclusively by this process already, and the two tiers
/// answer to different verbs and different callers. `DiskStoreFileName` is the file's
/// name in both, so a test measuring either store spells it once.
///
/// **Never `cfg.clusterDir` alone**: that is only the TYPED directory, so a zero-config
/// node -- which names none -- would open its shared tier at a path relative to the
/// working directory.
/// @param cfg The configuration; its state directory must be resolved.
/// @return The store file.
[[nodiscard]] std::filesystem::path SharedCacheStorePath(NodeConfig const& cfg);

/// The member endpoint `--raft-self` -- or, when it is not given, this machine's resolved
/// fully qualified name -- and `--listen-raft` name between them.
///
/// **One author for a value two places need**, which is the whole reason it is a
/// function: `ConsensusDialAddressOf` answers from it, and `BootstrapMembersOf` builds
/// this node's own member entry out of that answer. Written twice, those two would
/// disagree about IPv6 bracketing, and the address a node starts under would not be the
/// one it reports.
/// @param cfg The parsed configuration.
/// @return `host:port`, or empty when either half is missing -- a node running no consensus,
///         or one naming no `--raft-self` before its names are resolved.
[[nodiscard]] std::string RaftSelfEndpoint(NodeConfig const& cfg);

/// Why `ConsensusDialAddressOf` has no address to give.
///
/// Private to this process: nothing transmits or persists it -- the wire carries the
/// endpoint as an optional, and a gap is simply not sent.
enum class ConsensusDialGap : std::uint8_t
{
    NoConsensus,      ///< `--listen-raft` does not resolve, so there is nothing to dial.
    Unstated,         ///< Consensus runs, and the node names itself neither way.
    AwaitingHostName, ///< Consensus runs, and the host name it falls back to is resolved at startup.
    DialsIn,          ///< Consensus runs in a mode that binds no Raft port: it dials, and nobody dials it.
    Last,             ///< The count, for `EnumTable`.
};

/// The consensus address this node tells the cluster to dial it at (#1328).
///
/// **A different question from the `raft` surface row, and the difference is the whole
/// value.** The row is what this node BINDS, which is routinely the wildcard; this is
/// what a peer DIALS, which the leader records when the node is admitted and echoes in
/// `--cluster-admit`'s receipt (#1296). An operator comparing that receipt against the
/// bound address compares the wrong string, so this is printed separately and labelled
/// as the receipt labels it.
///
/// `RaftSelfEndpoint`: `--raft-self`'s host, or this machine's name, on the raft port -- the
/// pair consensus runs under and `EnrollClaim` sends. This reports it and refuses nothing.
///
/// **Absences, not an empty string**: a node running no consensus has no dial address
/// (absent is not zero), a consensus node naming itself neither way has one nobody stated
/// -- which the startup table refuses, and which `--print-surfaces` still has to be able
/// to print -- and a mode whose row closes the Raft port (a learner) needs none, because it
/// dials the fleet's voters and the leader answers it on that session. That last one is
/// never refused for lacking an address: nobody would ever dial the one it named.
///
/// The one derivation of this precedence: `ApplyNodeIdentity` builds the member entry
/// from it, so the address a node runs under and the one it prints cannot disagree.
/// @param cfg The parsed configuration, with or without its identity applied.
/// @return The `host:port` peers dial, or which of the two absences it is.
[[nodiscard]] std::expected<std::string, ConsensusDialGap> ConsensusDialAddressOf(NodeConfig const& cfg);

/// Who this node admits, as one line an operator reads at startup.
///
/// One spelling for two callers -- the scheduler tier's ready line and the worker's
/// -- because the policy is the **node's** rather than any one surface's, and a
/// phrase each of them built separately is one that drifts. Read off the
/// configuration rather than off the oracle: the oracle is shared by three surfaces
/// and no longer any one tier's to inspect.
///
/// It says "this machine" out loud, because that admission is unconditional. It names
/// the key routes on every node rather than only where a roster is visible in the
/// configuration, because a state directory can keep a roster no configuration can
/// see, and a line claiming "this machine only" on such a node would be a confident
/// wrong answer. `--fleet-open` is the one admission a flag states, so it is the one
/// tail the sentence grows.
/// @param cfg The parsed configuration.
/// @return A phrase naming who this node admits.
[[nodiscard]] std::string AdmissionSummary(NodeConfig const& cfg);

/// What the readiness line says about this node's worker.
///
/// A node running no worker says exactly that, rather than "0 slot(s) ... identifying 0
/// toolchain(s)", which describes a worker that serves nothing: a different machine from
/// the one the operator configured (#206). The worker's words are a fixture's input --
/// `dist-compile-e2e.sh` reads a derived slot count out of them -- so they are pinned by
/// `NodeConfig_test` rather than left to the line that prints them.
/// @param cfg The parsed configuration; its node class names the worker.
/// @param workerSlots What `WorkerSlotsOf` answered, absent on a node running no worker.
/// @param toolchains How many toolchains the worker is bringing up.
/// @return The phrase.
[[nodiscard]] std::string WorkerReadinessPhrase(NodeConfig const& cfg,
                                                std::optional<std::uint32_t> workerSlots,
                                                std::size_t toolchains);

/// Whether a machine that is not this one could reach this node's compile verbs, as the
/// configuration says: a door onto them answers somewhere but loopback, and the admission
/// policy lets a remote peer through it.
///
/// **The one question that decides whether a lease must be checked** (#282, #178), asked by
/// the startup table of a configuration and by `NodeRoster::Build` of a state directory -- two
/// moments, one predicate. It reads `--bind`, so under socket activation it describes nothing,
/// and `MakeWorkerLeaseValidator` keeps the backstop for that case.
///
/// **The key routes count**, through `AdmitsRemotePeers`: a roster admits machines by
/// proof and by ticket, so a network-facing worker holding one reaches other machines
/// although no flag names any of them. Asked with `Absent` it answers for a node known
/// to hold no roster; with `Unknown`, for a configuration asked before any roster was
/// read, and then a key route counts as live; with `Formed`, for a node running consensus,
/// whose roster admits the members its formation record names.
/// @param cfg The parsed configuration.
/// @param roster What the caller knows about the roster this node verifies keys against.
/// @return True when another machine could present a lease here.
[[nodiscard]] bool CompileVerbsReachOtherMachines(NodeConfig const& cfg, RosterPresence roster);

/// One path-valued worker flag whose file holds a secret.
///
/// The shared row type, spelled for this binary. It was a struct of its own until
/// [#864](https://github.com/LASTRADA-Software/fastcached/issues/864) gave the daemon
/// the same two tables and the two definitions differed only in a type -- so
/// `Config/SecretProvenance.hpp` holds the one row and both binaries name it.
using NodeSecretFile = SecretFileRow<NodeConfig, std::filesystem::path>;

/// One path-valued worker flag whose file is deliberately NOT a secret.
///
/// `PublicPathFlag` unchanged -- the daemon's rows are the same two fields for the
/// same reason, and `ctest -R "every path-valued flag is classified"` requires every
/// `=<path>` row of `NodeOptions()` to appear in exactly one of the two tables.
using NodePublicPathFlag = PublicPathFlag;

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
/// owner. The files `NodeSecretFileTable()` names are not gated at all --
/// the path is not the secret and the file is, so a world-readable key file is
/// exposed however its path was named
/// ([#752](https://github.com/LASTRADA-Software/fastcached/issues/752)).
///
/// **A named file is reported whether or not a tier reads it**, and that is the
/// lesson the retired `--cluster-key-file` refusal in `StartupPolicyRejection` was
/// narrowed twice to learn: whether a surface exists is not a fact about the
/// configuration, so a rule whose premise is "somebody will read this" cannot state its
/// premise without guessing. The file holds a secret on disk either way.
///
/// @param cfg The merged configuration.
/// @param configFile The configuration file that was actually read, or empty when
///        none was. The RESOLVED path, since a discovered file is named by no flag.
/// @param secretNamedOnCommandLine Whether argv supplied `--requirepass`.
/// @return The paths, config file first when it qualifies; empties are kept out.
[[nodiscard]] std::vector<std::filesystem::path> NodeSecretFiles(NodeConfig const& cfg,
                                                                 std::filesystem::path const& configFile,
                                                                 bool secretNamedOnCommandLine);

/// `NodeSecretFiles`, each file with the hint its row names: what a warning renders. The
/// identity key is told the OWNER-ONLY remedy, since the node minted it and nothing else reads it.
/// @param cfg The merged configuration.
/// @param configFile The configuration file that was actually read, or empty.
/// @param secretNamedOnCommandLine Whether argv supplied `--requirepass`.
/// @return The subjects, in `NodeSecretFiles`' order.
[[nodiscard]] std::vector<SecretFileSubject> NodeSecretFileSubjects(NodeConfig const& cfg,
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

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Platform/HostNaming.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/// @file NodeDefaults.hpp
/// What a node with no flags does: the ports it opens, where it keeps its state, and what it
/// calls itself. Each is the value of the row it defaults, never a special case beside it, so
/// "did the operator type it" stays provenance and a service registration carries none of them.
namespace FastCache::Node
{

struct NodeConfig;
class NodeConditions;

/// The node surface's port: the cache and scheduler verbs, and where `--fleet-seed` dials.
inline constexpr std::uint16_t DefaultNodePort = 6674;

/// The consensus surface's port.
inline constexpr std::uint16_t DefaultRaftPort = 6680;

/// The UDP port discovery beacons are sent to and heard on.
inline constexpr std::uint16_t DefaultDiscoveryPort = 6681;

/// `--listen-node`'s default: the wildcard, because every node is a fleet participant and one
/// bound to loopback would advertise an address no other machine can dial.
inline constexpr std::string_view DefaultNodeListen = "0.0.0.0:6674";

/// `--listen-raft`'s default. A bare port binds the wildcard.
inline constexpr std::string_view DefaultRaftListen = "6680";

/// `--discovery`'s default, whose PORT is the beacon port and whose HOST is used only when an operator
/// names this value: a node that names none beacons to every up interface's DIRECTED broadcast on this
/// port (`Cluster::DirectedBroadcastDestinations`), because the limited broadcast leaves by one
/// interface the stack guesses -- measured on a Windows host, a VPN-type adapter rather than the LAN.
/// Which of the two a node does is decided by provenance (`BeaconDestinationsFor`), never by
/// comparing against this value.
inline constexpr std::string_view DefaultDiscoveryAddress = "255.255.255.255:6681";

/// The last component of every platform's default state directory.
inline constexpr std::string_view NodeStateDirectoryName = "fastcache-node";

/// The variable a service manager hands a unit's state directory over in: systemd's, for
/// `StateDirectory=`, and the one a launchd registration of this node states.
inline constexpr std::string_view ServiceStateDirectoryVariable = "STATE_DIRECTORY";

/// Why a node keeps its state where it does.
///
/// **PRIVATE: persisted and transmitted nowhere.** What travels is `DescribeStateDirectoryOrigin`'s
/// words, so the values bind nothing; the explicit `= 0` is `EnumTable`'s anchor.
enum class StateDirectoryOrigin : std::uint8_t
{
    Named = 0,      ///< `--cluster-dir`, typed.
    ServiceManager, ///< Handed over by the service manager (`STATE_DIRECTORY`).
    MachineWide,    ///< The platform's machine-wide directory, because this process is privileged.
    PerUser,        ///< The account's own state directory, because this process is not.
    Last,           ///< Not an origin.
};

/// Where a node keeps its state, and why there.
struct NodeStateDirectoryChoice
{
    std::filesystem::path path;  ///< The directory.
    StateDirectoryOrigin origin; ///< Why this one.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(NodeStateDirectoryChoice const&, NodeStateDirectoryChoice const&) = default;
};

/// Why a node keeps its state where it does, as an operator reads it.
/// @param origin The origin.
/// @return Its words, for `--print-surfaces` and `--node-status`.
[[nodiscard]] std::string_view DescribeStateDirectoryOrigin(StateDirectoryOrigin origin) noexcept;

/// The state directory a node uses when `--cluster-dir` is not given.
///
/// First the directory a service manager hands over (`STATE_DIRECTORY`, POSIX only, and only an
/// entry named `fastcache-node`); then, for a PRIVILEGED process, the machine-wide one --
/// `%ProgramData%\fastcache-node`, `/Library/Application Support/fastcache-node` or
/// `/var/lib/fastcache-node`; otherwise the account's own -- `%LOCALAPPDATA%\fastcache-node`, or
/// `$XDG_STATE_HOME/fastcache-node` falling back to `~/.local/state/fastcache-node`.
///
/// **Per-user when unprivileged, because the machine-wide directory is the service's**: a node
/// a developer starts by hand cannot create it, and one that could would share an identity with
/// the machine's service -- two processes proving one key, which the cluster reads as a clone.
/// So two identities on one machine are expected, and the origin is reported beside the path on
/// `--print-surfaces` and `--node-status` so an operator can tell which is which.
/// @param probe Where the environment and the privilege are read.
/// @return The directory and why, or nothing when every applicable row's base is unset or empty.
///         Never a relative path: for a service that would be relative to `System32` or `/`,
///         and the node's identity would be minted wherever that happened to be.
[[nodiscard]] std::optional<NodeStateDirectoryChoice> DefaultNodeClusterDirectory(IConfigPathProbe const& probe);

/// The machine-wide state directory, whatever the privilege of the process asking.
///
/// What a service registration OWNS on every platform -- so the install creates it and grants the
/// service's account access to it, ADDING that account's entry: on Windows `%ProgramData%`'s
/// inherited read for every local user stays on the directory, and what keeps a voter's identity
/// key to its owner is the key FILE, created owner-only whatever the directory grants
/// (`ResolveNodeKey`) -- and what it hands a POSIX service running as an unprivileged
/// account (`STATE_DIRECTORY`), so that service keeps its identity where the machine's node does
/// rather than in its account's home.
/// @param probe Where the environment is read: `%ProgramData%` on Windows.
/// @return The directory, or nothing when the environment names no base for it.
[[nodiscard]] std::optional<std::filesystem::path> MachineWideNodeClusterDirectory(IConfigPathProbe const& probe);

/// Whether this platform's service manager hands a service its state directory in
/// `STATE_DIRECTORY` -- a row of the lookup rather than a platform `#if` at each reader.
/// @return True where the lookup has a service-manager row.
[[nodiscard]] bool ServiceManagerHandsOverStateDirectory() noexcept;

/// Resolve the default state directory into @p cfg, as `ApplyNodeIdentity` writes the id.
/// @param cfg The configuration; its `stateDirectory` is set, or left disengaged when nothing resolves.
/// @param probe Where the environment and the privilege are read.
void ApplyNodeStateDirectory(NodeConfig& cfg, IConfigPathProbe const& probe);

/// This host's DNS names as the start resolved them.
///
/// Not a flag: `ApplyHostNaming` writes it into every configuration this process builds, as
/// `ApplyNodeIdentity` writes the id.
struct NodeHostNames
{
    std::string fqdn;      ///< The name peers are told to dial; empty when withheld.
    std::string dnsSuffix; ///< Where the fleet's SRV record is asked for; empty for none.

    /// The name the start found and WITHHELD, because it reaches only this machine
    /// (`NamesOnlyThisMachine`): `localhost`, a name under `.localhost`, or a loopback address.
    /// Empty when nothing was withheld. Kept for the one condition that names it.
    std::string withheld;

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(NodeHostNames const&, NodeHostNames const&) = default;
};

/// Write @p names into @p cfg: the one writer of `NodeConfig::hostNames`, applied to every
/// configuration a process builds -- the running one, the command line's, and each reload's
/// candidate -- so none of them falls back to a name the others do not.
/// @param cfg The configuration; its `hostNames` is engaged.
/// @param names This machine's names.
void ApplyHostNames(NodeConfig& cfg, NodeHostNames names);

/// Write @p naming's names into @p cfg, through `ApplyHostNames`.
/// @param cfg The configuration; its `hostNames` is engaged.
/// @param naming This machine's names.
void ApplyHostNaming(NodeConfig& cfg, IHostNaming const& naming);

/// How long the start waits for this machine's fully qualified name before it settles for the
/// bare host name.
///
/// **Bounded rather than awaited because nothing serves yet**: this is asked before the node
/// opens a port, so every second it waits is a second the node answers nobody -- and moving the
/// wait behind serving is a later change to the startup order.
///
/// **Six seconds: one lost packet's worth of DNS on POSIX**, where the read is `getaddrinfo` and
/// may ask a resolver: glibc's default per-try timeout (`RES_TIMEOUT`) is five seconds, so six
/// admits the answer to one retransmission. A resolver silent past that is working through the
/// rest of its schedule, and waiting it out buys nothing the bare host name does not.
/// **Windows never waits here**: its read is `GetComputerNameExW`, which reads local
/// configuration and touches no network, so the bound is never reached there.
inline constexpr std::chrono::milliseconds HostNamingBound { 6000 };

/// Reads this machine's names, waiting at most a bound.
class IHostNamingLookup
{
  public:
    IHostNamingLookup() = default;
    IHostNamingLookup(IHostNamingLookup const&) = delete;
    IHostNamingLookup(IHostNamingLookup&&) = delete;
    IHostNamingLookup& operator=(IHostNamingLookup const&) = delete;
    IHostNamingLookup& operator=(IHostNamingLookup&&) = delete;
    virtual ~IHostNamingLookup() = default;

    /// @param bound The longest to wait.
    /// @return The names, or null when they did not arrive within @p bound.
    [[nodiscard]] virtual std::unique_ptr<IHostNaming> ReadWithin(std::chrono::milliseconds bound) = 0;
};

/// The production lookup: the read runs on a thread of its own, and a caller that stops waiting
/// leaves it to finish into state it shares, so nothing it writes outlives what it writes to.
class ThreadedHostNamingLookup final: public IHostNamingLookup
{
  public:
    /// @param read What reads the names; may block for as long as the resolver takes.
    explicit ThreadedHostNamingLookup(std::function<std::unique_ptr<IHostNaming>()> read);

    /// @copydoc IHostNamingLookup::ReadWithin
    [[nodiscard]] std::unique_ptr<IHostNaming> ReadWithin(std::chrono::milliseconds bound) override;

  private:
    std::function<std::unique_ptr<IHostNaming>()> _read;
};

/// How the start came by this machine's names.
///
/// **PRIVATE: persisted and transmitted nowhere**; the explicit `= 0` is `EnumTable`'s anchor.
enum class HostNamingOutcome : std::uint8_t
{
    Resolved = 0,           ///< The platform answered a qualified name.
    Declined,               ///< It answered a placeholder domain, refused for the bare host name.
    TimedOut,               ///< It did not answer within the bound; the bare host name stands in.
    ReachesOnlyThisMachine, ///< What it named reaches only this machine, so no name is offered.
    Last,                   ///< Not an outcome.
};

/// What the start resolved this machine's names to, and how.
struct ResolvedHostNames
{
    NodeHostNames names;             ///< The names to apply.
    HostNamingOutcome outcome;       ///< How they were come by.
    std::string declined;            ///< The placeholder name refused, when `outcome` is `Declined`.
    std::chrono::milliseconds bound; ///< How long the lookup was given.
};

/// Ask @p lookup for this machine's names, waiting at most @p bound.
/// @param lookup Where the names are read.
/// @param bareHostName The host name the platform reports without asking DNS; what stands in
///        when the lookup does not answer.
/// @param bound The longest to wait.
/// @return The names and how they were come by.
[[nodiscard]] ResolvedHostNames ResolveHostNames(IHostNamingLookup& lookup,
                                                 std::string_view bareHostName,
                                                 std::chrono::milliseconds bound);

/// The warning a start logs about how it came by its names.
/// @param resolved What `ResolveHostNames` answered.
/// @return The sentence, or nothing when the names resolved.
[[nodiscard]] std::optional<std::string> HostNamingWarning(ResolvedHostNames const& resolved);

/// What this node tells peers to dial that is a bare host name, and through which flags.
///
/// A name with no domain resolves only where a DNS search list completes it, so on the next
/// office's network it is a confident wrong signal: the node registers and nobody reaches it.
/// @param cfg The configuration, with its host names applied.
/// @return The condition's detail, or nothing when no bare name is in use.
[[nodiscard]] std::optional<std::string> UnqualifiedHostNameInUse(NodeConfig const& cfg);

/// What was confined because this machine's name reaches only itself, and the name.
///
/// A name every machine resolves to ITSELF is withheld rather than offered: nothing is advertised
/// to a scheduler elsewhere, and consensus -- on by default -- is confined to loopback
/// (`ConsensusConfinedToThisMachine`), so the node runs as a fleet of its own and serves this
/// machine alone. That is a vague right answer where offering the name would be a confident
/// wrong one, and this is what makes it visible rather than silent.
/// @param cfg The configuration, with its host names applied.
/// @return The condition's detail, or nothing when no name was withheld or nothing stood down.
[[nodiscard]] std::optional<std::string> NameReachesOnlyThisMachineInUse(NodeConfig const& cfg);

/// Evaluate the host-name rows -- `unqualified-host-name` and `host-name-reaches-only-this-machine`
/// -- raising each while it applies and clearing it otherwise.
///
/// Asked at startup and again after every accepted reload, since `--advertise` is reloadable.
/// @param conditions Where the answer goes.
/// @param cfg The running configuration.
void EvaluateHostNameCondition(NodeConditions& conditions, NodeConfig const& cfg);

} // namespace FastCache::Node

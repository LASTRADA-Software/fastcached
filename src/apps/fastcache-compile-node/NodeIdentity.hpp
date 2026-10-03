// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/StateFiles.hpp>
#include <FastCache/Platform/ProcessExit.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// Where the identity this node runs under came from.
///
/// Three states rather than a `bool`, because they are three different facts an
/// operator acts on differently: a typed id is theirs to change, a recorded one is
/// this machine's history, and a minted one is a machine that has just joined the
/// world. A count or a flag would collapse the last two, and those are exactly the
/// pair that matters -- "this node kept the id it had" and "this node invented one"
/// look identical in every log line that does not say which.
enum class NodeIdentityOrigin : std::uint8_t
{
    Configured, ///< `--node-id`, typed by an operator; recorded so a later start keeps it.
    Recorded,   ///< Read back out of the state directory, where a previous start put it.
    Minted,     ///< Drawn fresh, because the state directory had none.

    Last, ///< Not an origin, and has no sentence: the length of a table keyed by one.
};

/// A node's identity, and how it was arrived at.
struct NodeIdentity
{
    std::string id;                                           ///< What the cluster admits and counts votes against.
    NodeIdentityOrigin origin { NodeIdentityOrigin::Minted }; ///< Where it came from.

    /// The public half of this node's identity key, or absent on a node that holds none (#178).
    ///
    /// Carried beside the id rather than resolved apart from it, because the two are applied to
    /// every configuration together: a reload candidate that received the id and not the key
    /// would hold a self member whose key has changed.
    std::optional<Ed25519PublicKey> publicKey;
};

/// What a node says at startup about where its identity came from.
///
/// A TABLE rather than a conditional chain at the one call site, for the reason every
/// table here exists: a fourth origin is a row. And it lives beside the enum rather
/// than in `main.cpp`, which is in no test target -- the sentence an operator reads
/// after a restart is the one that says whether this machine is still the member the
/// cluster counts, and a sentence nothing can assert is one that drifts.
/// @param origin Where the identity came from.
/// @return The parenthetical, without its brackets.
[[nodiscard]] std::string_view DescribeNodeIdentityOrigin(NodeIdentityOrigin origin) noexcept;

/// The file inside a state directory that records a node's identity.
///
/// Beside the Raft log deliberately: a node IS its state directory. Two nodes on one
/// machine already need two of them, because two Raft logs cannot share a directory,
/// so the identity needs no discriminator that the state does not already force.
inline constexpr std::string_view NodeIdentityFileName = StateFileName(StateFile::Identity);

/// What a new id is written to first, beside `NodeIdentityFileName`, and renamed into place.
inline constexpr std::string_view NodeIdentityReplacementSuffix = ".new";

/// How many hex characters a minted identity carries.
///
/// 128 bits. Long enough that no fleet collides by accident and short enough to read
/// back off a dashboard -- and it is never ABBREVIATED anywhere, which is the rule
/// that decides the length is not a UX question: an abbreviated identifier is a
/// display form, and this project has already paid for one that was compared.
inline constexpr std::size_t MintedNodeIdLength = 32;

// A minted id is one every reader of an id accepts: the summary's codec and the member grammar
// both refuse one past the one id bound.
static_assert(MintedNodeIdLength <= CompileCacheWire::MaxIdBytes, "a minted node id must fit the one id bound");

/// Where this node keeps its state, and why there: `--cluster-dir` when given, else the
/// default `ApplyNodeStateDirectory` resolved.
/// @param cfg The configuration.
/// @return The directory and its origin, or nothing before the default has been resolved (or
///         when none could be).
[[nodiscard]] std::optional<NodeStateDirectoryChoice> ChosenStateDirectory(NodeConfig const& cfg);

/// Where this node's identity and consensus state live.
///
/// **One author of the directory**, which `ConsensusTier::Start` used to be a second of. Two
/// nodes on one machine need two Raft logs and two identities, so they need two directories:
/// `--cluster-dir` for the second, or one privileged and one not, which the default already
/// separates.
///
/// **A precondition, not a fallback**: asked before the default is resolved it throws
/// `std::logic_error` rather than answer a relative path, because every caller WRITES there --
/// an identity minted in the working directory is `System32` under a service. The start
/// resolves the default before anything reaches this, and refuses by name when none resolves.
/// @param cfg The configuration, with its state directory resolved.
/// @return `--cluster-dir` when given, else the resolved default.
[[nodiscard]] std::filesystem::path NodeStateDirectory(NodeConfig const& cfg);

/// What `--print-surfaces` and `--node-status` say about the state directory.
/// @param cfg The configuration.
/// @return `<path> (<why>)`, or why there is none yet.
[[nodiscard]] std::string DescribeNodeStateDirectory(NodeConfig const& cfg);

/// The line `--print-identity` prints after the identity: where it is kept, and why there.
///
/// An unelevated run on Windows keeps a PER-USER identity under `%LOCALAPPDATA%`, which the
/// service -- holding the machine's, under `%ProgramData%` -- never runs as; an operator who
/// admitted that key would admit an identity no machine proves. The lines above it look the
/// same either way, so this one says which.
/// @param cfg The configuration, its state directory resolved.
/// @return `state-directory <path> (<why>)`, ending in a newline.
[[nodiscard]] std::string DescribeIdentityOrigin(NodeConfig const& cfg);

/// Why a node refuses to start with no state directory.
inline constexpr std::string_view NoStateDirectoryRefusal =
    "this node has no state directory to keep its identity in: no --cluster-dir was given, and the platform's "
    "default could not be derived because the variable it comes from (ProgramData or LOCALAPPDATA on Windows, "
    "XDG_STATE_HOME or HOME elsewhere) is unset, empty or not an absolute path. Name --cluster-dir=<dir>";

/// Why this invocation must not go on, when it has no state directory and would write there.
///
/// **Only an invocation that writes there is refused** (`NodeIdentityNeed`). `--print-surfaces`,
/// the one-shot cluster verbs and `--uninstall-service` write nothing into it, and the last is the
/// recovery an operator reaches for when the configuration is already wrong -- refusing it for a
/// directory it would never touch would leave them nothing to reach for.
/// @param cfg The configuration, with the platform's default applied.
/// @return `NoStateDirectoryRefusal`, or nothing when there is a directory or no need for one.
[[nodiscard]] std::optional<std::string_view> StateDirectoryRefusal(NodeConfig const& cfg);

/// Whether this invocation needs an identity, and may write one.
///
/// Two states, and the second is the one that must not be reached by accident:
/// resolving MINTS when there is nothing recorded, which creates a directory and a
/// file. `--print-surfaces` prints a worksheet, `--cluster-status` asks somebody
/// else, and `--uninstall-service` removes a registration; none of them is entitled
/// to leave state behind, and a flag whose own comment says it "opens nothing and
/// changes nothing" must keep saying so.
enum class IdentityNeed : std::uint8_t
{
    None, ///< Nothing here will run holding an identity, or register a service that will.
    Mint, ///< This invocation will run holding an identity key, or register a service that will.
};

/// Whether @p cfg describes an invocation that needs a resolved identity.
/// @param cfg The parsed configuration.
/// @return What this invocation is entitled to do about an identity.
[[nodiscard]] IdentityNeed NodeIdentityNeed(NodeConfig const& cfg) noexcept;

/// What went wrong resolving a node's identity.
///
/// Private: never transmitted or persisted.
enum class NodeIdentityFault : std::uint8_t
{
    Unreadable,   ///< The identity file is there, and could not be read.
    Empty,        ///< It was read, and holds nothing.
    NotText,      ///< It was read, and does not hold text.
    DrawFailed,   ///< The operating system's generator refused the bits a new identity needs.
    CreateFailed, ///< The state directory could not be created.
    WriteFailed,  ///< The identity could not be recorded.
    Last,         ///< Not a fault, and has no row: the length of a table keyed by one.
};

/// A refusal, and the sentence an operator reads.
struct NodeIdentityRefusal
{
    NodeIdentityFault fault { NodeIdentityFault::Unreadable }; ///< What went wrong, for a caller that decides on it.
    std::string message;                                       ///< What an operator is told, naming the file.
};

/// One identity fault, and the step of a start it gives up at.
struct NodeIdentityFaultStage
{
    NodeIdentityFault fault { NodeIdentityFault::Unreadable }; ///< The fault.
    StartStage stage { StartStage::NodeIdentityIo };           ///< Which decides how a start AND a command end.
};

/// Every identity fault, by `StartStageRows`' rule: a verdict on the bytes of an identity file that
/// was READ is `NodeIdentity` (refused, 78); a read, a directory, a write or a draw that failed is
/// `NodeIdentityIo` (a failure, restarted), whatever its errno.
///
/// A ONE-SHOT command that mints -- `--print-identity`, `--install-service` -- ends by the same stage
/// (`EndingOf`): an I/O arm is transient (1), a verdict a decision (2).
inline constexpr auto NodeIdentityFaultStages = EnumTable<NodeIdentityFault, NodeIdentityFaultStage> { {
    { .fault = NodeIdentityFault::Unreadable, .stage = StartStage::NodeIdentityIo },
    { .fault = NodeIdentityFault::Empty, .stage = StartStage::NodeIdentity },
    { .fault = NodeIdentityFault::NotText, .stage = StartStage::NodeIdentity },
    { .fault = NodeIdentityFault::DrawFailed, .stage = StartStage::NodeIdentityIo },
    { .fault = NodeIdentityFault::CreateFailed, .stage = StartStage::NodeIdentityIo },
    { .fault = NodeIdentityFault::WriteFailed, .stage = StartStage::NodeIdentityIo },
} };
static_assert(RowsInEnumeratorOrder(NodeIdentityFaultStages, [](NodeIdentityFaultStage const& row) { return row.fault; }),
              "every NodeIdentityFault needs a stage, at its own index");

/// The step of a start @p fault gives up at.
/// @param fault What went wrong with the identity.
/// @return `NodeIdentity` for a verdict on bytes read, `NodeIdentityIo` for an I/O arm.
[[nodiscard]] constexpr StartStage StageOf(NodeIdentityFault fault) noexcept
{
    return NodeIdentityFaultStages[static_cast<std::size_t>(fault)].stage;
}

/// How a one-shot command that stopped on @p fault ended.
/// @param fault What went wrong with the identity.
/// @return `Failed` for an I/O arm, `Declined` for a verdict on the bytes read.
[[nodiscard]] constexpr CommandEnding EndingOf(NodeIdentityFault fault) noexcept
{
    return EndingOf(StageOf(fault));
}

/// Read this node's recorded identity, minting one if the directory holds none.
///
/// **Minted FRESH, never derived from the machine**, and that is a decision against
/// the obvious alternative rather than an omission. Deriving from `/etc/machine-id`
/// or `MachineGuid` buys an identity that survives losing the state directory -- and
/// that is precisely the outcome to avoid: the state directory is the Raft log and
/// the vote record, and a node that comes back under its old identity having
/// forgotten which term it voted in is `--cluster-dir`'s own documented hazard, two
/// leaders in one term, made automatic. It also makes two machines cloned from one
/// image mint the SAME id the moment neither has state yet, which is the silent
/// duplicate the derivation was chosen to avoid.
///
/// A typed `--node-id` is RECORDED, so an operator names it once and every later
/// start finds it. That is what makes the flag an override rather than a thing to
/// keep typing, and it is what a re-image must not be able to change silently.
///
/// A recorded value that is empty or is not text is a REFUSAL, never a re-mint: an
/// identity this cluster has already admitted must not be replaced because a file
/// was hard to read. Nor is a file that is there and cannot be read taken for an
/// absent one: that is refused as well, as the I/O failure it is. Whoever meets that message can delete the file
/// deliberately and take the consequences knowingly.
///
/// And so is a mint whose bits cannot be drawn (#1527): nothing is written, and the
/// caller refuses to start rather than running under an identity drawn from anywhere
/// weaker.
/// @param stateDirectory Where consensus keeps its durable state.
/// @param configured What `--node-id` said, or empty.
/// @param random Where a minted identity's bits come from.
/// @return The identity and how it was arrived at, or why it could not be.
[[nodiscard]] std::expected<NodeIdentity, NodeIdentityRefusal> ResolveNodeIdentity(
    std::filesystem::path const& stateDirectory, std::string_view configured, ISecureRandom& random);

/// The id recorded in @p stateDirectory, read and never minted.
///
/// For a sentence that has to name this node before its identity is resolved -- a refusal of its
/// key, which is judged first. Nothing when there is no file, or one `ResolveNodeIdentity` would
/// refuse: the sentence then names the file instead of a value it cannot vouch for.
/// @param stateDirectory Where the node keeps its state.
/// @return The id, or nothing.
[[nodiscard]] std::optional<std::string> RecordedNodeId(std::filesystem::path const& stateDirectory);

/// Draw a fresh identity.
///
/// Exposed so a test can drive it against a scripted source rather than observing it
/// through a file, and because "two draws differ" is a property of this function
/// rather than of the resolver around it.
///
/// **From the operating system's generator, never a seeded engine** (#1527). "Two
/// machines cloned from one image mint the SAME id" is the collision this whole file
/// exists to prevent, and an engine seeded from `std::random_device` reproduces it on
/// any host where that device answers a constant -- measured at zero for 57% of draws
/// on the one #1507 was found on. It also has to be UNGUESSABLE, which an engine never
/// was: an open enrollment window hands the key to whoever names an approved id, and
/// the operator documentation calls a minted id a gate for exactly that reason. A draw
/// that fails is a refusal, not a fallback.
/// @param random Where the bits come from.
/// @return `MintedNodeIdLength` lowercase hex characters, or why none could be drawn.
[[nodiscard]] std::expected<std::string, SecureRandomError> MintNodeId(ISecureRandom& random);

/// What a node holding an identity is, for what `--print-identity` tells its operator to type.
///
/// **PRIVATE: persisted and transmitted nowhere.**
enum class IdentityRole : std::uint8_t
{
    Member, ///< Runs consensus: admitted with `--cluster-admit`, or by enrollment.
    Worker, ///< Runs none: admitted with `--cluster-admit-worker` or `--enroll-from`.
};

/// What `--print-identity` prints (#178): one `name value` line per fact, the names the ones
/// `fastcache-cli node` reports under.
///
/// Pure, and apart from the verb, so what an operator copies into the other members'
/// command lines is asserted rather than eyeballed: the `cluster-admit` line is the token
/// `--cluster-admit` parses back into this very member, key included.
/// @param id The node's id; empty on a node that runs no consensus and was named none, which
///        prints no `node-id` line and no token.
/// @param key Its public key, shown whole.
/// @param dialAddress Where its peers dial it, when this configuration says; without one the
///        token cannot be written and is left out rather than guessed.
/// @param role What the node IS (#178): a consensus MEMBER prints the `cluster-admit` token an
///        operator types on a member, a WORKER the `cluster-admit-worker` token --
///        the one line a worker's admission needs, spelled the way the flag parses it.
/// @return The lines, each ending in a newline.
[[nodiscard]] std::string DescribeIdentity(std::string_view id,
                                           Ed25519PublicKey const& key,
                                           std::optional<std::string> const& dialAddress,
                                           IdentityRole role);

/// Put a resolved identity into a configuration: the id and the key this node runs as. Its
/// member entry is the formation's (`BootstrapMembersOf`), built from these and the address
/// `ConsensusDialAddressOf` derives.
///
/// Applied at the START and again to every RELOAD candidate, through the one
/// function, for the reason `AssembleEffectiveConfig` is a required argument to the
/// reloader: a candidate rebuilt without it holds an empty `--node-id`, which is an
/// unreloadable field that has changed, so every reload would be refused by name.
///
/// The key reaches the configuration here too (#178), and before the id: a node that runs
/// no consensus has no id and may still hold a key.
/// @param cfg The configuration to complete.
/// @param identity What this node runs as.
void ApplyNodeIdentity(NodeConfig& cfg, NodeIdentity const& identity);

} // namespace FastCache::Node

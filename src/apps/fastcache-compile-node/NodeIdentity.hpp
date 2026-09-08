// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
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
inline constexpr std::string_view NodeIdentityFileName = "node-id";

/// How many hex characters a minted identity carries.
///
/// 128 bits. Long enough that no fleet collides by accident and short enough to read
/// back off a dashboard -- and it is never ABBREVIATED anywhere, which is the rule
/// that decides the length is not a UX question: an abbreviated identifier is a
/// display form, and this project has already paid for one that was compared.
inline constexpr std::size_t MintedNodeIdLength = 32;

/// Where this node's consensus state lives.
///
/// **One author of the default**, which `ConsensusTier::Start` used to be a second
/// of. It also had to CHANGE at #1024: the default was
/// `fastcache-cluster/<node-id>`, and an identity read out of the state directory
/// cannot name the directory it is read from. The discriminator it supplied is not
/// lost -- two nodes on one machine need two Raft logs, so they need two directories
/// whatever they are called.
/// @param cfg The parsed configuration.
/// @return `--cluster-dir` when given, else the built-in default.
[[nodiscard]] std::filesystem::path NodeStateDirectory(NodeConfig const& cfg);

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
    None, ///< Nothing here will run or register consensus.
    Mint, ///< This invocation will run consensus, or register a service that will.
};

/// Whether @p cfg describes an invocation that needs a resolved identity.
/// @param cfg The parsed configuration.
/// @return What this invocation is entitled to do about an identity.
[[nodiscard]] IdentityNeed NodeIdentityNeed(NodeConfig const& cfg) noexcept;

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
/// was hard to read. Whoever meets that message can delete the file deliberately and
/// take the consequences knowingly.
/// @param stateDirectory Where consensus keeps its durable state.
/// @param configured What `--node-id` said, or empty.
/// @param random Where a minted identity's bits come from.
/// @return The identity and how it was arrived at, or why it could not be.
[[nodiscard]] std::expected<NodeIdentity, std::string> ResolveNodeIdentity(std::filesystem::path const& stateDirectory,
                                                                           std::string_view configured,
                                                                           IRandomSource& random);

/// Draw a fresh identity.
///
/// Exposed so a test can drive it against a scripted source rather than observing it
/// through a file, and because "two draws differ" is a property of this function
/// rather than of the resolver around it.
/// @param random Where the bits come from.
/// @return `MintedNodeIdLength` lowercase hex characters.
[[nodiscard]] std::string MintNodeId(IRandomSource& random);

/// Put a resolved identity into a configuration, including this node's own peer entry.
///
/// **Both halves, in one function, because a derived identity is unusable without the
/// second.** `--raft-peer=<id>=<host>:<port>` cannot be typed for an id nobody typed,
/// and a node that names no member of its own configuration is refused -- so
/// `--raft-self` states the address and this is where it becomes a member.
///
/// Applied at the START and again to every RELOAD candidate, through the one
/// function, for the reason `AssembleEffectiveConfig` is a required argument to the
/// reloader: a candidate rebuilt without it holds an empty `--node-id`, which is an
/// unreloadable field that has changed, so every reload would be refused by name.
/// @param cfg The configuration to complete.
/// @param identity What this node runs as.
void ApplyNodeIdentity(NodeConfig& cfg, NodeIdentity const& identity);

} // namespace FastCache::Node

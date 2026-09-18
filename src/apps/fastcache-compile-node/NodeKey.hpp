// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/SecureBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// The file inside a state directory that holds this node's identity key (#178).
///
/// Beside `node-id` and the Raft log, for `NodeIdentityFileName`'s reason: a node IS its
/// state directory, so what proves which node this is lives where everything else that
/// says so does. A wiped directory is a new node -- a new id and a new key together.
inline constexpr std::string_view NodeKeyFileName = "node-key";

/// Bytes in a key file: a four-byte magic, a two-byte version, the 32-byte seed and the
/// 32-byte public key it derives.
///
/// The public key is stored although the seed determines it, and that is the file's own
/// integrity check rather than a cache: a flipped bit in a bare seed is a DIFFERENT valid
/// key, which a reader would adopt silently -- a node becoming a stranger to its own cluster
/// with nothing anywhere saying why. Stored beside it, the pair must agree, and a file whose
/// two halves do not is refused as damaged.
inline constexpr std::size_t NodeKeyFileBytes = 4 + 2 + Ed25519SeedBytes + Ed25519PublicKeyBytes;

/// Where this node's identity key came from.
///
/// Two states, both of which an operator acts on: a recorded key is the machine the cluster
/// may already have admitted, and a minted one is a machine it has never seen. Private: never
/// transmitted or persisted.
enum class NodeKeyOrigin : std::uint8_t
{
    Recorded, ///< Read back out of the state directory, where a previous start minted it.
    Minted,   ///< Drawn fresh, because the state directory had none.
    Last,     ///< Not an origin, and has no sentence: the length of a table keyed by one.
};

/// What a node says at startup about where its key came from.
/// @param origin Where the key came from.
/// @return The parenthetical, without its brackets.
[[nodiscard]] std::string_view DescribeNodeKeyOrigin(NodeKeyOrigin origin) noexcept;

/// Why a node could not have its identity key.
///
/// **Private: never transmitted or persisted.** One value per REMEDY, which is why a file
/// that is too short and one that is not a key file at all are apart: the first is a write
/// that did not finish and the second is somebody else's file in this node's directory.
///
/// None of these is ever answered by minting. Only an ABSENT file mints -- a key the cluster
/// may have admitted must not be replaced because it was hard to read, which is the
/// `node-id` rule arriving at the file that proves the id.
enum class NodeKeyFault : std::uint8_t
{
    Unreadable,    ///< The file is there, and could not be opened or read.
    Truncated,     ///< Shorter than the format: a write that did not finish.
    NotAKeyFile,   ///< It does not open with a key file's magic.
    ForeignFormat, ///< A key file another build laid out differently: intact, and not this build's.
    Damaged,       ///< Longer than the format, or its public key does not derive from its secret.
    DrawFailed,    ///< The operating system's generator refused the bits a new key needs.
    WriteFailed,   ///< The state directory or the file could not be created or written.
    Last,          ///< Not a fault, and has no row: the length of a table keyed by one.
};

/// A refusal, and the sentence an operator reads.
struct NodeKeyRefusal
{
    NodeKeyFault fault { NodeKeyFault::Unreadable }; ///< What went wrong, for a caller that decides on it.
    std::string message;                             ///< What an operator is told, naming the file.
};

/// A node's identity key, and how it was arrived at.
struct NodeKey
{
    Ed25519KeyPair pair;                            ///< The key. Its secret half never leaves this process.
    NodeKeyOrigin origin { NodeKeyOrigin::Minted }; ///< Where it came from.
};

/// Whether @p cfg describes a node that holds an identity key.
///
/// **A node holds a key when it has a state directory to hold it in**: it runs consensus,
/// whose state directory always exists (named, or the built-in default a consensus node has
/// always used), or it names `--cluster-dir`. A node with neither has nowhere a key would
/// SURVIVE -- its working directory is `/run` under the packaged unit and `System32` under a
/// Windows service -- and a key minted afresh at every boot is a new machine at every boot,
/// which is worse than none: the cluster would be asked to admit a stranger each time. So it
/// holds none, says so at startup, and reports no key.
///
/// Asked only of a node that is about to SERVE: every verb that answers and exits returns
/// before the start path reaches the key, so none of them can create a directory as a side
/// effect of being asked a question.
/// @param cfg The resolved configuration.
/// @return True when this node holds a key.
[[nodiscard]] bool HoldsNodeKey(NodeConfig const& cfg) noexcept;

/// Where @p cfg's identity key lives.
///
/// The one author of the path, asked by the start that reads it and by the secret-exposure
/// row that watches it, so the two cannot come to name different files.
/// @param cfg The resolved configuration.
/// @return `<state directory>/node-key`, or empty when `HoldsNodeKey` is false.
[[nodiscard]] std::filesystem::path NodeKeyPath(NodeConfig const& cfg);

/// The bytes a key file holds.
///
/// Pure, and exposed so the format is tested apart from the filesystem.
/// @param seed The 32-byte seed.
/// @param publicKey The public key the seed derives.
/// @return The file's contents, in wiped storage.
[[nodiscard]] SecureByteBuffer EncodeNodeKeyFile(std::span<std::byte const> seed, Ed25519PublicKey const& publicKey);

/// Read a key file's bytes back into a key pair.
///
/// The header is judged FIRST, before any length: the magic and the version sit at the same
/// offsets in every format and nothing else does, so a file another build wrote is refused
/// as `ForeignFormat` -- intact, and not this build's -- rather than failing a length check
/// drawn from this build's layout and being reported as damage.
/// @param bytes The file's contents.
/// @param path The file, for the sentence.
/// @return The key pair, or why these bytes are not one.
[[nodiscard]] std::expected<Ed25519KeyPair, NodeKeyRefusal> DecodeNodeKeyFile(std::span<std::byte const> bytes,
                                                                              std::filesystem::path const& path);

/// Read this node's identity key, minting one when the state directory holds none (#178).
///
/// **Only an ABSENT file mints.** A file that is there and cannot be read, is too short, is
/// not a key file, was laid out by another build or does not agree with itself is REFUSED by
/// name and left exactly as it is: re-minting would replace an identity the cluster may have
/// admitted, silently, on a machine whose only symptom is that it has become a stranger.
/// Whoever meets the refusal can remove the file deliberately and take the consequence --
/// a new identity -- knowingly.
///
/// **Absent is asked of the OPEN, never of `exists()`**: a stat that cannot answer read as
/// "absent" would mint over a key this machine holds. And the mint is an EXCLUSIVE create
/// with nothing in front of it, `StoreClusterKey`'s idiom for the same reason: a file that
/// appeared a moment ago is refused, never truncated.
///
/// The secret is drawn from @p random before anything is created, so a draw this host cannot
/// make leaves no directory and no file behind. On POSIX the file is created mode 0600 in the
/// same call that creates it, so there is no moment at which the secret is readable by
/// anybody else. On Windows it takes the state directory's access list, which is what the
/// secret-exposure row then reports on.
/// @param stateDirectory Where this node keeps its state.
/// @param random Where a minted key's seed comes from.
/// @return The key and how it was arrived at, or why there is none.
[[nodiscard]] std::expected<NodeKey, NodeKeyRefusal> ResolveNodeKey(std::filesystem::path const& stateDirectory,
                                                                    ISecureRandom& random);

/// Resolve the key @p cfg holds, or nothing when it holds none.
///
/// `HoldsNodeKey` and `ResolveNodeKey` in one call, so the start asks ONE question and a node
/// that holds no key provably touches nothing: no directory is created and nothing is drawn.
/// @param cfg The resolved configuration.
/// @param random Where a minted key's seed comes from.
/// @return The key, DISENGAGED when this node holds none, or why it could not be had.
[[nodiscard]] std::expected<std::optional<NodeKey>, NodeKeyRefusal> ResolveNodeKeyFor(NodeConfig const& cfg,
                                                                                      ISecureRandom& random);

/// What a node that holds no key says at startup, once.
///
/// Stated rather than left to be inferred from an absent line, because "this node has no
/// identity" and "this node never got as far as looking" read identically in a log that says
/// nothing -- and the remedy is one flag.
inline constexpr std::string_view NoNodeKeySentence =
    "no identity key: this node runs no consensus and names no --cluster-dir, so it has no state directory a key "
    "would survive a restart in; name --cluster-dir to give it one";

} // namespace FastCache::Node

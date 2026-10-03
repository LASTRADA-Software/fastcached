// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/StateFiles.hpp>
#include <FastCache/Platform/FileTrust.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

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
inline constexpr std::string_view NodeKeyFileName = StateFileName(StateFile::Key);

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
    Unreadable,          ///< The file is there, and could not be opened or read.
    Truncated,           ///< Shorter than the format: a write that did not finish.
    NotAKeyFile,         ///< It does not open with a key file's magic.
    ForeignFormat,       ///< A key file another build laid out differently: intact, and not this build's.
    Damaged,             ///< Longer than the format, or its public key does not derive from its secret.
    Exposed,             ///< Accounts other than its owner may read it, so the key it holds may be disclosed.
    ForeignOwner,        ///< Another account owns it, or another file of the state directory (`NodeStateFiles`).
    OpenDirectory,       ///< Other accounts may create or delete entries in the state directory.
    LinkEntry,           ///< An entry of the state directory is a link, which this node never writes.
    DrawFailed,          ///< The operating system's generator refused the bits a new key needs.
    WriteFailed,         ///< The state directory or the file could not be created or written.
    Unprotectable,       ///< A new key file could not be made readable by its owner alone, so nothing was written.
    UnknownEntry,        ///< An entry of the state directory no row names, whoever wrote it (`NodeStateFiles`).
    OthersMayWrite,      ///< Accounts other than its owner may write a file of the state directory (`NodeStateFiles`).
    NotARegularFile,     ///< The key's name is taken by something that is not a regular file: a FIFO, a device, ...
    WritersUndetermined, ///< Whether others may write a file of the state directory could not be asked (`NodeStateFiles`).
    Last,                ///< Not a fault, and has no row: the length of a table keyed by one.
};

/// A refusal, and the sentence an operator reads.
struct NodeKeyRefusal
{
    NodeKeyFault fault { NodeKeyFault::Unreadable }; ///< What went wrong, for a caller that decides on it.
    std::string message;                             ///< What an operator is told, naming the file.
};

/// One key fault, and the step of a start it gives up at.
struct NodeKeyFaultStage
{
    NodeKeyFault fault { NodeKeyFault::Unreadable }; ///< The fault.
    StartStage stage { StartStage::IdentityKeyIo };  ///< Which decides how a start AND a command end.
};

/// Every key fault, by `StartStageRows`' rule: a verdict on the bytes of a key file that was
/// READ is `IdentityKey` (refused, 78); an open, a read, a write or a draw that failed is
/// `IdentityKeyIo` (a failure, restarted), whatever its errno.
///
/// A verdict on who may reach the file or its state directory -- an access list, an owner, a link,
/// an entry no row names -- was read from the filesystem and is judged the same way at the next
/// start, so it is `IdentityKey` too. An access list that could not be SET on a new file, or a
/// question about writers the filesystem would not answer, is an arm that failed: `IdentityKeyIo`.
///
/// A ONE-SHOT command that mints -- `--print-identity` -- ends by the same stage (`EndingOf`): an I/O
/// arm is transient (1), a verdict a decision (2).
inline constexpr auto NodeKeyFaultStages = EnumTable<NodeKeyFault, NodeKeyFaultStage> { {
    { .fault = NodeKeyFault::Unreadable, .stage = StartStage::IdentityKeyIo },
    { .fault = NodeKeyFault::Truncated, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::NotAKeyFile, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::ForeignFormat, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::Damaged, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::Exposed, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::ForeignOwner, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::OpenDirectory, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::LinkEntry, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::DrawFailed, .stage = StartStage::IdentityKeyIo },
    { .fault = NodeKeyFault::WriteFailed, .stage = StartStage::IdentityKeyIo },
    { .fault = NodeKeyFault::Unprotectable, .stage = StartStage::IdentityKeyIo },
    { .fault = NodeKeyFault::UnknownEntry, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::OthersMayWrite, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::NotARegularFile, .stage = StartStage::IdentityKey },
    { .fault = NodeKeyFault::WritersUndetermined, .stage = StartStage::IdentityKeyIo },
} };
static_assert(RowsInEnumeratorOrder(NodeKeyFaultStages, [](NodeKeyFaultStage const& row) { return row.fault; }),
              "every NodeKeyFault needs a stage, at its own index");

/// The step of a start @p fault gives up at.
/// @param fault What went wrong with the key.
/// @return `IdentityKey` for a verdict on bytes read, `IdentityKeyIo` for an I/O arm.
[[nodiscard]] constexpr StartStage StageOf(NodeKeyFault fault) noexcept
{
    return NodeKeyFaultStages[static_cast<std::size_t>(fault)].stage;
}

/// How a one-shot command that stopped on @p fault ended.
/// @param fault What went wrong with the key.
/// @return `Failed` for an I/O arm, `Declined` for a verdict on the bytes read.
[[nodiscard]] constexpr CommandEnding EndingOf(NodeKeyFault fault) noexcept
{
    return EndingOf(StageOf(fault));
}

/// A node's identity key, and how it was arrived at.
struct NodeKey
{
    Ed25519KeyPair pair;                            ///< The key. Its secret half never leaves this process.
    NodeKeyOrigin origin { NodeKeyOrigin::Minted }; ///< Where it came from.
};

/// Who may read a key file, asked of it and established on it.
///
/// **The seam `ResolveNodeKey` reaches the filesystem's access control through**, so the two
/// decisions it makes over that -- a key file others can read is refused, and a new one is
/// protected before the secret is written into it -- are tested against a scripted answer
/// rather than against whatever the test host's permissions happen to be. The production
/// guard is `FileTrustNodeKeyGuard`.
class INodeKeyFileGuard
{
  public:
    INodeKeyFileGuard() = default;
    INodeKeyFileGuard(INodeKeyFileGuard const&) = delete;
    INodeKeyFileGuard(INodeKeyFileGuard&&) = delete;
    INodeKeyFileGuard& operator=(INodeKeyFileGuard const&) = delete;
    INodeKeyFileGuard& operator=(INodeKeyFileGuard&&) = delete;
    virtual ~INodeKeyFileGuard() = default;

    /// Who other than its owner and the administrators may read @p file.
    /// @param file An existing key file.
    /// @return The exposure, `Undetermined` when the platform would not say.
    [[nodiscard]] virtual SecretExposure ExposureOf(std::filesystem::path const& file) = 0;

    /// Who owns @p file: a key file nobody else can READ may still be one somebody else WROTE.
    /// @param file An existing key file.
    /// @return Its owner.
    [[nodiscard]] virtual FileOwner OwnerOf(std::filesystem::path const& file) = 0;

    /// Who may create or delete entries in @p directory: whoever may is whoever decides which
    /// key file this node finds there, and whether it finds one at all.
    /// @param directory The state directory.
    /// @return Who can write in it.
    [[nodiscard]] virtual DirectoryWriters WritersOf(std::filesystem::path const& directory) = 0;

    /// Whether @p entry is a link rather than a file or directory of its own. The node writes
    /// none into its state directory, so one there was put there by somebody else, pointing
    /// wherever they chose -- and an owner read through it would answer for the target.
    /// @param entry An entry of the state directory.
    /// @return True for a link.
    [[nodiscard]] virtual bool IsLink(std::filesystem::path const& entry) = 0;

    /// Whether an account other than @p entry's owner and the administrators may WRITE it: a file
    /// somebody else can rewrite holds contents they chose, whoever owns it.
    /// @param entry An entry of the state directory.
    /// @return True when others may, false when they may not, and the platform's error when it
    ///         would not say -- which the caller decides about, never this seam.
    [[nodiscard]] virtual std::expected<bool, std::error_code> OthersMayWrite(std::filesystem::path const& entry) = 0;

    /// Make @p file readable by its owner and the administrators alone.
    /// @param file A key file this process has just created, still empty.
    /// @return Who may read it afterwards, as READ BACK rather than as the call reported.
    [[nodiscard]] virtual SecretExposure Protect(std::filesystem::path const& file) = 0;
};

/// The production guard: `Platform/FileTrust`'s secrecy half, and nothing of its own.
class FileTrustNodeKeyGuard final: public INodeKeyFileGuard
{
  public:
    /// @copydoc INodeKeyFileGuard::ExposureOf
    [[nodiscard]] SecretExposure ExposureOf(std::filesystem::path const& file) override;

    /// @copydoc INodeKeyFileGuard::OwnerOf
    [[nodiscard]] FileOwner OwnerOf(std::filesystem::path const& file) override;

    /// @copydoc INodeKeyFileGuard::WritersOf
    [[nodiscard]] DirectoryWriters WritersOf(std::filesystem::path const& directory) override;

    /// @copydoc INodeKeyFileGuard::IsLink
    [[nodiscard]] bool IsLink(std::filesystem::path const& entry) override;

    /// @copydoc INodeKeyFileGuard::OthersMayWrite
    [[nodiscard]] std::expected<bool, std::error_code> OthersMayWrite(std::filesystem::path const& entry) override;

    /// @copydoc INodeKeyFileGuard::Protect
    [[nodiscard]] SecretExposure Protect(std::filesystem::path const& file) override;
};

/// When a key file's exposure is judged: the rule differs in one cell, deliberately.
///
/// **Private: never transmitted or persisted.**
enum class KeyMoment : std::uint8_t
{
    Mint, ///< A key this node is about to write.
    Read, ///< A key a previous start wrote.
    Last,
};

/// Does @p exposure keep a file from holding this node's identity key, at @p moment?
///
/// One table with two columns, and they differ in `Undetermined` alone. At MINT nothing exists
/// yet, and writing a secret the protection could not be read back on would make the order
/// property -- protected before written -- vacuous on exactly that filesystem, so it is
/// refused and costs one `--cluster-dir`. At READ refusing it would stop a node that ran
/// yesterday on an ordinary deployment (`SecretExposure` defines it as reported, never refused),
/// and the secret-exposure row reports it at every start.
/// @param exposure What the guard found.
/// @param moment Whether the key is being minted or read back.
/// @return True when the file must not hold the key.
[[nodiscard]] bool RefusesKeyExposure(SecretExposure exposure, KeyMoment moment) noexcept;

/// Does a state directory whose entries @p writers may create or delete keep this node from
/// trusting a key file in it, or minting one there?
/// @param writers What the guard found.
/// @return True when the directory is refused.
[[nodiscard]] bool RefusesKeyDirectory(DirectoryWriters writers) noexcept;

/// Where @p cfg's identity key lives.
///
/// The one author of the path, asked by the start that reads it and by the secret-exposure
/// row that watches it, so the two cannot come to name different files.
/// **Every node holds one**, because every node has a state directory now: `--cluster-dir`,
/// or the default `ApplyNodeStateDirectory` resolved. A key minted afresh at every boot would
/// be a new machine at every boot, which is why a node with nowhere to keep one used to hold
/// none; the default directory is what ended that case.
/// @param cfg The resolved configuration.
/// @return `<state directory>/node-key`, or empty before the default state directory has been
///         resolved -- a watcher asked that early has no file to watch yet.
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
/// with nothing in front of it, for the same reason: a file that appeared a moment ago is
/// refused, never truncated.
///
/// The secret is drawn from @p random before anything is created, so a draw this host cannot
/// make leaves no directory and no file behind. On POSIX the file is created mode 0600 in the
/// same call that creates it, so there is no moment at which the secret is readable by
/// anybody else.
///
/// **And the key file is its owner's alone WHOEVER created the directory it is in.** A new
/// file is created ALREADY protected -- readable by its owner and the administrators only from
/// its first instant, with nothing else able to open it while it is being written
/// (`CreateStateFile`, the key's row) -- because access is decided at OPEN: a file protected after
/// it was created would still be readable through any handle opened in between. The
/// protection is then read back through @p guard before the secret is written, and a file
/// whose protection does not read back is removed while it is still empty and the mint is
/// refused (`Unprotectable`).
///
/// **And the key is this node's only if nobody else could have PUT it there.** A state
/// directory other accounts may add to or delete from is refused (`OpenDirectory`), with the
/// command that restricts it -- one this node creates itself is created its owner's alone --
/// and a key file another account owns is refused (`ForeignOwner`), naming the owner: it holds
/// a key they chose, and a node that adopted it would prove itself with a secret they keep. So
/// is every other file of the state directory the node acts on (`RefuseForeignStateFiles`),
/// asked here because every path that acts on the directory resolves the key first -- and asked
/// as well by the two that READ it earlier, the start's formation record and enrollment's
/// consensus history. A
/// key file that is there and that others may read is refused (`Exposed`) and left untouched,
/// never tightened in place: whether the key it holds was read in the meantime is a question
/// only the operator can answer, and the refusal says what each answer costs.
/// @param stateDirectory Where this node keeps its state.
/// @param random Where a minted key's seed comes from.
/// @param guard Who may read the key file, asked and established.
/// @return The key and how it was arrived at, or why there is none.
[[nodiscard]] std::expected<NodeKey, NodeKeyRefusal> ResolveNodeKey(std::filesystem::path const& stateDirectory,
                                                                    ISecureRandom& random,
                                                                    INodeKeyFileGuard& guard);

/// Judge the state directory before anything in it is read: WHO may write in it (`OpenDirectory`,
/// carrying the key's own exposure question when the key is readable by others, since the
/// directory's remedy would erase that evidence), then WHO wrote each entry
/// (`RefuseForeignStateFiles`). Every path that reads the directory asks this first: the key
/// resolution, the start's formation record, enrollment's consensus history.
/// @param stateDirectory Where this node keeps its state. An absent directory is not judged.
/// @param guard Who may write in it, who owns each entry, and who may read the key.
/// @return Nothing, or the refusal.
[[nodiscard]] std::expected<void, NodeKeyRefusal> JudgeStateDirectory(std::filesystem::path const& stateDirectory,
                                                                      INodeKeyFileGuard& guard);

/// Resolve the key @p cfg holds, in its state directory.
/// @param cfg The resolved configuration; its state directory must be resolved
///        (`NodeStateDirectory`'s precondition).
/// @param random Where a minted key's seed comes from.
/// @param guard Who may read the key file, asked and established.
/// @return The key, or why it could not be had.
[[nodiscard]] std::expected<NodeKey, NodeKeyRefusal> ResolveNodeKeyFor(NodeConfig const& cfg,
                                                                       ISecureRandom& random,
                                                                       INodeKeyFileGuard& guard);

} // namespace FastCache::Node
